#include "cmd.h"

typedef struct {
    char **paths;
    size_t n, cap;
} PathList;

typedef struct {
    Arena *a;
    Repo *repo;
    const Ignore *ig;
    PathList files;
} StatusWalk;

static void path_push(Arena *a, PathList *pl, const char *p) {
    ARENA_GROW(a, pl->paths, pl->n, pl->cap, char *);
    pl->paths[pl->n++] = arena_strdup(a, p);
}

static WalkAction on_entry(const char *rel, bool is_dir, void *ud) {
    StatusWalk *sw = (StatusWalk *)ud;
    if (ignore_match(sw->ig, rel, is_dir))
        return is_dir ? WALK_SKIP_DIR : WALK_CONT;
    if (!is_dir)
        path_push(sw->a, &sw->files, rel);
    return WALK_CONT;
}

static WalkAction on_shadow_entry(const char *rel, bool is_dir, void *ud) {
    StatusWalk *sw = (StatusWalk *)ud;
    if (!is_dir) {
        /* shadow file with no working counterpart => deleted */
        char wpath[LAP_PATH_MAX];
        snprintf(wpath, sizeof wpath, "%s/%s", sw->repo->root, rel);
        if (!plat_is_file(wpath))
            path_push(sw->a, &sw->files, rel);
    }
    return WALK_CONT;
}

/* qsort comparator: must return int per the C standard API */
static int cmp_paths(const void *pa, const void *pb) {
    return strcmp(*(const char *const *)pa, *(const char *const *)pb);
}

int32_t cmd_status(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, NULL, "--json");
    Repo repo;
    char err[512];
    if (!repo_open(a, &repo, false, err, sizeof err)) {
        err_out(json, "no_repo", "%s", err);
        return LAP_EXIT_ERR;
    }

    StatusWalk sw;
    memset(&sw, 0, sizeof sw);
    sw.a = a;
    sw.repo = &repo;
    sw.ig = ignore_load(a, repo.root);
    plat_walk(a, repo.root, on_entry, &sw);
    char shadow_root[LAP_PATH_MAX];
    snprintf(shadow_root, sizeof shadow_root, "%s/%s", repo.lapdir,
             LAP_SHADOW_NAME);
    plat_walk(a, shadow_root, on_shadow_entry, &sw);

    if (sw.files.n > 1)
        qsort(sw.files.paths, sw.files.n, sizeof(char *), cmp_paths);

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
            sb_printf(&sb, " \"%.*s\"\n", mlen, repo.active_session_msg);
        } else {
            sb_puts(&sb, "session: none (start one with: lap session start "
                         "\"...\")\n");
        }
    }

    const char *prev = NULL;
    for (size_t i = 0; i < sw.files.n; i++) {
        const char *rel = sw.files.paths[i];
        if (prev && strcmp(prev, rel) == 0)
            continue; /* dedupe overlap between walks */
        prev = rel;

        FileDiff fd;
        if (!file_diff_load(a, &repo, rel, &fd, err, sizeof err))
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
            continue; /* clean */
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
            sb_printf(&sb, "  %s", rel);
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
