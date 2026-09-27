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
    size_t walked_n; /* files[0, walked_n): the working walk's, sorted */
} StatusWalk;

static int cmp_seen(const void *pa, const void *pb);

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
    if (!is_dir)
        seen_push(sw->a, &sw->files, rel, st, -1);
    return WALK_CONT;
}

/* Without an index, deleted files are found by walking the shadow store. */
static WalkAction on_shadow_entry(const char *rel, bool is_dir,
                                  const PlatStat *st, void *ud) {
    (void)st;
    StatusWalk *sw = (StatusWalk *)ud;
    if (is_dir)
        return WALK_CONT;
    /* Every tracked file the working walk did not list: deleted (no
     * working counterpart), or ignored since it was tracked — the index
     * lists those too, and a missing cache must not change the output. */
    Seen key;
    key.path = rel;
    if (sw->walked_n > 0 &&
        bsearch(&key, sw->files.v, sw->walked_n, sizeof(Seen), cmp_seen))
        return WALK_CONT;
    char wpath[LAP_PATH_MAX];
    snprintf(wpath, sizeof wpath, "%s/%s", sw->repo->root, rel);
    PlatStat wst;
    seen_push(sw->a, &sw->files, rel, plat_stat(wpath, &wst) ? &wst : NULL,
              -1);
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

/* With an index, every file whose last commit is not a delete is tracked.
 * One merge of the sorted walk against the sorted tracked files gives each
 * walked file its head and adds the tracked files the walk did not see —
 * the deleted ones, found without walking the shadow store. */
static void join_tracked(Arena *a, const Idx *ix, SeenList *files) {
    Tracked *t = (Tracked *)arena_alloc(
        a, (size_t)(ix->npaths ? ix->npaths : 1) * sizeof(Tracked));
    size_t nt = 0;
    for (int32_t f = 0; f < ix->npaths; f++) {
        int64_t head = ix->heads[f].head;
        if (head >= 0 && ix->v[head].op != IDX_OP_DELETE)
            t[nt++] = (Tracked){ix->paths[f], head};
    }
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
        err_out(json, "no_repo", "%s", err);
        return LAP_EXIT_ERR;
    }

    /* taken before any file is looked at: the racy rule's reference */
    int64_t start = plat_now_sec();
    StatusWalk sw;
    memset(&sw, 0, sizeof sw);
    sw.a = a;
    sw.repo = &repo;
    sw.ig = ignore_load(a, repo.root);
    plat_walk(a, repo.root, on_entry, &sw);
    if (sw.files.n > 1)
        qsort(sw.files.v, sw.files.n, sizeof(Seen), cmp_seen);
    sw.walked_n = sw.files.n;
    Idx *ix = idx_ready(a, &repo);
    if (ix) {
        join_tracked(a, ix, &sw.files);
    } else {
        char shadow_root[LAP_PATH_MAX];
        snprintf(shadow_root, sizeof shadow_root, "%s/%s", repo.lapdir,
                 LAP_SHADOW_NAME);
        plat_walk(a, shadow_root, on_shadow_entry, &sw);
    }
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

    if (json) {
        sb_puts(&sb, "{\"ok\":true,");
        if (repo.active_session[0]) {
            sb_printf(&sb, "\"session\":{\"id\":\"%s\",\"msg\":",
                      repo.active_session);
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
            sb_field(&sb, S_ACTIVE, repo.active_session, 0);
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
        if (!file_diff_load(fa, &repo, rel, &fd, err, sizeof err))
            continue;

        const char *state = NULL;
        if (fd.binary) {
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
            } else {
                sb_puts(&sb, "  (unsupported, ignored)\n");
            }
        }
    }
    arena_free(fa);

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
