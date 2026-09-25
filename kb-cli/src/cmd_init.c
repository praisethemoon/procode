#include "cmd.h"

/* `kb init` — create a store.
 *
 * The store is a directory, not a database, and §1.6 fixes its shape: two
 * append-only logs, a content-addressed blob directory, and an `index/` that
 * is entirely derived. `store_create` builds all of it, including the
 * `.gitignore` that ignores `index/` and nothing else — §1.5 says the logs and
 * the blobs are committed so a collaborator can clone, run `kb rebuild`, and
 * have the corpus without re-fetching or re-embedding anything.
 *
 * INITIALISING IS THE ONE OPERATION THAT CANNOT DEFAULT TO THE PROJECT STORE.
 * Everywhere else, §1.4's rule is that a write goes to the project store when
 * one exists — but that rule is stated in terms of a store that already
 * exists, and here the question is where to make one. Defaulting to "walk up
 * and use whatever I find" would mean `kb init` in a subdirectory silently
 * doing nothing because an ancestor already had a `.kb`. So this creates in
 * the current directory unless told otherwise, and says plainly when a store
 * is already there. */

static const char *const VALUE_FLAGS[] = {"--store", NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

int32_t cmd_init(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }

    /* `--store global` targets `~/.kb` (or `$KB_STORE`); anything else makes
     * the project store here. There is deliberately no `all`: creating two
     * stores from one command would leave the caller unsure which one the next
     * ingest lands in. */
    const char *want = flag_value(argc, argv, VALUE_FLAGS, "--store");
    char dir[KB_PATH_MAX];
    if (want && strcmp(want, "global") == 0) {
        if (!store_global_dir(dir, sizeof dir)) {
            err_out(json, "not_found", "cannot locate a global store path");
            return KB_EXIT_ERR;
        }
    } else if (want && strcmp(want, "project") != 0) {
        err_out(json, "usage", "--store expects project or global");
        return KB_EXIT_ERR;
    } else {
        char here[KB_PATH_MAX];
        if (!store_abs_path(".", here, sizeof here)) {
            err_out(json, "internal", "cannot resolve the current directory");
            return KB_EXIT_FATAL;
        }
        int32_t n = snprintf(dir, sizeof dir, "%s/%s", here, KB_DIR);
        if (n < 0 || (size_t)n >= sizeof dir) {
            err_out(json, "usage", "path is too long");
            return KB_EXIT_ERR;
        }
    }

    /* Existing is not an error worth an exit code — `kb init` is the kind of
     * command a script runs unconditionally — but it must not be reported as
     * having created anything, because a caller that believes it made a fresh
     * store will believe the store is empty. */
    bool existed = plat_is_dir(dir);

    char err[512];
    if (!existed && !store_create(a, dir, err, sizeof err)) {
        err_out(json, "internal", "%s", err);
        return KB_EXIT_FATAL;
    }

    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_puts(&sb, "{\"ok\":true,\"store\":");
        json_escape_c(&sb, dir);
        sb_printf(&sb, ",\"created\":%s}", existed ? "false" : "true");
        puts(sb_finish(&sb));
    } else {
        printf("%s %s\n", existed ? "store already at" : "store created at",
               dir);
    }
    return KB_EXIT_OK;
}
