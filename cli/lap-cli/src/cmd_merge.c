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

/* A branch of the chain one merge adopts: the branch named, and the
 * branches it started from that this folder has not taken in yet — outer
 * (the one started from this folder's history) first. */
typedef struct {
    const char *id, *name;
    int32_t cut;        /* its own chunks up to this one (0: all) */
    OwnChunk *own;      /* its own chunks, as read */
    int32_t nown;
    int32_t first, last; /* its branch record and last record in the stream */
    const char *head;    /* its head at its last merge here, or NULL */
    int32_t adopted, left;
    const char **stop_file, **stop_at;
    size_t nstop, scap, scap2;
    const char **already;
    size_t nalready, acap;
} Lin;

/* Branches nest at most this deep: a longer chain is a loop. */
#define MERGE_MAX_CHAIN 32

/* True when lineage's records are part of the history this folder reads:
 * its own, or one it started from. */
static bool in_view(const Hist *h, const char *lineage) {
    if (strcmp(h->lineage, lineage) == 0)
        return true;
    for (int32_t i = 0; i < h->n; i++) {
        if (strcmp(h->v[i].lineage, lineage) == 0)
            return true;
    }
    return false;
}

int32_t merge_redo_point(const RecLog *log, const StrSet *newer) {
    int32_t p0 = -1; /* the interrupted run's first record here */
    for (int32_t i = 0; i < log->count && p0 < 0; i++) {
        if (log->v[i].from && strset_has(newer, log->v[i].from))
            p0 = i;
    }
    if (p0 < 0)
        return log->count - 1;
    for (int32_t i = p0; i < log->count; i++) {
        if (!log->v[i].from || !strset_has(newer, log->v[i].from))
            return log->count - 1; /* other work since: no redo */
    }
    return p0 - 1;
}

/* Adds an adopted commit's id (and, for JSON, its original's hash) to the
 * report's list. */
static void report_id(StrBuf *ids, bool json, const char *id,
                      const char *from) {
    if (ids->len)
        sb_puts(ids, json ? "," : ", ");
    if (json)
        sb_printf(ids, "{\"id\":\"%s\",\"from\":\"%s\"}", id, from);
    else
        sb_puts(ids, id);
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
        err_out(json, repo_error_code(), "%s", err);
        return LAP_EXIT_ERR;
    }
    int32_t rc = LAP_EXIT_ERR;

    /* The branch, and where its history is. */
    Branches reg;
    branches_load_deep(a, repo.lapdir, &reg);
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
    /* The chain: the branch, then each branch it started from, until one
     * whose history this folder already reads. Merged into the branch it
     * started from, a branch is its own chain; a branch of a branch merged
     * straight into main brings the branches between, as git's merge of it
     * brings their code. A git checkout's history comes here through git
     * merge, never from its folder unless the user asks: taken earlier, it
     * would run ahead of the code, and the next git merge would conflict on
     * the copies. */
    bool git = reachable && folder_is_git(ent->path);
    bool fill = reachable && (!git || copy);
    char flap[LAP_PATH_MAX];
    snprintf(flap, sizeof flap, "%s/%s", reachable ? ent->path : ".",
             LAP_DIR);
    Lin *lin = (Lin *)arena_alloc0(a, MERGE_MAX_CHAIN * sizeof(Lin));
    int32_t nlin = 0;
    const char *anchor = id;
    int32_t anchor_cut = 0;
    while (!in_view(&repo.hist, anchor)) {
        Rec fr;
        bool found =
            hist_first_record(a, repo.lapdir, anchor, &fr, err, sizeof err) ||
            (fill && hist_first_record(a, flap, anchor, &fr, err, sizeof err));
        if (!found && git && !copy) {
            err_out(json, "git_merge_first",
                    "branch %s's history has not come through git yet: no "
                    "chunk of %s is in %s. Run git merge on its git branch "
                    "first, then lap merge",
                    key, anchor, repo.hist.dir);
            goto done;
        }
        if (!found) {
            err_out(json, "branch_not_found",
                    "branch %s is registered but its history is nowhere to be "
                    "read: no chunk of %s is in %s, nor in its folder %s",
                    key, anchor, repo.hist.dir,
                    ent ? ent->path : "(unknown)");
            goto done;
        }
        if (fr.type != REC_BRANCH || strcmp(fr.id, anchor) != 0 ||
            nlin == MERGE_MAX_CHAIN) {
            err_out(json, "log_broken",
                    "branch %s's history does not open with its branch "
                    "record, or its branches form a loop",
                    anchor);
            goto done;
        }
        lin[nlin].id = anchor;
        lin[nlin].name = fr.name;
        lin[nlin].cut = anchor_cut;
        nlin++;
        anchor = fr.parent;
        anchor_cut = fr.base_chunk;
    }
    if (nlin == 0) {
        err_out(json, "merge_in_branch",
                "%s is this folder's own history, or one it started from: "
                "lap merge adopts a branch started from here (or from one of "
                "its branches)",
                key);
        goto done;
    }
    for (int32_t k = 0; k < nlin / 2; k++) { /* outer first */
        Lin t = lin[k];
        lin[k] = lin[nlin - 1 - k];
        lin[nlin - 1 - k] = t;
    }

    /* One stream: this folder's history up to the outer branch's base
     * chunk, then each branch's own chunks up to the next one's base,
     * this folder's copies first (own_chunks). */
    Hist ah;
    char *adata;
    size_t alen;
    if (!hist_open_view(a, repo.lapdir, anchor, anchor_cut, &ah, err,
                        sizeof err)) {
        err_out(json, "unrelated_history",
                "branch %s did not start from this folder's history: %s", key,
                err);
        goto done;
    }
    if (!hist_read_all(a, &ah, &adata, &alen)) {
        err_out(json, "log_unreadable", "cannot read %s", ah.dir);
        goto done;
    }
    StrBuf all;
    sb_init(&all, a);
    sb_putn(&all, adata, alen);
    for (int32_t k = 0; k < nlin; k++) {
        Lin *l = &lin[k];
        if (!own_chunks(a, repo.lapdir, reachable ? ent->path : NULL, l->id,
                        fill, &l->own, &l->nown, err, sizeof err)) {
            err_out(json, "log_unreadable", "%s", err);
            goto done;
        }
        if (l->cut > 0 && l->nown > l->cut)
            l->nown = l->cut;
        if (l->nown == 0 || l->nown < l->cut) {
            err_out(json, git && !copy ? "git_merge_first" : "log_unreadable",
                    "branch %s's history is not all here: %s lacks chunks of "
                    "%s%s",
                    key, repo.hist.dir, l->name,
                    git && !copy ? " (run git merge on its git branch first, "
                                   "then lap merge)"
                                 : "");
            goto done;
        }
        for (int32_t j = 0; j < l->nown; j++)
            sb_putn(&all, l->own[j].data, l->own[j].len);
    }
    size_t blen = all.len;
    char *bdata = sb_finish(&all);

    /* Both histories, and the branch's record. */
    RecLog plog, blog;
    if (!repo_log_load(a, &repo, &plog, err, sizeof err) ||
        !rec_log_parse(a, bdata, blen, NULL, NULL, &blog, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        goto done;
    }
    /* Which branch of the chain each record belongs to (-1: this folder's
     * own part), and where each branch's records lie. */
    int32_t *lof = (int32_t *)arena_alloc(
        a, (size_t)(blog.count ? blog.count : 1) * sizeof(int32_t));
    for (int32_t k = 0; k < nlin; k++)
        lin[k].first = lin[k].last = -1;
    int32_t cur = -1;
    for (int32_t i = 0; i < blog.count; i++) {
        for (int32_t k = 0; blog.v[i].type == REC_BRANCH && k < nlin; k++) {
            if (strcmp(blog.v[i].id, lin[k].id) == 0) {
                cur = k;
                lin[k].first = i;
            }
        }
        lof[i] = cur;
        if (cur >= 0)
            lin[cur].last = i;
    }
    for (int32_t k = 0; k < nlin; k++) {
        if (lin[k].first < 0) {
            err_out(json, "log_broken",
                    "branch %s's history does not hold %s's branch record",
                    key, lin[k].name);
            goto done;
        }
    }
    int32_t bi = lin[0].first;
    const Rec *brec = &blog.v[lin[nlin - 1].first];
    if (find_hash(&plog, blog.v[bi].base) < 0) {
        err_out(json, "unrelated_history",
                "branch %s did not start from this folder's history", key);
        goto done;
    }
    /* After the base check: an unrelated branch's record does not chain
     * from this folder's history either, and should be named as such. */
    if (!blog.chain_ok) {
        err_out(json, "log_broken", "branch %s's history is broken: %s", key,
                blog.chain_err);
        goto done;
    }

    /* What earlier merges took in of each branch, and the sessions they
     * carried: a session adopted before, straight from its branch or by way
     * of a branch that had adopted it, is not adopted again. */
    StrSet stopped_before, ended;
    strset_init(&stopped_before, a);
    strset_init(&ended, a); /* branch session_end hashes adopted here */
    Map adopted_sessions = {0}; /* branch session_start hash -> our id */
    for (int32_t i = 0; i < plog.count; i++) {
        const Rec *p = &plog.v[i];
        if (p->type == REC_MERGE) {
            for (int32_t k = 0; k < nlin; k++) {
                if (strcmp(p->branch, lin[k].id) != 0)
                    continue;
                lin[k].head = p->head;
                for (int32_t s = 0; s < p->stopped_n; s++)
                    strset_add(&stopped_before, p->stopped_file[s]);
            }
        } else if (p->type == REC_SESSION_START && p->from) {
            map_put(a, &adopted_sessions, p->from, p->id);
        } else if (p->type == REC_SESSION_END && p->from) {
            strset_add(&ended, p->from);
        }
    }
    int32_t upto = bi; /* the stream's last record already taken in here */
    for (int32_t k = 0; k < nlin; k++) {
        if (!lin[k].head)
            continue;
        int32_t h = find_hash(&blog, lin[k].head);
        if (h >= lin[k].first) {
            if (h > upto)
                upto = h;
        } else if (k == nlin - 1) {
            err_out(json, "log_broken",
                    "branch %s's history no longer holds %.7s, where the "
                    "last merge stopped",
                    key, lin[k].head);
            goto done;
        } else if (lin[k].last > upto) {
            upto = lin[k].last; /* taken in past what this stream holds */
        }
    }
    int32_t start = upto + 1;
    Map starts = {0}; /* branch session id -> its session_start's hash */
    for (int32_t i = bi + 1; i < blog.count; i++) {
        if (blog.v[i].type == REC_SESSION_START)
            map_put(a, &starts, blog.v[i].id, blog.v[i].hash);
    }

    /* A run of this merge that stopped part-way (a crash, a full disk) left
     * some records adopted and no merge record: they are found by their
     * from links. When nothing else was recorded since, the run is redone
     * against this folder's history from before them, and what it appended
     * is not appended again, so the result is the uninterrupted merge's.
     * Otherwise its commits come out already done. */
    StrSet newer;
    strset_init(&newer, a);
    for (int32_t i = start; i < blog.count; i++)
        strset_add(&newer, blog.v[i].hash);
    Map done_from = {0}; /* branch record hash -> its copy already here */
    for (int32_t i = 0; i < plog.count; i++) {
        const Rec *p = &plog.v[i];
        if (p->from && strset_has(&newer, p->from))
            map_put(a, &done_from, p->from, p->id);
    }
    int32_t parent_at = merge_redo_point(&plog, &newer);
    /* A branch's amendments land on the commits adopted here for the ones
     * they name: branch commit hash -> its copy's hash. An amendment that
     * came here by another route (a branch between) is not carried twice. */
    Map copy_of = {0};
    StrSet amends_here;
    strset_init(&amends_here, a);
    for (int32_t i = 0; i < plog.count; i++) {
        const Rec *p = &plog.v[i];
        if (p->type == REC_COMMIT && p->from)
            map_put(a, &copy_of, p->from, p->hash);
        else if (p->type == REC_AMEND && p->from)
            strset_add(&amends_here, p->from);
    }
    int32_t amends_carried = 0, amends_left = 0;

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
    const char **already = NULL; /* branch commits this folder had made */
    size_t nalready = 0, acap = 0;
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
            for (size_t k = 0; k < n; k++)
                lin[lof[idx[k]]].left++;
            continue;
        }
        bool base_has, parent_has;
        Lines base = file_at(a, &blog, files[f], upto, &base_has);
        Lines parent = file_at(a, &plog, files[f], parent_at,
                               &parent_has);
        Placement p;
        adopt_place(a, base, parent, parent_has, (const Rec *const *)mine,
                    (int32_t)n, &p);
        int32_t last = -1; /* the last commit placed, not already done */
        for (int32_t k = 0; k < p.placed; k++) {
            Lin *l = &lin[lof[idx[k]]];
            if (p.already[k]) { /* seen, and nothing to adopt */
                ARENA_GROW(a, already, nalready, acap, const char *);
                already[nalready++] = mine[k]->hash;
                ARENA_GROW(a, l->already, l->nalready, l->acap, const char *);
                l->already[l->nalready++] = mine[k]->hash;
                continue;
            }
            at[idx[k]] = p.start[k];
            eof[idx[k]] = p.eof_nl[k];
            adopted++;
            l->adopted++;
            last = k;
        }
        left += (int32_t)n - p.placed;
        /* A stopped file stops for every branch of the chain with commits
         * to it left: each branch's merge record names its first one. */
        for (int32_t k = p.placed; k < (int32_t)n; k++) {
            Lin *l = &lin[lof[idx[k]]];
            l->left++;
            if (l->nstop > 0 && l->stop_file[l->nstop - 1] == files[f])
                continue;
            size_t s2 = l->nstop;
            ARENA_GROW(a, l->stop_file, l->nstop, l->scap, const char *);
            ARENA_GROW(a, l->stop_at, s2, l->scap2, const char *);
            l->stop_file[l->nstop] = files[f];
            l->stop_at[l->nstop] = mine[k]->hash;
            l->nstop++;
        }
        if (last >= 0) {
            changed[f] = true;
            results[f] = p.result;
            gone[f] = strcmp(mine[last]->op, "delete") == 0;
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
    for (int32_t k = 0; !dry && k < nlin; k++) {
        for (int32_t j = 0; j < lin[k].nown; j++) {
            const OwnChunk *c = &lin[k].own[j];
            if (!c->write)
                continue;
            if (!plat_mkdirs(repo.hist.dir) ||
                !hist_write_chunk(repo.hist.dir, c->name, c->data, c->len)) {
                err_out(json, "io_error", "cannot copy %s into %s", c->name,
                        repo.hist.dir);
                goto done;
            }
        }
    }

    /* Appending, in the branch's order. */
    /* For tests: stop with an I/O error after this many records, as a crash
     * or a full disk would. */
    const char *fail_env = getenv("LAP_TEST_MERGE_FAIL_AFTER");
    int32_t fail_after = fail_env ? atoi(fail_env) : -1, appended = 0;
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
                const char *prior = map_get(&adopted_sessions, b->hash);
                if (!prior && b->from) /* adopted here by another route */
                    prior = map_get(&adopted_sessions, b->from);
                if (prior) {
                    map_put(a, &adopted_sessions, b->hash, prior);
                    continue;
                }
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
                if (!ours || strset_has(&ended, b->hash) ||
                    (b->from && strset_has(&ended, b->from)))
                    continue; /* not adopted, or already ended here */
                rec.type = REC_SESSION_END;
                rec.id = ours;
                rec.user = NULL;
            } else if (b->type == REC_COMMIT && at[i] > 0) {
                const char *had = map_get(&done_from, b->hash);
                if (had) { /* appended by an interrupted run of this merge */
                    report_id(&ids, json, had, b->hash);
                    continue;
                }
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
            } else if (b->type == REC_AMEND) {
                if (strset_has(&amends_here, b->hash) ||
                    (b->from && strset_has(&amends_here, b->from)))
                    continue; /* carried already */
                /* the commit it names, adopted here straight from this
                 * branch or from the branch that commit came from */
                const char *of = map_get(&copy_of, b->of);
                int32_t j = of ? -1 : find_hash(&blog, b->of);
                if (j >= 0 && blog.v[j].from)
                    of = map_get(&copy_of, blog.v[j].from);
                if (!of) { /* not adopted: it stays in the branch */
                    amends_left++;
                    continue;
                }
                amends_carried++;
                if (map_get(&done_from, b->hash))
                    continue; /* appended by an interrupted run */
                rec.type = REC_AMEND;
                rec.of = of;
                rec.intent = b->intent;
                rec.behavior = b->behavior;
                rec.forced = b->forced;
            } else {
                continue;
            }
            if (fail_after >= 0 && appended++ == fail_after) {
                err_out(json, "io_error", "stopped after %d records "
                        "(LAP_TEST_MERGE_FAIL_AFTER)", fail_after);
                goto done;
            }
            if (!repo_append(&repo, &rec, err, sizeof err)) {
                err_out(json, "io_error", "%s", err);
                goto done;
            }
            if (rec.type == REC_COMMIT) {
                report_id(&ids, json, rec.id, b->hash);
                map_put(a, &copy_of, b->hash, arena_strdup(a, rec.hash));
            }
        }
        /* One merge record per branch of the chain with anything new: a
         * branch it started from advances to the base of the next. */
        for (int32_t k = 0; k < nlin; k++) {
            const Lin *l = &lin[k];
            if (l->last <= upto)
                continue;
            Rec m;
            memset(&m, 0, sizeof m);
            m.type = REC_MERGE;
            m.branch = l->id;
            m.name = l->name;
            m.head = blog.v[l->last].hash;
            m.adopted = l->adopted;
            m.left = l->left;
            m.stopped_file = l->stop_file;
            m.stopped_at = l->stop_at;
            m.stopped_n = (int32_t)l->nstop;
            m.already = l->already;
            m.already_n = (int32_t)l->nalready;
            m.user = repo_user(&repo);
            if (fail_after >= 0 && appended++ == fail_after) {
                err_out(json, "io_error", "stopped after %d records "
                        "(LAP_TEST_MERGE_FAIL_AFTER)", fail_after);
                goto done;
            }
            if (!repo_append(&repo, &m, err, sizeof err)) {
                err_out(json, "io_error", "%s", err);
                goto done;
            }
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
    int32_t total = adopted + left + (int32_t)nalready;
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
        sb_puts(&sb, "],\"already\":[");
        for (size_t s = 0; s < nalready; s++)
            sb_printf(&sb, s ? ",\"%s\"" : "\"%s\"", already[s]);
        sb_printf(&sb, "],\"amendments\":{\"carried\":%d,\"left\":%d}",
                  amends_carried, amends_left);
        sb_printf(&sb, ",\"commits\":[%s]}", ids.len ? sb_finish(&ids) : "");
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
        for (size_t s = 0; s < nalready; s++)
            printf("  already done here: #%.7s (this folder made the same "
                   "change)\n",
                   already[s]);
        if (amends_carried > 0)
            printf("  amendments carried: %d (lap amend on the branch)\n",
                   amends_carried);
        if (amends_left > 0)
            printf("  amendments left in the branch: %d (their commits were "
                   "not adopted)\n",
                   amends_left);
        if (left > 0 && !dry)
            printf("what is left shows in lap status: commit it as usual, "
                   "citing the branch commits it stands for (#<hash>)\n");
    }
    rc = LAP_EXIT_OK;
done:
    repo_close(&repo);
    return rc;
}
