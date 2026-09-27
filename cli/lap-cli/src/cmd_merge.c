#include "adopt.h"
#include "branches.h"
#include "cmd.h"

/* lap merge <branch>: after git merged a branch's code into this folder,
 * adopt the branch's history — every commit that can be placed on this
 * folder's version of its file (adopt.h), with from links to the
 * originals — and close with a merge record. What could not be placed is
 * left for `lap status`. See SPEC.md, Branches → Merging. */

typedef struct {
    const char *key; /* a branch session id, or a record hash */
    const char *val;
} Pair;

typedef struct {
    Pair *v;
    int32_t n, cap;
} Map;

static void map_put(Arena *a, Map *m, const char *key, const char *val) {
    ARENA_GROW(a, m->v, m->n, m->cap, Pair);
    m->v[m->n].key = key;
    m->v[m->n].val = val;
    m->n++;
}

static const char *map_get(const Map *m, const char *key) {
    for (int32_t i = m->n - 1; i >= 0; i--) {
        if (strcmp(m->v[i].key, key) == 0)
            return m->v[i].val;
    }
    return NULL;
}

static int32_t find_hash(const RecLog *log, const char *hash) {
    for (int32_t i = log->count - 1; i >= 0; i--) {
        if (strcmp(log->v[i].hash, hash) == 0)
            return i;
    }
    return -1;
}

/* A file's lines as of record `upto` of log; *exists false when it was
 * never created, or deleted. */
static Lines file_at(Arena *a, const RecLog *log, const char *file,
                     int32_t upto, bool *exists) {
    Lines l = {NULL, 0, true};
    bool deleted = false;
    *exists = rec_replay_file(a, log, file, upto, &l, &deleted) && !deleted;
    if (!*exists) {
        l.lines = NULL;
        l.count = 0;
        l.eof_nl = true;
    }
    return l;
}

/* A branch folder under git: it has a .git entry (a directory, or the file
 * a worktree holds). lap never runs git; this is the whole check. */
static bool folder_is_git(const char *path) {
    char g[LAP_PATH_MAX];
    snprintf(g, sizeof g, "%s/.git", path);
    return plat_is_dir(g) || plat_is_file(g);
}

bool own_chunks(Arena *a, const char *lapdir, const char *from,
                const char *id, bool fill, OwnChunk **out, int32_t *n,
                char *err, size_t errsz) {
    Hist here, there;
    memset(&there, 0, sizeof there);
    if (!hist_open(a, lapdir, id, &here, err, errsz))
        return false;
    if (from && fill) {
        char flap[LAP_PATH_MAX];
        snprintf(flap, sizeof flap, "%s/%s", from, LAP_DIR);
        if (!hist_open(a, flap, id, &there, err, errsz))
            return false;
    }
    int32_t max = here.n > there.n ? here.n : there.n;
    OwnChunk *v =
        (OwnChunk *)arena_alloc0(a, (size_t)(max ? max : 1) * sizeof(OwnChunk));
    int32_t k = 0;
    for (; k < max; k++) {
        char path[LAP_PATH_MAX];
        char *hd = NULL, *td = NULL;
        size_t hl = 0, tl = 0;
        bool has_h = k < here.n, has_t = k < there.n;
        if (has_h) {
            hist_chunk_path(&here, k, path, sizeof path);
            if (!plat_read_file(a, path, &hd, &hl)) {
                snprintf(err, errsz, "cannot read %s", path);
                return false;
            }
        }
        if (has_t) {
            hist_chunk_path(&there, k, path, sizeof path);
            if (!plat_read_file(a, path, &td, &tl)) {
                snprintf(err, errsz, "cannot read %s", path);
                return false;
            }
            while (tl > 0 && td[tl - 1] != '\n')
                tl--; /* a line the branch is still writing */
        }
        OwnChunk *c = &v[k];
        hist_chunk_name(id, k + 1, c->name);
        if (has_h && !(has_t && tl > hl && memcmp(td, hd, hl) == 0)) {
            c->data = hd;
            c->len = hl;
        } else if (has_t && tl > 0) {
            c->data = td;
            c->len = tl;
            c->write = true;
        } else {
            break;
        }
    }
    *out = v;
    *n = k;
    return true;
}

int32_t cmd_merge(Arena *a, int32_t argc, char **argv) {
    static const char *const bool_flags[] = {"--json", "--dry-run",
                                             "--copy-from-folder", NULL};
    bool json = has_flag(argc, argv, NULL, "--json");
    if (!flags_known(argc, argv, NULL, bool_flags))
        return LAP_EXIT_ERR;
    bool dry = has_flag(argc, argv, NULL, "--dry-run");
    bool copy = has_flag(argc, argv, NULL, "--copy-from-folder");
    const char *key = positional_arg(argc, argv, NULL, 0);
    if (!key || positional_arg(argc, argv, NULL, 1)) {
        err_out(json, "usage", "usage: lap merge <branch> [--dry-run] "
                               "[--copy-from-folder] [--json]");
        return LAP_EXIT_ERR;
    }
    Repo repo;
    char err[1024];
    if (!repo_open(a, &repo, !dry, err, sizeof err)) {
        err_out(json, "no_repo", "%s", err);
        return LAP_EXIT_ERR;
    }
    int32_t rc = LAP_EXIT_ERR;
    if (repo.hist.parent[0]) {
        err_out(json, "merge_in_branch",
                "this folder is branch %s; lap merge runs in the folder a "
                "branch started from",
                repo.hist.name);
        goto done;
    }

    /* The branch, and where its history is. */
    Branches reg;
    branches_load(a, repo.lapdir, &reg);
    const char *id = branch_find(a, &repo, &reg, key);
    const BranchEntry *ent = id ? branches_find(&reg, id) : NULL;
    bool reachable = ent && branch_folder_is(a, ent->path, id);
    if (!id) {
        err_out(json, "branch_not_found",
                "no branch %s here: it is not in this folder's registry, and "
                "no chunk of it is in %s (git merge its code first, or "
                "check the name with lap branch list)",
                key, repo.hist.dir);
        goto done;
    }
    /* The branch's own chunks, this folder's copies first. A git checkout's
     * history comes here through git merge, never from its folder unless
     * the user asks: taken earlier, it would run ahead of the code, and the
     * next git merge would conflict on the copies. */
    bool git = reachable && folder_is_git(ent->path);
    OwnChunk *own;
    int32_t nown;
    if (!own_chunks(a, repo.lapdir, reachable ? ent->path : NULL, id,
                    reachable && (!git || copy), &own, &nown, err,
                    sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        goto done;
    }
    if (nown == 0 && git && !copy) {
        err_out(json, "git_merge_first",
                "branch %s's history has not come through git yet: no chunk "
                "of it is in %s. Run git merge on its git branch first, then "
                "lap merge",
                key, repo.hist.dir);
        goto done;
    }
    if (nown == 0) {
        err_out(json, "branch_not_found",
                "branch %s is registered but its history is nowhere to be "
                "read: no chunk of it is in %s, nor in its folder %s",
                key, repo.hist.dir, ent ? ent->path : "(unknown)");
        goto done;
    }

    /* Its history as one stream: its parent's chunks up to its base chunk,
     * as its branch record (its first line) names them, then its own. */
    Rec first;
    const char *nl = memchr(own[0].data, '\n', own[0].len);
    if (!nl ||
        !rec_decode(a, own[0].data, (size_t)(nl - own[0].data), &first, err,
                    sizeof err) ||
        first.type != REC_BRANCH || strcmp(first.id, id) != 0) {
        err_out(json, "log_broken",
                "branch %s's history does not open with its branch record",
                key);
        goto done;
    }
    Hist ph;
    char *pdata;
    if (!hist_open(a, repo.lapdir, first.parent, &ph, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        goto done;
    }
    if (first.base_chunk < 1 || first.base_chunk > ph.n) {
        err_out(json, "unrelated_history",
                "branch %s did not start from this folder's history", key);
        goto done;
    }
    const HistChunk *bc = &ph.v[first.base_chunk - 1];
    if (!hist_read(a, &ph, 0, (size_t)(bc->start + bc->size), &pdata)) {
        err_out(json, "log_unreadable", "cannot read %s", ph.dir);
        goto done;
    }
    StrBuf all;
    sb_init(&all, a);
    sb_putn(&all, pdata, (size_t)(bc->start + bc->size));
    for (int32_t k = 0; k < nown; k++)
        sb_putn(&all, own[k].data, own[k].len);
    size_t blen = all.len;
    char *bdata = sb_finish(&all);

    /* Both histories, and the branch's record. */
    RecLog plog, blog;
    if (!repo_log_load(a, &repo, &plog, err, sizeof err) ||
        !rec_log_parse(a, bdata, blen, NULL, NULL, &blog, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        goto done;
    }
    if (!blog.chain_ok) {
        err_out(json, "log_broken", "branch %s's history is broken: %s", key,
                blog.chain_err);
        goto done;
    }
    int32_t bi = -1;
    for (int32_t i = 0; i < blog.count; i++) {
        if (blog.v[i].type == REC_BRANCH && strcmp(blog.v[i].id, id) == 0) {
            bi = i;
            break;
        }
    }
    const Rec *brec = bi >= 0 ? &blog.v[bi] : NULL;
    if (!brec || find_hash(&plog, brec->base) < 0) {
        err_out(json, "unrelated_history",
                "branch %s did not start from this folder's history", key);
        goto done;
    }

    /* What earlier merges of it adopted, and the sessions they carried. */
    const char *last_head = NULL;
    StrSet stopped_before;
    strset_init(&stopped_before, a);
    Map adopted_sessions = {0}; /* branch session_start hash -> our id */
    for (int32_t i = 0; i < plog.count; i++) {
        const Rec *p = &plog.v[i];
        if (p->type == REC_MERGE && strcmp(p->branch, id) == 0) {
            last_head = p->head;
            for (int32_t k = 0; k < p->stopped_n; k++)
                strset_add(&stopped_before, p->stopped_file[k]);
        } else if (p->type == REC_SESSION_START && p->from) {
            map_put(a, &adopted_sessions, p->from, p->id);
        }
    }
    int32_t upto = bi; /* the branch's version of every file so far adopted */
    if (last_head) {
        upto = find_hash(&blog, last_head);
        if (upto < bi) {
            err_out(json, "log_broken",
                    "branch %s's history no longer holds %.7s, where the "
                    "last merge stopped",
                    key, last_head);
            goto done;
        }
    }
    int32_t start = upto + 1;
    Map starts = {0}; /* branch session id -> its session_start's hash */
    for (int32_t i = bi + 1; i < blog.count; i++) {
        if (blog.v[i].type == REC_SESSION_START)
            map_put(a, &starts, blog.v[i].id, blog.v[i].hash);
    }

    /* Placement, file by file. */
    int32_t *at = (int32_t *)arena_alloc0(
        a, (size_t)(blog.count ? blog.count : 1) * sizeof(int32_t));
    bool *eof = (bool *)arena_alloc0(
        a, (size_t)(blog.count ? blog.count : 1) * sizeof(bool));
    const char **files = NULL;
    size_t nfiles = 0, fcap = 0;
    StrSet seen;
    strset_init(&seen, a);
    for (int32_t i = start; i < blog.count; i++) {
        if (blog.v[i].type == REC_COMMIT && strset_add(&seen, blog.v[i].file)) {
            ARENA_GROW(a, files, nfiles, fcap, const char *);
            files[nfiles++] = blog.v[i].file;
        }
    }
    Lines *results =
        (Lines *)arena_alloc0(a, (nfiles ? nfiles : 1) * sizeof(Lines));
    bool *changed = (bool *)arena_alloc0(a, nfiles ? nfiles : 1);
    bool *gone = (bool *)arena_alloc0(a, nfiles ? nfiles : 1);
    const char **stop_file = NULL, **stop_at = NULL, **stop_why = NULL;
    size_t nstop = 0, scap = 0, scap2 = 0, scap3 = 0;
    int32_t adopted = 0, left = 0;
    for (size_t f = 0; f < nfiles; f++) {
        const Rec **mine = NULL;
        int32_t *idx = NULL;
        size_t n = 0, mcap = 0, icap = 0;
        for (int32_t i = start; i < blog.count; i++) {
            if (blog.v[i].type != REC_COMMIT ||
                strcmp(blog.v[i].file, files[f]) != 0)
                continue;
            size_t ni = n;
            ARENA_GROW(a, mine, n, mcap, const Rec *);
            ARENA_GROW(a, idx, ni, icap, int32_t);
            mine[n] = &blog.v[i];
            idx[n] = i;
            n++;
        }
        if (strset_has(&stopped_before, files[f])) {
            left += (int32_t)n; /* stopped once, stopped for good */
            continue;
        }
        bool base_has, parent_has;
        Lines base = file_at(a, &blog, files[f], upto, &base_has);
        Lines parent = file_at(a, &plog, files[f], plog.count - 1,
                               &parent_has);
        Placement p;
        adopt_place(a, base, parent, (const Rec *const *)mine, (int32_t)n,
                    &p);
        for (int32_t k = 0; k < p.placed; k++) {
            at[idx[k]] = p.start[k];
            eof[idx[k]] = p.eof_nl[k];
        }
        adopted += p.placed;
        left += (int32_t)n - p.placed;
        if (p.placed > 0) {
            changed[f] = true;
            results[f] = p.result;
            gone[f] = strcmp(mine[p.placed - 1]->op, "delete") == 0;
        }
        if (p.placed < (int32_t)n) {
            size_t s2 = nstop, s3 = nstop;
            ARENA_GROW(a, stop_file, nstop, scap, const char *);
            ARENA_GROW(a, stop_at, s2, scap2, const char *);
            ARENA_GROW(a, stop_why, s3, scap3, const char *);
            stop_file[nstop] = files[f];
            stop_at[nstop] = mine[p.placed]->hash;
            stop_why[nstop] = p.why;
            nstop++;
        }
    }

    /* Chunks read from the branch folder are kept here, so the history
     * stays readable once that folder is gone — written only now, with
     * every check passed. */
    for (int32_t k = 0; !dry && k < nown; k++) {
        if (!own[k].write)
            continue;
        char dst[LAP_PATH_MAX];
        snprintf(dst, sizeof dst, "%s/%s", repo.hist.dir, own[k].name);
        if (!plat_mkdirs(repo.hist.dir) ||
            !plat_write_file_atomic(dst, own[k].data, own[k].len)) {
            err_out(json, "io_error", "cannot copy %s into %s", own[k].name,
                    repo.hist.dir);
            goto done;
        }
    }

    /* Appending, in the branch's order. */
    StrBuf ids;
    sb_init(&ids, a);
    if (!dry && start < blog.count) {
        for (int32_t i = start; i < blog.count; i++) {
            const Rec *b = &blog.v[i];
            Rec rec;
            memset(&rec, 0, sizeof rec);
            rec.from = b->hash;
            rec.ts = b->ts;
            rec.user = b->user;
            if (b->type == REC_SESSION_START) {
                if (map_get(&adopted_sessions, b->hash))
                    continue;
                rec.type = REC_SESSION_START;
                rec.id = arena_printf(a, "S%lld", (long long)repo.next_session++);
                rec.msg = b->msg;
                rec.meta_keys = b->meta_keys;
                rec.meta_vals = b->meta_vals;
                rec.meta_n = b->meta_n;
                map_put(a, &adopted_sessions, b->hash, rec.id);
            } else if (b->type == REC_SESSION_END) {
                const char *sh = map_get(&starts, b->id);
                const char *ours = sh ? map_get(&adopted_sessions, sh) : NULL;
                if (!ours)
                    continue;
                rec.type = REC_SESSION_END;
                rec.id = ours;
                rec.user = NULL;
            } else if (b->type == REC_COMMIT && at[i] > 0) {
                const char *sh = b->session ? map_get(&starts, b->session)
                                            : NULL;
                rec.type = REC_COMMIT;
                rec.id = arena_printf(a, "L%lld", (long long)repo.next_commit++);
                rec.session = sh ? map_get(&adopted_sessions, sh) : NULL;
                rec.file = b->file;
                rec.op = b->op;
                rec.old_start = rec.new_start = at[i];
                rec.old_lines = b->old_lines;
                rec.new_lines = b->new_lines;
                rec.old_text = b->old_text;
                rec.old_n = b->old_n;
                rec.new_text = b->new_text;
                rec.new_n = b->new_n;
                rec.eof_nl = eof[i];
                rec.intent = b->intent;
                rec.behavior = b->behavior;
                rec.forced = b->forced;
            } else {
                continue;
            }
            if (!repo_append(&repo, &rec, err, sizeof err)) {
                err_out(json, "io_error", "%s", err);
                goto done;
            }
            if (rec.type == REC_COMMIT) {
                if (ids.len)
                    sb_puts(&ids, json ? "," : ", ");
                if (json)
                    sb_printf(&ids, "{\"id\":\"%s\",\"from\":\"%s\"}", rec.id,
                              b->hash);
                else
                    sb_puts(&ids, rec.id);
            }
        }
        Rec m;
        memset(&m, 0, sizeof m);
        m.type = REC_MERGE;
        m.branch = id;
        m.name = brec->name;
        m.head = blog.v[blog.count - 1].hash;
        m.adopted = adopted;
        m.left = left;
        m.stopped_file = stop_file;
        m.stopped_at = stop_at;
        m.stopped_n = (int32_t)nstop;
        m.user = repo_user(&repo);
        if (!repo_append(&repo, &m, err, sizeof err)) {
            err_out(json, "io_error", "%s", err);
            goto done;
        }
        for (size_t f = 0; f < nfiles; f++) {
            if (!changed[f])
                continue;
            bool ok = true;
            if (gone[f]) {
                shadow_remove(&repo, files[f]); /* absent is as good */
            } else {
                size_t len;
                char *data = join_lines(a, results[f], &len);
                ok = shadow_write(&repo, files[f], data, len);
            }
            if (!ok) {
                err_out(json, "io_error", "cannot update the shadow of %s",
                        files[f]);
                goto done;
            }
        }
        if (!repo_state_save(&repo, err, sizeof err)) {
            err_out(json, "io_error", "%s", err);
            goto done;
        }
        caches_sync_warn(a, &repo);
    }

    /* The report. */
    int32_t total = adopted + left;
    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_printf(&sb, "{\"ok\":true,\"dry_run\":%s,\"branch\":\"%s\",\"name\":",
                  dry ? "true" : "false", id);
        json_escape_c(&sb, brec->name);
        sb_printf(&sb, ",\"new\":%s,\"adopted\":%d,\"left\":%d,\"head\":\"%s\"",
                  start < blog.count ? "true" : "false", adopted, left,
                  blog.v[blog.count - 1].hash);
        sb_puts(&sb, ",\"stopped\":[");
        for (size_t s = 0; s < nstop; s++) {
            sb_puts(&sb, s ? ",{\"file\":" : "{\"file\":");
            json_escape_c(&sb, stop_file[s]);
            sb_printf(&sb, ",\"at\":\"%s\",\"why\":", stop_at[s]);
            json_escape_c(&sb, stop_why[s]);
            sb_putc(&sb, '}');
        }
        sb_printf(&sb, "],\"commits\":[%s]}", ids.len ? sb_finish(&ids) : "");
        puts(sb_finish(&sb));
    } else if (start >= blog.count) {
        printf("branch %s: nothing new to adopt since the last merge\n",
               brec->name);
    } else {
        printf("%s branch %s (%s): %s %d of %d commit%s%s%s%s\n",
               dry ? "would merge" : "merged", brec->name, id,
               dry ? "would adopt" : "adopted", adopted, total,
               total == 1 ? "" : "s", ids.len ? " (" : "",
               ids.len ? sb_finish(&ids) : "", ids.len ? ")" : "");
        for (size_t s = 0; s < nstop; s++)
            printf("  stopped: %s at #%.7s (%s)\n", stop_file[s], stop_at[s],
                   stop_why[s]);
        if (left > 0 && !dry)
            printf("what is left shows in lap status: commit it as usual, "
                   "citing the branch commits it stands for (#<hash>)\n");
    }
    rc = LAP_EXIT_OK;
done:
    repo_close(&repo);
    return rc;
}
