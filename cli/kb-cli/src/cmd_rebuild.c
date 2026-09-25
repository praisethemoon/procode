#include "cmd.h"

/* POST /rebuild (§7): reconstruct every derived structure from the logs and
 * the blobs.
 *
 * This is the command that makes §1.5's claim true — a collaborator clones
 * the repository, runs it, and has the whole research corpus without
 * re-fetching anything. So it has to be correct from nothing at all: no
 * index/ directory, a truncated fts.db, an fts.db written by another
 * version. All three are the same job, because the file is never read
 * before it is replaced.
 *
 * index/model.json is the one derived file that cannot be derived. §1.6
 * lists it under index/ and calls everything there reconstructible, but §8
 * defines it as the record of the configuration the index was BUILT with —
 * which is exactly the thing the logs do not contain. A rebuild therefore
 * writes it only when it is absent, and otherwise obeys it: reproducing the
 * chunk ids the log reserved requires chunking the way the store was
 * chunked, not the way this build would chunk today.
 */

static const char *const VALUE_FLAGS[] = {NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

int32_t cmd_rebuild(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }

    char err[512];
    char dir[KB_PATH_MAX];
    if (!store_resolve(dir, sizeof dir, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return KB_EXIT_ERR;
    }

    Store s;
    const char *code;
    if (!store_open(a, &s, dir, true, err, sizeof err, &code)) {
        err_out(json, code, "%s", err);
        return strcmp(code, "internal") == 0 ? KB_EXIT_FATAL : KB_EXIT_ERR;
    }
    /* A store that has never been written has no model.json; one that
     * has keeps the parameters it was built with (§8). */
    if (!store_write_chunk_params(&s, err, sizeof err)) {
        store_close(&s);
        err_out(json, "internal", "%s", err);
        return KB_EXIT_FATAL;
    }
    uint32_t ndocs = 0, missing = 0;
    FtsBuildStats stats;
    if (!index_rebuild(a, &s, &ndocs, &missing, &stats, err, sizeof err)) {
        store_close(&s);
        err_out(json, "internal", "%s", err);
        return KB_EXIT_FATAL;
    }
    uint64_t bytes = 0;
    char path[KB_PATH_MAX];
    fts_path(&s, path, sizeof path);
    plat_file_size(path, &bytes);

    StrBuf sb;
    sb_init(&sb, a);
    if (json) {
        sb_puts(&sb, "{\"ok\":true,\"path\":");
        json_escape_c(&sb, s.dir);
        sb_printf(&sb,
                  ",\"documents\":%lu,\"chunks\":%lu,\"terms\":%lu,"
                  "\"indexBytes\":%llu,\"missingBlobs\":%lu,"
                  "\"mismatched\":%lu}",
                  (unsigned long)ndocs, (unsigned long)stats.chunks,
                  (unsigned long)stats.terms, (unsigned long long)bytes,
                  (unsigned long)missing, (unsigned long)stats.mismatched);
        puts(sb_finish(&sb));
    } else {
        sb_printf(&sb, "%lu documents, %lu chunks, %lu terms, %llu bytes\n",
                  (unsigned long)ndocs, (unsigned long)stats.chunks,
                  (unsigned long)stats.terms, (unsigned long long)bytes);
        if (missing)
            sb_printf(&sb, "%lu document%s blob is missing\n",
                      (unsigned long)missing, missing == 1 ? "'s" : "s'");
        if (stats.mismatched)
            sb_printf(&sb,
                      "%lu document%s re-chunked to a different count; a "
                      "reindex is owed\n",
                      (unsigned long)stats.mismatched,
                      stats.mismatched == 1 ? "" : "s");
        fputs(sb_finish(&sb), stdout);
    }
    store_close(&s);
    return KB_EXIT_OK;
}
