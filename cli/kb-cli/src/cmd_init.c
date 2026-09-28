#include "cmd.h"
#include "help.h"

/* `kb init` — create a store.
 *
 * The store is a directory, not a database, and §1.6 fixes its shape: two
 * append-only logs, a content-addressed blob directory, and an `index/` that
 * is entirely derived. `store_create` builds all of it, including the
 * `.gitignore` that ignores `index/` and nothing else — §1.5 says the logs and
 * the blobs are committed so a collaborator can clone, run `kb rebuild`, and
 * have the corpus without re-fetching or re-embedding anything.
 *
 * INITIALISING IS THE ONE OPERATION THAT DOES NOT WALK UP. Every other
 * command uses the first `.kb/` at or above the current directory — but that
 * rule is stated in terms of a store that already exists, and here the
 * question is where to make one. Walking up would mean `kb init` in a
 * subdirectory silently doing nothing because an ancestor already had a
 * `.kb`. So this creates in the current directory, always, and says plainly
 * when a store is already there. It takes no path: there is no store outside
 * the workspace for it to make. */

/* The flags kb init accepts: the ones its --help lists (help.c). */
#define VALUE_FLAGS help_values("init")
#define BOOL_FLAGS help_bools("init")

int32_t cmd_init(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }

    char dir[KB_PATH_MAX];
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

    /* AN EXISTING STORE IS AN ERROR, NOT A NO-OP. `kb init` is the one command
     * whose whole effect is creating something, so "it was already there" is a
     * different outcome from "I made it" and the caller has to be able to tell
     * — silently succeeding would let a script believe it had a fresh store
     * and then be surprised by what is in it. Nothing is overwritten either
     * way; the logs are the truth and init never touches an existing one. */
    /* THE HOME DIRECTORY'S `.kb` IS THE MACHINE'S MODELS (§8), and §1.4 has
     * no store there: one would be found by every folder under the home
     * directory that has none of its own. */
    if (store_is_home(here)) {
        err_out(json, "init_failed",
                "the home directory holds kb's models, not a store; run kb init in a workspace");
        return KB_EXIT_ERR;
    }
    if (plat_is_dir(dir)) {
        /* `init_failed`, not a code of this command's own invention. §11's
         * table is not the whole vocabulary — the CLI also prints
         * `corrupt_log`, `internal`, `unknown_command` and `usage` — and
         * `kb-js` carries the full list with a guard that fails on any code
         * the C can print and the reader has never heard of. A new spelling
         * here would reach a caller flagged as unrecognised. */
        err_out(json, "init_failed", "a store already exists at %s", dir);
        return KB_EXIT_ERR;
    }

    char err[512];
    if (!store_create(a, dir, err, sizeof err)) {
        err_out(json, "internal", "%s", err);
        return KB_EXIT_FATAL;
    }

    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_puts(&sb, "{\"ok\":true,\"path\":");
        json_escape_c(&sb, dir);
        sb_putc(&sb, '}');
        puts(sb_finish(&sb));
    } else {
        printf("store created at %s\n", dir);
    }
    return KB_EXIT_OK;
}
