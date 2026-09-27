#include "cmd.h"

/* lap rebuild: drop every derived cache and reconstruct it from the log —
 * the executable proof that the log (.lap/log/) alone is the truth.
 * --verify fails the command when the log's hash chain is broken.
 */
/* Counts the history's records and checks their chain one chunk at a
 * time (records never span chunks), so this costs one chunk's memory, not
 * the history's. False only when a chunk cannot be read or a record
 * cannot be parsed. */
static bool stream_chain(const Hist *h, int32_t *records, bool *chain_ok,
                         char *chain_err, size_t cerrsz, char *err,
                         size_t errsz) {
    Arena *ca = arena_new(1 << 16), *ra = arena_new(1 << 16);
    char last[65];
    snprintf(last, sizeof last, "%s", LAP_HASH_ZERO);
    *records = 0;
    *chain_ok = true;
    bool ok = true;
    for (int32_t k = 0; ok && k < h->n; k++) {
        arena_reset(ca);
        char *data;
        if (h->v[k].size == 0)
            continue;
        if (!hist_read(ca, h, h->v[k].start, (size_t)h->v[k].size, &data)) {
            snprintf(err, errsz, "cannot read %s", h->v[k].name);
            ok = false;
            break;
        }
        size_t len = (size_t)h->v[k].size, start = 0;
        for (int32_t line_no = 1; start < len; line_no++) {
            const char *nl = memchr(data + start, '\n', len - start);
            if (!nl)
                break; /* a torn tail: never acknowledged */
            size_t n = (size_t)(nl - (data + start));
            if (n > 0) {
                arena_reset(ra);
                Rec rec;
                char derr[128];
                if (!rec_decode(ra, data + start, n, &rec, derr,
                                sizeof derr)) {
                    snprintf(err, errsz, "%s line %d: %s", h->v[k].name,
                             line_no, derr);
                    ok = false;
                    break;
                }
                if (*chain_ok && strcmp(rec.prev, last) != 0) {
                    *chain_ok = false;
                    snprintf(chain_err, cerrsz,
                             "hash chain broken at %s line %d", h->v[k].name,
                             line_no);
                }
                snprintf(last, sizeof last, "%s", rec.hash);
                (*records)++;
            }
            start += n + 1;
        }
    }
    arena_free(ca);
    arena_free(ra);
    return ok;
}

int32_t cmd_rebuild(Arena *a, int32_t argc, char **argv) {
    static const char *const bool_flags[] = {"--json", "--verify", NULL};
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
    int32_t records;
    bool chain_ok;
    char chain_err[256] = "";
    if (!stream_chain(&repo.hist, &records, &chain_ok, chain_err,
                      sizeof chain_err, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        goto done;
    }
    if (verify && !chain_ok) {
        err_out(json, "chain_broken", "%s", chain_err);
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
