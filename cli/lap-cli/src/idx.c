#include "idx.h"

#include <time.h>

/* 02: offsets into the chunked history. 03: amend entries; a lap from
 * before them finds no index it knows, reads the history itself and so
 * sees the amend records it cannot write after. */
#define IDX_MAGIC "LAPIDX03"

_Static_assert(sizeof(IdxHeader) == 40, "index header is 40 bytes");
_Static_assert(sizeof(IdxEntry) == 64, "index entries are 64 bytes");
_Static_assert(sizeof(FileHead) == 32, "head slots are 32 bytes");

static void cache_path(const Repo *r, const char *name, char *out,
                       size_t outsz) {
    snprintf(out, outsz, "%s/%s", r->lapdir, name);
}

uint64_t idx_epoch(const char *ts) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (sscanf(ts, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
#ifdef _WIN32
    return (uint64_t)_mkgmtime(&tm);
#else
    return (uint64_t)timegm(&tm);
#endif
}

/* with_entries=false loads only header + paths + heads: the writer's
 * append path must stay O(1) in index size — reading every entry per
 * commit would reinstate the exact cost this layer removes. */
static bool idx_load(Arena *a, const Repo *r, Idx *out, bool with_entries) {
    memset(out, 0, sizeof *out);
    char path[LAP_PATH_MAX];
    cache_path(r, "index", path, sizeof path);
    uint64_t fsize;
    if (!plat_file_size(path, &fsize) || fsize < sizeof(IdxHeader))
        return false;
    if (with_entries) {
        char *data;
        size_t len;
        if (!plat_read_file_max(a, path, &data, &len, (size_t)-1))
            return false;
        memcpy(&out->h, data, sizeof(IdxHeader));
        out->v = (IdxEntry *)(data + sizeof(IdxHeader));
    } else {
        char *data;
        if (!plat_read_range(a, path, 0, sizeof(IdxHeader), &data))
            return false;
        memcpy(&out->h, data, sizeof(IdxHeader));
    }
    if (memcmp(out->h.magic, IDX_MAGIC, 8) != 0 ||
        fsize < sizeof(IdxHeader) + out->h.count * sizeof(IdxEntry))
        return false;

    /* paths and heads are PART of the index, not optional extras: loading
     * without them would answer path-keyed queries wrongly instead of
     * falling back, which a cache must never do. */
    cache_path(r, "paths", path, sizeof path);
    char *pdata;
    size_t plen = 0;
    if (plat_read_file_max(a, path, &pdata, &plen, (size_t)-1) && plen > 0) {
        Lines l = split_lines(a, pdata, plen);
        out->paths = (char **)arena_alloc(
            a, (size_t)(l.count ? l.count : 1) * sizeof(char *));
        for (int32_t i = 0; i < l.count; i++)
            out->paths[i] = str_dup_c(a, l.lines[i]);
        out->npaths = l.count;
    }
    if (out->npaths == 0)
        return out->h.commits == 0; /* commits exist => paths must too */
    cache_path(r, "heads", path, sizeof path);
    char *hdata;
    size_t hlen;
    if (!plat_read_file_max(a, path, &hdata, &hlen, (size_t)-1) ||
        hlen != (size_t)out->npaths * sizeof(FileHead))
        return false;
    out->heads = (FileHead *)hdata;
    return true;
}

bool idx_header(Arena *a, const Repo *r, IdxHeader *out) {
    Idx idx;
    if (!idx_load(a, r, &idx, false))
        return false;
    *out = idx.h;
    return true;
}

Idx *idx_ready(Arena *a, const Repo *r) {
    if (r->foreign) /* the index covers this folder's history only */
        return NULL;
    Idx *idx = (Idx *)arena_alloc(a, sizeof(Idx));
    if (idx_load(a, r, idx, true) && idx->h.covered == r->hist.size) {
        idx->arena = a;
        if (idx->h.unknown > 0)
            rec_note_newer(NULL);
        return idx;
    }
    return NULL;
}

int32_t idx_file_id(const Idx *idx, const char *rel) {
    for (int32_t i = 0; i < idx->npaths; i++) {
        if (strcmp(idx->paths[i], rel) == 0)
            return i;
    }
    return -1;
}

int64_t idx_find_commit(const Idx *idx, int64_t commit_no) {
    int64_t lo = 0, hi = idx->h.count ? (int64_t)idx->h.count - 1 : -1;
    int64_t hit = -1;
    while (lo <= hi) { /* first entry with id >= commit_no */
        int64_t mid = lo + (hi - lo) / 2;
        if ((int64_t)idx->v[mid].id >= commit_no) {
            hit = mid;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    for (; hit >= 0 && hit < (int64_t)idx->h.count &&
           (int64_t)idx->v[hit].id == commit_no;
         hit++) {
        if (idx->v[hit].kind == IDX_COMMIT)
            return hit;
    }
    return -1;
}

/* Applies to commit c, read from entry, the amendments naming it: the
 * amend records are decoded once per index, and only those after the
 * commit can name it. */
static void idx_amend(const Repo *r, const Idx *cidx, int64_t entry, Rec *c) {
    Idx *idx = (Idx *)cidx; /* the amend cache only */
    Arena *a = idx->arena;
    if (!a)
        return;
    if (!idx->amends_read) {
        idx->amends_read = true;
        size_t cap = 0, atcap = 0;
        for (uint64_t i = 0; i < idx->h.count; i++) {
            const IdxEntry *e = &idx->v[i];
            char *line;
            char err[128];
            Rec am;
            if (e->kind != IDX_AMEND ||
                !hist_read(a, &r->hist, e->off, e->len, &line) ||
                !rec_decode(a, line, e->len, &am, err, sizeof err))
                continue;
            ARENA_GROW(a, idx->amends, (size_t)idx->amends_n, cap, Rec);
            ARENA_GROW(a, idx->amend_at, (size_t)idx->amends_n, atcap,
                       int64_t);
            idx->amend_at[idx->amends_n] = (int64_t)i;
            idx->amends[idx->amends_n++] = am;
        }
    }
    int32_t room = 0;
    for (int32_t i = 0; i < idx->amends_n; i++)
        room += idx->amend_at[i] > entry &&
                strcmp(idx->amends[i].of, c->hash) == 0;
    for (int32_t i = 0; room > 0 && i < idx->amends_n; i++) {
        if (idx->amend_at[i] > entry &&
            strcmp(idx->amends[i].of, c->hash) == 0)
            rec_amend_one(a, c, &idx->amends[i], room);
    }
}

bool idx_fetch(Arena *a, const Repo *r, const Idx *idx, int64_t entry,
               Rec *out) {
    const IdxEntry *e = &idx->v[entry];
    char *line;
    char err[128];
    if (!hist_read(a, &r->hist, e->off, e->len, &line) ||
        !rec_decode(a, line, e->len, out, err, sizeof err))
        return false;
    out->lineage = hist_label(&r->hist, hist_locate(&r->hist, e->off));
    if (out->type == REC_COMMIT)
        idx_amend(r, idx, entry, out);
    return true;
}

bool idx_write_heads(const Repo *r, const Idx *idx, char *err, size_t errsz) {
    char path[LAP_PATH_MAX];
    cache_path(r, "heads", path, sizeof path);
    if (!plat_write_file_atomic(path, idx->heads,
                                (size_t)idx->npaths * sizeof(FileHead))) {
        snprintf(err, errsz, "cannot write %s", path);
        return false;
    }
    return true;
}

bool idx_reset_budget(const Repo *r, Idx *idx, int32_t fid, int64_t at,
                      char *err, size_t errsz) {
    idx->heads[fid].snap_at = at;
    idx->heads[fid].delta_bytes = 0;
    idx->heads[fid].delta_count = 0;
    return idx_write_heads(r, idx, err, errsz);
}

/* ---- writer side ---- */

typedef struct { /* path -> file_id, open addressing */
    Arena *a;
    const char **keys;
    int32_t *vals;
    size_t cap, n;
} PathMap;

static void pm_grow(PathMap *m);

static void pm_init(PathMap *m, Arena *a) {
    m->a = a;
    m->cap = 64;
    m->n = 0;
    m->keys = (const char **)arena_alloc0(a, m->cap * sizeof(char *));
    m->vals = (int32_t *)arena_alloc(a, m->cap * sizeof(int32_t));
}

static size_t pm_slot(const char **keys, size_t cap, const char *key) {
    size_t i = (size_t)str_hash(str_c(key)) & (cap - 1);
    while (keys[i] && strcmp(keys[i], key) != 0)
        i = (i + 1) & (cap - 1);
    return i;
}

static void pm_grow(PathMap *m) {
    size_t ncap = m->cap * 2;
    const char **nk = (const char **)arena_alloc0(m->a, ncap * sizeof(char *));
    int32_t *nv = (int32_t *)arena_alloc(m->a, ncap * sizeof(int32_t));
    for (size_t i = 0; i < m->cap; i++) {
        if (m->keys[i]) {
            size_t j = pm_slot(nk, ncap, m->keys[i]);
            nk[j] = m->keys[i];
            nv[j] = m->vals[i];
        }
    }
    m->keys = nk;
    m->vals = nv;
    m->cap = ncap;
}

static int32_t pm_get(const PathMap *m, const char *key) {
    size_t i = pm_slot(m->keys, m->cap, key);
    return m->keys[i] ? m->vals[i] : -1;
}

static void pm_put(PathMap *m, const char *key, int32_t val) {
    if (m->n * 2 >= m->cap)
        pm_grow(m);
    size_t i = pm_slot(m->keys, m->cap, key);
    if (!m->keys[i]) {
        m->keys[i] = key;
        m->n++;
    }
    m->vals[i] = val;
}

typedef struct {
    Idx idx;
    IdxEntry *newv;
    size_t newn, newcap;
    char **paths;
    size_t npaths, pcap;
    FileHead *heads;
    size_t hcap;
    PathMap pm;
    bool paths_grew;
} Sync;

static int32_t sync_file_id(Arena *a, Sync *s, const char *rel) {
    int32_t id = pm_get(&s->pm, rel);
    if (id >= 0)
        return id;
    ARENA_GROW(a, s->paths, s->npaths, s->pcap, char *);
    size_t hn = s->npaths;
    ARENA_GROW(a, s->heads, hn, s->hcap, FileHead);
    id = (int32_t)s->npaths;
    s->paths[id] = arena_strdup(a, rel);
    s->heads[id].head = -1;
    s->heads[id].snap_at = -1;
    s->heads[id].delta_bytes = 0;
    s->heads[id].delta_count = 0;
    s->npaths++;
    pm_put(&s->pm, s->paths[id], id);
    s->paths_grew = true;
    return id;
}

static bool sync_write(const Repo *r, Sync *s, uint64_t covered, char *err,
                       size_t errsz) {
    char path[LAP_PATH_MAX];
    cache_path(r, "index", path, sizeof path);
    FILE *f = fopen(path, "r+b");
    if (!f)
        f = fopen(path, "w+b");
    if (!f) {
        snprintf(err, errsz, "cannot open %s", path);
        return false;
    }
    /* Entries, then the sidecars, and only then the header. The header's
     * `covered` is the freshness marker readers trust, so publishing it
     * before the data it describes would leave a stale index looking
     * current — unrepairable, because sync would then short-circuit. */
    bool ok =
        fseek(f, (long)(sizeof(IdxHeader) +
                        s->idx.h.count * sizeof(IdxEntry)),
              SEEK_SET) == 0 &&
        (s->newn == 0 ||
         fwrite(s->newv, sizeof(IdxEntry), s->newn, f) == s->newn) &&
        plat_fsync(f);
    if (ok && s->paths_grew) {
        StrBuf sb;
        sb_init(&sb, s->pm.a);
        for (size_t i = 0; i < s->npaths; i++) {
            sb_puts(&sb, s->paths[i]);
            sb_putc(&sb, '\n');
        }
        cache_path(r, "paths", path, sizeof path);
        ok = plat_write_file_atomic(path, sb.data, sb.len);
    }
    if (ok) {
        cache_path(r, "heads", path, sizeof path);
        ok = plat_write_file_atomic(path, s->heads,
                                    s->npaths * sizeof(FileHead));
    }
    if (ok) {
        s->idx.h.covered = covered;
        s->idx.h.count += s->newn;
        memcpy(s->idx.h.magic, IDX_MAGIC, 8);
        ok = fseek(f, 0, SEEK_SET) == 0 &&
             fwrite(&s->idx.h, sizeof(IdxHeader), 1, f) == 1 &&
             plat_fsync(f);
    }
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        snprintf(err, errsz, "cannot write the index cache");
        return false;
    }
    return true;
}

bool idx_sync(Arena *a, const Repo *r, char *err, size_t errsz) {
    uint64_t size = r->hist.size;
    Sync s;
    memset(&s, 0, sizeof s);
    pm_init(&s.pm, a);
    bool loaded = idx_load(a, r, &s.idx, false) && s.idx.h.covered <= size;
    if (!loaded)
        memset(&s.idx, 0, sizeof s.idx); /* damaged or shrunk: full rebuild */
    for (int32_t i = 0; i < s.idx.npaths; i++) {
        ARENA_GROW(a, s.paths, s.npaths, s.pcap, char *);
        size_t hn = s.npaths;
        ARENA_GROW(a, s.heads, hn, s.hcap, FileHead);
        s.paths[s.npaths] = s.idx.paths[i];
        s.heads[s.npaths] = s.idx.heads[i];
        pm_put(&s.pm, s.paths[s.npaths], (int32_t)s.npaths);
        s.npaths++;
    }
    if (loaded && s.idx.h.covered == size)
        return true;

    /* a history with nothing in it yet still gets its (empty) index */
    char *data = "";
    size_t dlen = (size_t)(size - s.idx.h.covered);
    if (dlen > 0 && !hist_read(a, &r->hist, s.idx.h.covered, dlen, &data)) {
        snprintf(err, errsz, "cannot read log tail");
        return false;
    }
    uint64_t off = s.idx.h.covered;
    uint64_t covered = s.idx.h.covered;
    Lines l = split_lines(a, data, dlen);
    for (int32_t i = 0; i < l.count; i++) {
        Str line = l.lines[i];
        if (i == l.count - 1 && !l.eof_nl)
            break; /* torn tail: not covered, next sync retries */
        if (line.len == 0) {
            off += 1;
            covered = off;
            continue;
        }
        Rec rec;
        char derr[128];
        if (!rec_decode(a, line.ptr, line.len, &rec, derr, sizeof derr)) {
            snprintf(err, errsz, "log offset %llu: %s",
                     (unsigned long long)off, derr);
            return false;
        }
        IdxEntry e;
        memset(&e, 0, sizeof e);
        e.off = off;
        e.len = (uint32_t)line.len;
        e.ts = idx_epoch(rec.ts);
        e.file_id = UINT32_MAX;
        e.prev_same_file = -1;
        switch (rec.type) {
        case REC_INIT:
            e.kind = IDX_INIT;
            break;
        case REC_COMMIT: {
            e.kind = IDX_COMMIT;
            s.idx.h.commits++;
            e.op = strcmp(rec.op, "create") == 0   ? IDX_OP_CREATE
                   : strcmp(rec.op, "delete") == 0 ? IDX_OP_DELETE
                                                   : IDX_OP_EDIT;
            e.session = rec_session_no(rec.session);
            e.old_start = (uint32_t)rec.old_start;
            e.old_lines = (uint32_t)rec.old_lines;
            e.new_start = (uint32_t)rec.new_start;
            e.new_lines = (uint32_t)rec.new_lines;
            int32_t fid = sync_file_id(a, &s, rec.file);
            e.file_id = (uint32_t)fid;
            e.prev_same_file = s.heads[fid].head;
            int64_t self = (int64_t)s.idx.h.count + (int64_t)s.newn;
            s.heads[fid].head = self;
            s.heads[fid].delta_bytes += e.len;
            s.heads[fid].delta_count++;
            break;
        }
        case REC_SESSION_START: {
            e.kind = IDX_SESSION_START;
            uint32_t sn = rec_session_no(rec.id);
            e.session = sn;
            if (!rec.from) /* an adopted session is history, not open */
                s.idx.h.open_session = sn;
            if (sn > s.idx.h.sessions)
                s.idx.h.sessions = sn;
            break;
        }
        case REC_SESSION_END:
            e.kind = IDX_SESSION_END;
            e.session = rec_session_no(rec.id);
            if (!rec.from)
                s.idx.h.open_session = 0;
            break;
        case REC_BRANCH: /* a branch starts with no session open */
            e.kind = IDX_BRANCH;
            s.idx.h.open_session = 0;
            break;
        case REC_MERGE:
            e.kind = IDX_MERGE;
            break;
        case REC_AMEND:
            e.kind = IDX_AMEND;
            break;
        case REC_UNKNOWN: /* a writer refuses a history holding one */
            e.kind = IDX_UNKNOWN;
            s.idx.h.unknown++;
            break;
        }
        e.id = s.idx.h.commits;
        ARENA_GROW(a, s.newv, s.newn, s.newcap, IdxEntry);
        s.newv[s.newn++] = e;
        off += line.len + 1;
        covered = off;
    }
    return sync_write(r, &s, covered, err, errsz);
}
