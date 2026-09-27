#include "cmd.h"

/* lap rebuild: drop every derived cache and reconstruct it from the log —
 * the executable proof that log.jsonl alone is the truth. --verify fails
 * the command when the log's hash chain is broken.
 */
int32_t cmd_rebuild(Arena *a, int32_t argc, char **argv) {
    static const char *const bool_flags[] = {"--json", "--verify", NULL};
    bool json = has_flag(argc, argv, NULL, "--json");
    if (!flags_known(argc, argv, NULL, bool_flags))
        return LAP_EXIT_ERR;
    bool verify = has_flag(argc, argv, NULL, "--verify");

    Repo repo;
    char err[512];
    if (!repo_open(a, &repo, true, err, sizeof err)) {
        err_out(json, "no_repo", "%s", err);
        return LAP_EXIT_ERR;
    }
    int32_t rc = LAP_EXIT_ERR;

    RecLog log;
    if (!rec_log_load(a, repo.logpath, &log, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        goto done;
    }
    if (verify && !log.chain_ok) {
        err_out(json, "chain_broken", "%s", log.chain_err);
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
        printf("{\"ok\":true,\"records\":%d,\"chain_ok\":%s}\n", log.count,
               log.chain_ok ? "true" : "false");
    } else {
        printf("rebuilt all caches from %d records%s\n", log.count,
               log.chain_ok ? " (chain ok)" : " (CHAIN BROKEN)");
    }
    rc = log.chain_ok || !verify ? LAP_EXIT_OK : LAP_EXIT_ERR;

done:
    repo_close(&repo);
    return rc;
}
