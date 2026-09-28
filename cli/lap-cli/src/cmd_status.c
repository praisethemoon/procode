#include "cmd.h"
#include "statcache.h"

/* A path status looks at: from the working-tree walk (with the stat the walk
 * took), or tracked by the index but not seen by the walk (deleted, or
 * ignored). head is the index entry of its last commit; -1 when untracked
 * or when there is no index to ask. */
typedef struct {
    const char *path;
    PlatStat st;
    bool walked;
    int64_t head;
} Seen;

typedef struct {
    Seen *v;
    size_t n, cap;
} SeenList;

typedef struct {
    Arena *a;
    Repo *repo;
    const Ignore *ig;
    SeenList files;
} StatusWalk;

static void seen_push(Arena *a, SeenList *l, const char *p, const PlatStat *st,
                      int64_t head) {
    ARENA_GROW(a, l->v, l->n, l->cap, Seen);
    Seen *s = &l->v[l->n++];
    s->path = arena_strdup(a, p);
    s->walked = st != NULL;
    if (st)
        s->st = *st;
    s->head = head;
}

static WalkAction on_entry(const char *rel, bool is_dir, const PlatStat *st,
                           void *ud) {
    StatusWalk *sw = (StatusWalk *)ud;
    if (ignore_match(sw->ig, rel, is_dir))
        return is_dir ? WALK_SKIP_DIR : WALK_CONT;
    if (is_dir && repo_nested(sw->repo->root, rel))
        return WALK_SKIP_DIR; /* another repository's files */
    if (!is_dir)
        seen_push(sw->a, &sw->files, rel, st, -1);
    return WALK_CONT;
}

/* qsort comparator: must return int per the C standard API */
static int cmp_seen(const void *pa, const void *pb) {
    return strcmp(((const Seen *)pa)->path, ((const Seen *)pb)->path);
}

typedef struct {
    const char *path;
    int64_t head;
} Tracked;

static int cmp_tracked(const void *pa, const void *pb) {
    return strcmp(((const Tracked *)pa)->path, ((const Tracked *)pb)->path);
}

/* Every file whose last commit is not a delete is tracked: from the index,
 * each with its head. */
static size_t tracked_from_index(Arena *a, const Idx *ix, Tracked **out) {
    Tracked *t = (Tracked *)arena_alloc(
        a, (size_t)(ix->npaths ? ix->npaths : 1) * sizeof(Tracked));
    size_t nt = 0;
    for (int32_t f = 0; f < ix->npaths; f++) {
        int64_t head = ix->heads[f].head;
        if (head >= 0 && ix->v[head].op != IDX_OP_DELETE)
            t[nt++] = (Tracked){ix->paths[f], head};
    }
    *out = t;
    return nt;
}

size_t log_tracked_files(Arena *a, const RecLog *log, const char ***out) {
    StrMap last; /* path -> "d" when its last commit deleted it, else "" */
    memset(&last, 0, sizeof last);
    const char **order = (const char **)arena_alloc(
        a, (size_t)(log->count ? log->count : 1) * sizeof(char *));
    size_t norder = 0;
    for (int32_t i = 0; i < log->count; i++) {
        const Rec *c = &log->v[i];
        if (c->type != REC_COMMIT || !c->file)
            continue;
        if (!strmap_get(&last, c->file))
            order[norder++] = c->file;
        strmap_put(a, &last, c->file,
                   strcmp(c->op, "delete") == 0 ? "d" : "");
    }
    size_t n = 0;
    for (size_t i = 0; i < norder; i++) {
        if (strcmp(strmap_get(&last, order[i]), "d") != 0)
            order[n++] = order[i];
    }
    *out = order;
    return n;
}

/* The same from the history itself, when there is no index (a clone holds
 * only log/): no heads, so nothing is cached for them. */
static size_t tracked_from_log(Arena *a, const RecLog *log, Tracked **out) {
    const char **paths;
    size_t n = log_tracked_files(a, log, &paths);
    Tracked *t =
        (Tracked *)arena_alloc(a, (size_t)(n ? n : 1) * sizeof(Tracked));
    for (size_t i = 0; i < n; i++)
        t[i] = (Tracked){paths[i], -1};
    *out = t;
    return n;
}

/* One merge of the sorted walk against the sorted tracked files gives each
 * walked file its head and adds the tracked files the walk did not see —
 * the deleted ones, and those ignored since they were recorded. */
static void join_tracked(Tracked *t, size_t nt, Arena *a, SeenList *files) {
    qsort(t, nt, sizeof *t, cmp_tracked);
    size_t walked = files->n, i = 0, j = 0;
    while (j < nt) {
        int c = i < walked ? strcmp(files->v[i].path, t[j].path) : 1;
        if (c < 0) {
            i++;
        } else if (c == 0) {
            files->v[i++].head = t[j++].head;
        } else {
            seen_push(a, files, t[j].path, NULL, t[j].head);
            j++;
        }
    }
}

int32_t cmd_status(Arena *a, int32_t argc, char **argv) {
    static const char *const bool_flags[] = {"--json", NULL};
    bool json = has_flag(argc, argv, NULL, "--json");
    if (!flags_known(argc, argv, NULL, bool_flags))
        return LAP_EXIT_ERR;
    Repo repo;
    char err[512];
    if (!repo_open(a, &repo, false, err, sizeof err)) {
        err_out(json, repo_error_code(), "%s", err);
        return LAP_EXIT_ERR;
    }

    /* taken before any file is looked at: the racy rule's reference */
    int64_t start = plat_now_sec();
    StatusWalk sw;
    memset(&sw, 0, sizeof sw);
    sw.a = a;
    sw.repo = &repo;
    sw.ig = ignore_load(a, repo.root);
    const char **undirs; /* folders the walk could not open */
    size_t nundirs;
    plat_walk_report(a, repo.root, on_entry, &sw, &undirs, &nundirs);
    if (sw.files.n > 1)
        qsort(sw.files.v, sw.files.n, sizeof(Seen), cmp_seen);
    Idx *ix = idx_ready(a, &repo);
    Tracked *tracked = NULL;
    size_t ntracked = 0;
    if (ix) {
        ntracked = tracked_from_index(a, ix, &tracked);
    } else {
        /* no index: the history says what is tracked, as the index would;
         * the shadow store may be missing too, and never decides it */
        RecLog log;
        if (!repo_log_load(a, &repo, &log, err, sizeof err)) {
            err_out(json, "log_unreadable", "%s", err);
            return LAP_EXIT_ERR;
        }
        ntracked = tracked_from_log(a, &log, &tracked);
    }
    join_tracked(tracked, ntracked, a, &sw.files);
    if (sw.files.n > 1)
        qsort(sw.files.v, sw.files.n, sizeof(Seen), cmp_seen);

    /* The stat cache needs the index's heads to key its entries on. */
    StatCache cached, fresh;
    statcache_init(&cached, a);
    statcache_init(&fresh, a);
    if (ix)
        statcache_load(&cached, &repo);
    size_t verified = 0; /* entries fresh gained by reading a file */

    StrBuf sb;
    sb_init(&sb, a);
    int32_t dirty = 0;

    /* the active session as it is named outside this folder */
    const char *ref = session_ref(
        a, repo.hist.parent[0] ? repo.hist.name : LAP_MAIN_LINEAGE,
        repo.active_session);
    if (json) {
        sb_puts(&sb, "{\"ok\":true,");
        if (repo.active_session[0]) {
            sb_printf(&sb, "\"session\":{\"id\":\"%s\",\"ref\":\"%s\",\"msg\":",
                      repo.active_session, ref);
            json_escape_c(&sb, repo.active_session_msg);
            sb_puts(&sb, "},");
        } else {
            sb_puts(&sb, "\"session\":null,");
        }
        sb_puts(&sb, "\"files\":[");
    } else {
        if (repo.active_session[0]) {
            /* show only the first line of the session message */
            const char *nl = strchr(repo.active_session_msg, '\n');
            int32_t mlen = nl ? (int32_t)(nl - repo.active_session_msg)
                              : (int32_t)strlen(repo.active_session_msg);
            sb_puts(&sb, "session: ");
            sb_field(&sb, S_ACTIVE, ref, 0);
            sb_puts(&sb, " \"");
            sb_text(&sb, repo.active_session_msg, (size_t)mlen);
            sb_puts(&sb, "\"\n");
        } else {
            sb_puts(&sb, "session: none (start one with: lap session start "
                         "\"...\")\n");
        }
    }

    /* Each file is read and diffed in fa, emptied before the next: what the
     * output needs is copied into sb as the file is reported. */
    Arena *fa = arena_new(1 << 16);
    const char *prev = NULL;
    for (size_t i = 0; i < sw.files.n; i++) {
        arena_reset(fa);
        const Seen *f = &sw.files.v[i];
        const char *rel = f->path;
        if (prev && strcmp(prev, rel) == 0)
            continue; /* dedupe overlap between walks */
        prev = rel;

        bool cacheable = f->walked && f->head >= 0;
        if (cacheable) {
            const StatEntry *e = statcache_get(&cached, rel);
            if (e && statcache_matches(e, f->head, &f->st)) {
                statcache_add(&fresh, rel, f->head, &f->st);
                continue; /* clean, known without reading it */
            }
        }

        FileDiff fd;
        bool loaded = file_diff_load(fa, &repo, rel, &fd, err, sizeof err);
        if (!loaded && !fd.unreadable)
            continue;

        const char *state = NULL;
        if (!loaded) {
            state = "unreadable"; /* never taken for deleted, or clean */
        } else if (fd.binary) {
            state = fd.shadow_exists ? "binary" : NULL; /* untracked binary:
                                                           silently skipped */
            if (!state)
                continue;
        } else if (!fd.work_exists && fd.shadow_exists) {
            state = "deleted";
        } else if (fd.work_exists && !fd.shadow_exists) {
            state = "new";
        } else if (fd.regions.count > 0) {
            state = "modified";
        } else {
            /* clean: the stat the walk took before this read is safe to
             * cache once it is older than the run's first second */
            if (cacheable && fd.work_exists && fd.shadow_exists &&
                statcache_settled(&f->st, start)) {
                statcache_add(&fresh, rel, f->head, &f->st);
                verified++;
            }
            continue;
        }
        dirty++;

        if (json) {
            if (dirty > 1)
                sb_putc(&sb, ',');
            sb_puts(&sb, "{\"path\":");
            json_escape_c(&sb, rel);
            sb_printf(&sb, ",\"state\":\"%s\"", state);
            if (strcmp(state, "modified") == 0) {
                sb_puts(&sb, ",\"edits\":[");
                for (int32_t e = 0; e < fd.regions.count; e++) {
                    Region *rg = &fd.regions.v[e];
                    if (e)
                        sb_putc(&sb, ',');
                    sb_printf(&sb,
                              "{\"index\":%d,\"old_start\":%d,\"old_lines\":"
                              "%d,\"new_start\":%d,\"new_lines\":%d}",
                              e + 1, rg->old_start, rg->old_lines,
                              rg->new_start, rg->new_lines);
                }
                sb_putc(&sb, ']');
            } else if (strcmp(state, "new") == 0) {
                sb_printf(&sb, ",\"lines\":%d", fd.work.count);
            }
            sb_putc(&sb, '}');
        } else {
            Style st = strcmp(state, "modified") == 0   ? S_CHANGED
                       : strcmp(state, "new") == 0      ? S_ADDED
                       : strcmp(state, "deleted") == 0  ? S_REMOVED
                                                        : S_MUTED;
            sb_puts(&sb, "  ");
            sb_field(&sb, st, state, 8);
            sb_puts(&sb, "  ");
            sb_text(&sb, rel, strlen(rel));
            if (strcmp(state, "modified") == 0) {
                sb_printf(&sb, "  (%d edit%s)\n", fd.regions.count,
                          fd.regions.count == 1 ? "" : "s");
                for (int32_t e = 0; e < fd.regions.count; e++) {
                    char desc[128];
                    region_describe(&fd.regions.v[e], desc, sizeof desc);
                    sb_printf(&sb, "      [%d] ", e + 1);
                    sb_field(&sb, S_MUTED, desc, 0);
                    sb_putc(&sb, 0x0a);
                }
            } else if (strcmp(state, "new") == 0) {
                sb_printf(&sb, "  (%d line%s)\n", fd.work.count,
                          fd.work.count == 1 ? "" : "s");
            } else if (strcmp(state, "deleted") == 0) {
                sb_putc(&sb, 0x0a);
            } else if (strcmp(state, "unreadable") == 0) {
                sb_puts(&sb, "  (cannot be read: fix its permissions)\n");
            } else {
                sb_puts(&sb, "  (unsupported, ignored)\n");
            }
        }
    }
    arena_free(fa);
    /* a folder it cannot open hides what is in it: named, never skipped */
    for (size_t i = 0; i < nundirs; i++) {
        dirty++;
        const char *d = undirs[i][0] ? undirs[i] : ".";
        if (json) {
            if (dirty > 1)
                sb_putc(&sb, ',');
            sb_puts(&sb, "{\"path\":");
            json_escape_c(&sb, arena_printf(a, "%s/", d));
            sb_puts(&sb, ",\"state\":\"unreadable\"}");
        } else {
            sb_puts(&sb, "  ");
            sb_field(&sb, S_MUTED, "unreadable", 8);
            sb_printf(&sb, "  %s/  (a folder lap cannot open: what is in it "
                           "is unknown)\n",
                      d);
        }
    }

    /* status is a reader, and this is the one cache a reader writes: only
     * when it changed, only under a lock that happens to be free, and
     * atomically. A writer holding the lock just means no refresh. */
    if (ix && (verified > 0 || fresh.n != cached.n)) {
        char lockpath[LAP_PATH_MAX];
        snprintf(lockpath, sizeof lockpath, "%s/%s", repo.lapdir,
                 LAP_LOCK_NAME);
        PlatLock *lock = plat_trylock(a, lockpath);
        if (lock) {
            statcache_save(&fresh, &repo);
            plat_unlock(lock);
        }
    }

    if (json) {
        sb_puts(&sb, "]}");
        puts(sb_finish(&sb));
    } else {
        if (dirty == 0)
            sb_puts(&sb, "clean: working tree matches the last commit\n");
        else
            sb_printf(&sb, "%d file%s with pending changes\n", dirty,
                      dirty == 1 ? "" : "s");
        fputs(sb_finish(&sb), stdout);
    }
    return LAP_EXIT_OK;
}
