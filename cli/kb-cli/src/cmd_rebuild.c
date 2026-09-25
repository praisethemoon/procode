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
 * It rebuilds every tier in scope rather than one, because §7's route takes
 * no store parameter and because the caller who has just cloned does not
 * know which tier was short of an index. Each tier is taken under its own
 * lock, in turn: holding both at once would make two independent stores
 * into one deadlock surface for no gain.
 *
 * index/model.json is the one derived file that cannot be derived. §1.6
 * lists it under index/ and calls everything there reconstructible, but §8
 * defines it as the record of the configuration the index was BUILT with —
 * which is exactly the thing the logs do not contain. A rebuild therefore
 * writes it only when it is absent, and otherwise obeys it: reproducing the
 * chunk ids the log reserved requires chunking the way the store was
 * chunked, not the way this build would chunk today.
 */

static const char *const VALUE_FLAGS[] = {"--store", NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

int32_t cmd_rebuild(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    StoreSel sel;
    if (!store_sel_parse(flag_value(argc, argv, VALUE_FLAGS, "--store"),
                         &sel)) {
        err_out(json, "usage", "--store expects project, global or all");
        return KB_EXIT_ERR;
    }

    char err[512];
    TierSet tiers;
    /* Resolved as a read so that "every tier that exists" is the default;
     * each one is then opened for write in turn. */
    if (!tiers_resolve(sel, false, &tiers, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return KB_EXIT_ERR;
    }

    StrBuf sb;
    sb_init(&sb, a);
    if (json)
        sb_puts(&sb, "{\"ok\":true,\"stores\":[");
    uint32_t total_mismatched = 0, total_missing = 0;
    for (size_t t = 0; t < tiers.n; t++) {
        Store s;
        const char *code;
        if (!store_open(a, &s, tiers.dir[t], tiers.tier[t], true, err,
                        sizeof err, &code)) {
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
        total_mismatched += stats.mismatched;
        total_missing += missing;

        if (json) {
            if (t)
                sb_putc(&sb, ',');
            sb_printf(&sb, "{\"store\":\"%s\",\"path\":", tier_name(s.tier));
            json_escape_c(&sb, s.dir);
            sb_printf(&sb,
                      ",\"documents\":%lu,\"chunks\":%lu,\"terms\":%lu,"
                      "\"indexBytes\":%llu,\"missingBlobs\":%lu,"
                      "\"mismatched\":%lu}",
                      (unsigned long)ndocs, (unsigned long)stats.chunks,
                      (unsigned long)stats.terms, (unsigned long long)bytes,
                      (unsigned long)missing,
                      (unsigned long)stats.mismatched);
        } else {
            sb_printf(&sb,
                      "%-8s %lu documents, %lu chunks, %lu terms, %llu bytes\n",
                      tier_name(s.tier), (unsigned long)ndocs,
                      (unsigned long)stats.chunks, (unsigned long)stats.terms,
                      (unsigned long long)bytes);
            if (missing)
                sb_printf(&sb, "         %lu document%s blob is missing\n",
                          (unsigned long)missing, missing == 1 ? "'s" : "s'");
            if (stats.mismatched)
                sb_printf(&sb,
                          "         %lu document%s re-chunked to a different "
                          "count; a reindex is owed\n",
                          (unsigned long)stats.mismatched,
                          stats.mismatched == 1 ? "" : "s");
        }
        store_close(&s);
    }
    if (json) {
        sb_printf(&sb, "],\"missingBlobs\":%lu,\"mismatched\":%lu}",
                  (unsigned long)total_missing,
                  (unsigned long)total_mismatched);
        puts(sb_finish(&sb));
    } else {
        fputs(sb_finish(&sb), stdout);
    }
    return KB_EXIT_OK;
}
