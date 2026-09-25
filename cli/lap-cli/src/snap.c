#include "snap.h"

#include "json.h"
#include "sha256.h"

static void snap_path(const Repo *r, const char *rel, char *out,
                      size_t outsz) {
    snprintf(out, outsz, "%s/snapshots/%s.jsonl", r->lapdir, rel);
}

static bool snap_append(const Repo *r, Arena *a, const char *rel, int64_t at,
                        bool eof_nl, const char *content, size_t len) {
    char path[LAP_PATH_MAX];
    snap_path(r, rel, path, sizeof path);
    char parent[LAP_PATH_MAX];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    *slash = '\0';
    if (!plat_mkdirs(parent))
        return false;
    StrBuf sb;
    sb_init(&sb, a);
    sb_printf(&sb, "{\"at\":%lld,\"eof_nl\":%s,\"content\":", (long long)at,
              eof_nl ? "true" : "false");
    json_escape(&sb, content, len);
    sb_puts(&sb, "}\n");
    return plat_append_file_sync(path, sb.data, sb.len);
}

/* Nearest snapshot with at <= upto (or any, when upto < 0). */
static bool snap_nearest(Arena *a, const Repo *r, const char *rel,
                         int64_t upto, int64_t *at, Lines *state) {
    char path[LAP_PATH_MAX];
    snap_path(r, rel, path, sizeof path);
    char *data;
    size_t len;
    if (!plat_read_file_max(a, path, &data, &len, (size_t)-1))
        return false;
    Lines l = split_lines(a, data, len);
    bool found = false;
    for (int32_t i = 0; i < l.count; i++) {
        if (l.lines[i].len == 0 || (i == l.count - 1 && !l.eof_nl))
            continue;
        char err[128];
        JVal *v = json_parse(a, l.lines[i].ptr, l.lines[i].len, err,
                             sizeof err);
        if (!v)
            continue; /* damaged cache line: ignore, correctness unharmed */
        int64_t vat = jobj_int(v, "at", -1);
        const char *content = jobj_str(v, "content");
        if (vat < 0 || !content || (upto >= 0 && vat > upto))
            continue;
        if (!found || vat > *at) {
            *at = vat;
            JVal *c = jobj_get(v, "content");
            *state = split_lines(a, content, c->s.len);
            state->eof_nl = jobj_bool(v, "eof_nl", true);
            found = true;
        }
    }
    return found;
}

bool snap_maybe(Arena *a, const Repo *r, Idx *idx, const char *rel,
                const char *content, size_t len, char *err, size_t errsz) {
    int32_t fid = idx_file_id(idx, rel);
    if (fid < 0)
        return true;
    const FileHead *fh = &idx->heads[fid];
    if (fh->delta_count < SNAP_MIN_DELTAS || fh->delta_bytes <= 2 * len)
        return true;
    if (!snap_append(r, a, rel, fh->head, len == 0 || content[len - 1] == '\n',
                     content, len)) {
        snprintf(err, errsz, "cannot write snapshot for %s", rel);
        return false;
    }
    return idx_reset_budget(r, idx, fid, fh->head, err, errsz);
}

bool snap_replay(Arena *a, const Repo *r, const Idx *idx, const char *rel,
                 int64_t upto, Lines *out, bool *deleted) {
    int32_t fid = idx_file_id(idx, rel);
    if (fid < 0)
        return false;
    if (upto < 0)
        upto = idx->heads[fid].head;
    Lines cur = {NULL, 0, true};
    int64_t seed_at = -1;
    snap_nearest(a, r, rel, upto, &seed_at, &cur);

    int64_t *chain = NULL;
    size_t n = 0, cap = 0;
    for (int64_t e = upto; e > seed_at; e = idx->v[e].prev_same_file) {
        ARENA_GROW(a, chain, n, cap, int64_t);
        chain[n++] = e;
        if (idx->v[e].prev_same_file < 0)
            break;
    }
    bool is_deleted = seed_at < 0 && n == 0;
    for (size_t i = n; i > 0; i--) {
        Rec rec;
        if (!idx_fetch(a, r, idx, chain[i - 1], &rec))
            return false;
        rec_apply(a, &cur, &rec);
        is_deleted = idx->v[chain[i - 1]].op == IDX_OP_DELETE;
    }
    *out = cur;
    *deleted = is_deleted;
    return true;
}

typedef struct {
    Arena *a;
    char **rels;
    size_t n, cap;
} RelList;

static WalkAction collect_files(const char *rel, bool is_dir, void *ud) {
    RelList *rl = (RelList *)ud;
    if (!is_dir && !plat_is_tmp_name(rel)) {
        ARENA_GROW(rl->a, rl->rels, rl->n, rl->cap, char *);
        rl->rels[rl->n++] = arena_strdup(rl->a, rel);
    }
    return WALK_CONT;
}

bool snap_rebuild_all(Arena *a, Repo *r, char *err, size_t errsz) {
    if (!idx_sync(a, r, err, errsz))
        return false;
    Idx *idx = idx_ready(a, r);
    if (!idx) {
        snprintf(err, errsz, "index unavailable after sync");
        return false;
    }

    /* wipe old snapshot files, then regenerate per current policy */
    char dir[LAP_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/snapshots", r->lapdir);
    RelList old = {a, NULL, 0, 0};
    plat_walk(a, dir, collect_files, &old);
    for (size_t i = 0; i < old.n; i++) {
        char p[LAP_PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, old.rels[i]);
        plat_remove_file(p);
    }

    for (int32_t fid = 0; fid < idx->npaths; fid++) {
        Lines cur = {NULL, 0, true};
        FileHead fh = {-1, -1, 0, 0};
        int64_t *chain = NULL;
        size_t n = 0, cap = 0;
        for (int64_t e = idx->heads[fid].head; e >= 0;
             e = idx->v[e].prev_same_file) {
            ARENA_GROW(a, chain, n, cap, int64_t);
            chain[n++] = e;
        }
        bool is_deleted = n == 0;
        for (size_t i = n; i > 0; i--) {
            int64_t e = chain[i - 1];
            Rec rec;
            if (!idx_fetch(a, r, idx, e, &rec)) {
                snprintf(err, errsz, "cannot fetch record %lld during rebuild",
                         (long long)e);
                return false;
            }
            rec_apply(a, &cur, &rec);
            is_deleted = idx->v[e].op == IDX_OP_DELETE;
            fh.head = e;
            fh.delta_bytes += idx->v[e].len;
            fh.delta_count++;
            if (!is_deleted && i > 1 && fh.delta_count >= SNAP_MIN_DELTAS) {
                size_t blen;
                char *bytes = join_lines(a, cur, &blen);
                if (fh.delta_bytes > 2 * blen) {
                    if (!snap_append(r, a, idx->paths[fid], e, cur.eof_nl,
                                     bytes, blen)) {
                        snprintf(err, errsz, "cannot write snapshot for %s",
                                 idx->paths[fid]);
                        return false;
                    }
                    fh.snap_at = e;
                    fh.delta_bytes = 0;
                    fh.delta_count = 0;
                }
            }
        }
        /* final state -> shadow */
        char spath[LAP_PATH_MAX];
        snprintf(spath, sizeof spath, "%s/%s/%s", r->lapdir, LAP_SHADOW_NAME,
                 idx->paths[fid]);
        if (is_deleted) {
            plat_remove_file(spath);
        } else {
            char parent[LAP_PATH_MAX];
            snprintf(parent, sizeof parent, "%s", spath);
            char *slash = strrchr(parent, '/');
            *slash = '\0';
            size_t blen;
            char *bytes = join_lines(a, cur, &blen);
            if (!plat_mkdirs(parent) ||
                !plat_write_file_atomic(spath, bytes, blen)) {
                snprintf(err, errsz, "cannot write shadow for %s",
                         idx->paths[fid]);
                return false;
            }
        }
        idx->heads[fid] = fh;
    }
    if (!idx_write_heads(r, idx, err, errsz))
        return false;

    /* drop shadows the log does not know */
    snprintf(dir, sizeof dir, "%s/%s", r->lapdir, LAP_SHADOW_NAME);
    RelList sh = {a, NULL, 0, 0};
    plat_walk(a, dir, collect_files, &sh);
    for (size_t i = 0; i < sh.n; i++) {
        if (idx_file_id(idx, sh.rels[i]) < 0) {
            char p[LAP_PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", dir, sh.rels[i]);
            plat_remove_file(p);
        }
    }

    /* counters + active session + last hash for state.json */
    r->next_commit = (int64_t)idx->h.commits + 1;
    r->next_session = (int64_t)idx->h.sessions + 1;
    r->active_session[0] = '\0';
    r->active_session_msg[0] = '\0';
    if (idx->h.open_session) {
        for (int64_t e = (int64_t)idx->h.count - 1; e >= 0; e--) {
            if (idx->v[e].kind == IDX_SESSION_START &&
                idx->v[e].session == idx->h.open_session) {
                Rec rec;
                if (idx_fetch(a, r, idx, e, &rec)) {
                    snprintf(r->active_session, sizeof r->active_session,
                             "%s", rec.id);
                    snprintf(r->active_session_msg,
                             sizeof r->active_session_msg, "%s", rec.msg);
                }
                break;
            }
        }
    }
    if (idx->h.count == 0) {
        snprintf(r->last_hash, sizeof r->last_hash, "%s", LAP_HASH_ZERO);
    } else {
        const IdxEntry *last = &idx->v[idx->h.count - 1];
        char *line;
        if (!plat_read_range(a, r->logpath, last->off, last->len, &line)) {
            snprintf(err, errsz, "cannot read log tail record");
            return false;
        }
        sha256_hex(line, last->len, r->last_hash);
    }
    return true;
}
