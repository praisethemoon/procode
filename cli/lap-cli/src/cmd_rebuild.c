#include "cmd.h"
#include "help.h"

/* lap rebuild: drop every derived cache and reconstruct it from the log —
 * the executable proof that the log (.lap/log/) alone is the truth.
 * --verify fails the command when the log's hash chain is broken.
 */
int32_t cmd_rebuild(Arena *a, int32_t argc, char **argv) {
    FlagSets fs;
    help_flag_sets("rebuild", &fs);
    const char *const *bool_flags = fs.bools;
    bool json = has_flag(argc, argv, NULL, "--json");
    if (!flags_known(argc, argv, NULL, bool_flags))
        return LAP_EXIT_ERR;
    bool verify = has_flag(argc, argv, NULL, "--verify");

    Repo repo;
    char err[512];
    if (!repo_open(a, &repo, true, err, sizeof err)) {
        err_out(json, repo_error_code(), "%s", err);
        return LAP_EXIT_ERR;
    }
    int32_t rc = LAP_EXIT_ERR;

    /* the log's count and chain, read a chunk at a time */
    HistScan scan;
    if (!hist_scan(a, &repo.hist, &scan, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        goto done;
    }
    int32_t records = scan.records;
    bool chain_ok = scan.chain_ok;
    if (verify && !chain_ok) {
        /* a broken chain is worded as verify words it, from the whole log
         * — read only in this case */
        Arena *la = arena_new(1 << 16);
        RecLog log;
        if (repo_log_load(la, &repo, &log, err, sizeof err))
            snprintf(err, sizeof err, "%s", log.chain_err);
        arena_free(la);
        err_out(json, "chain_broken", "%s", err);
        goto done;
    }

    /* force a from-scratch index so the rebuild proves the contract */
    const char *caches[] = {"index", "paths", "heads", LAP_STATE_NAME};
    for (size_t i = 0; i < sizeof caches / sizeof caches[0]; i++) {
        char p[LAP_PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", repo.lapdir, caches[i]);
        plat_remove_file(p);
    }

    if (!repo_rebuild(a, &repo, err, sizeof err)) {
        err_out(json, "rebuild_failed", "%s", err);
        goto done;
    }
    if (json) {
        printf("{\"ok\":true,\"records\":%d,\"chain_ok\":%s}\n", records,
               chain_ok ? "true" : "false");
    } else {
        printf("rebuilt all caches from %d records%s\n", records,
               chain_ok ? " (chain ok)" : " (CHAIN BROKEN)");
    }
    rc = chain_ok || !verify ? LAP_EXIT_OK : LAP_EXIT_ERR;

done:
    repo_close(&repo);
    return rc;
}
