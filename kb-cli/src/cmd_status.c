#include "cmd.h"

/* GET /status (§7) for both tiers: paths, counts, disk use, and which tier
 * is which.
 *
 * §7 also asks for index freshness, model identity and whether the tiers
 * agree on a model. None of that exists yet — there is no model and no
 * index — so this reports the chunking parameters each tier's index was
 * built with instead, and whether they still match what this build would
 * produce. That is the same question one layer down: a store whose
 * chunkTokens or chunker differ from the running binary owes a reindex
 * exactly as a store built with another model would (§8).
 */

static const char *const VALUE_FLAGS[] = {"--store", NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

/* dir is NULL when the tier has no path at all — no store above the current
 * directory, or neither KB_STORE nor HOME set. That is a different thing
 * from a known path with nothing at it yet, which still tells the caller
 * where `kb init` would put one. */
static void tier_json(StrBuf *sb, Arena *a, const char *dir, Tier tier,
                      bool present) {
    sb_printf(sb, "{\"store\":\"%s\",\"path\":", tier_name(tier));
    if (dir)
        json_escape_c(sb, dir);
    else
        sb_puts(sb, "null");
    if (!present) {
        sb_puts(sb, ",\"present\":false}");
        return;
    }
    Store s;
    char err[512];
    const char *code;
    if (!store_open(a, &s, dir, tier, false, err, sizeof err, &code)) {
        sb_puts(sb, ",\"present\":true,\"readable\":false,\"error\":");
        json_escape_c(sb, err);
        sb_putc(sb, '}');
        return;
    }
    uint64_t index_bytes = 0;
    uint64_t total = store_disk_bytes(a, &s, &index_bytes);
    uint64_t chunks = 0, content = 0;
    for (size_t i = 0; i < s.documents.n; i++) {
        chunks += s.documents.v[i].chunk_count;
        content += s.documents.v[i].bytes;
    }
    ChunkParams cp = store_chunk_params(a, &s);
    bool matches = cp.chunk_tokens == KB_CHUNK_TOKENS &&
                   cp.chunk_overlap == KB_CHUNK_OVERLAP &&
                   strcmp(cp.chunker, KB_CHUNKER_ID) == 0;
    sb_printf(sb,
              ",\"present\":true,\"readable\":true,\"sources\":%zu,"
              "\"documents\":%zu,\"chunks\":%llu,\"contentBytes\":%llu,"
              "\"diskBytes\":%llu,\"indexBytes\":%llu",
              s.sources.n, s.documents.n, (unsigned long long)chunks,
              (unsigned long long)content, (unsigned long long)total,
              (unsigned long long)index_bytes);
    sb_printf(sb,
              ",\"nextIds\":{\"source\":\"S-%lld\",\"document\":\"D-%lld\","
              "\"chunk\":\"C-%lld\"}",
              (long long)s.next_source, (long long)s.next_document,
              (long long)s.next_chunk);
    sb_printf(sb,
              ",\"chunking\":{\"recorded\":%s,\"chunker\":\"%s\","
              "\"chunkTokens\":%lu,\"chunkOverlap\":%lu,\"current\":%s}",
              cp.present ? "true" : "false", cp.chunker,
              (unsigned long)cp.chunk_tokens, (unsigned long)cp.chunk_overlap,
              matches ? "true" : "false");
    /* §7 asks for index freshness. The keyword index either describes the
     * logs as they now are or it does not, and that is the same question
     * `kb search` asks before it will answer anything. */
    FtsIndex ix;
    const char *icode;
    char ierr[512];
    bool ok = fts_open_store(a, &s, &ix, &icode, ierr, sizeof ierr);
    sb_printf(sb,
              ",\"index\":{\"keyword\":{\"current\":%s,\"chunks\":%lu,"
              "\"terms\":%lu,\"bytes\":%llu",
              ok ? "true" : "false", ok ? (unsigned long)ix.chunk_count : 0UL,
              ok ? (unsigned long)ix.term_count : 0UL,
              (unsigned long long)ix.file_bytes);
    if (!ok) {
        sb_puts(sb, ",\"error\":\"index_stale\",\"message\":");
        json_escape_c(sb, ierr);
    }
    sb_puts(sb, "}}");
    /* The model is the one thing §7 reports that this slice cannot: say so
     * rather than omit the field, so a caller can tell "no model yet" from
     * "this build does not know about models". */
    sb_puts(sb, ",\"model\":null");
    sb_puts(sb, ",\"torn\":");
    sb_puts(sb, (s.sources.torn_tail || s.documents.torn_tail) ? "true"
                                                              : "false");
    sb_putc(sb, '}');
    store_close(&s);
}

static void tier_human(Arena *a, const char *dir, Tier tier, bool present,
                       bool is_default_write) {
    printf("%-8s %s%s\n", tier_name(tier),
           dir ? dir : (tier == TIER_PROJECT
                            ? "(none above the current directory)"
                            : "(no KB_STORE or HOME)"),
           is_default_write ? "   (default for writes)" : "");
    if (!present) {
        puts("         not initialized");
        return;
    }
    Store s;
    char err[512];
    const char *code;
    if (!store_open(a, &s, dir, tier, false, err, sizeof err, &code)) {
        printf("         unreadable: %s\n", err);
        return;
    }
    uint64_t index_bytes = 0;
    uint64_t total = store_disk_bytes(a, &s, &index_bytes);
    uint64_t chunks = 0;
    for (size_t i = 0; i < s.documents.n; i++)
        chunks += s.documents.v[i].chunk_count;
    ChunkParams cp = store_chunk_params(a, &s);
    printf("         %zu sources, %zu documents, %llu chunks\n", s.sources.n,
           s.documents.n, (unsigned long long)chunks);
    printf("         %llu bytes on disk (%llu in index/)\n",
           (unsigned long long)total, (unsigned long long)index_bytes);
    printf("         next ids S-%lld D-%lld C-%lld\n",
           (long long)s.next_source, (long long)s.next_document,
           (long long)s.next_chunk);
    printf("         chunking %s tokens=%lu overlap=%lu%s\n", cp.chunker,
           (unsigned long)cp.chunk_tokens, (unsigned long)cp.chunk_overlap,
           (cp.chunk_tokens == KB_CHUNK_TOKENS &&
            cp.chunk_overlap == KB_CHUNK_OVERLAP &&
            strcmp(cp.chunker, KB_CHUNKER_ID) == 0)
               ? ""
               : "  (differs from this build: reindex owed)");
    FtsIndex ix;
    const char *icode;
    char ierr[512];
    if (fts_open_store(a, &s, &ix, &icode, ierr, sizeof ierr))
        printf("         keyword index %lu chunks, %lu terms, %llu bytes\n",
               (unsigned long)ix.chunk_count, (unsigned long)ix.term_count,
               (unsigned long long)ix.file_bytes);
    else
        printf("         keyword index stale: %s\n", ierr);
    puts("         model   none (not built yet)");
    if (s.sources.torn_tail || s.documents.torn_tail)
        puts("         an interrupted append left a partial final line; the "
             "next write repairs it");
    store_close(&s);
}

int32_t cmd_status(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }

    char project[KB_PATH_MAX], global[KB_PATH_MAX];
    /* The project store is only ever a path once it exists — it is found by
     * walking up, not by construction. The global one has a path whether or
     * not anything is there yet. */
    const char *project_dir =
        store_find_project(project, sizeof project) ? project : NULL;
    const char *global_dir =
        store_global_dir(global, sizeof global) ? global : NULL;
    bool has_project = project_dir != NULL;
    bool has_global = global_dir && plat_is_dir(global_dir);

    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_puts(&sb, "{\"ok\":true,\"tiers\":[");
        tier_json(&sb, a, project_dir, TIER_PROJECT, has_project);
        sb_putc(&sb, ',');
        tier_json(&sb, a, global_dir, TIER_GLOBAL, has_global);
        sb_printf(&sb, "],\"defaultWrite\":\"%s\"}",
                  has_project ? "project" : (has_global ? "global" : "none"));
        puts(sb_finish(&sb));
    } else {
        tier_human(a, project_dir, TIER_PROJECT, has_project, has_project);
        tier_human(a, global_dir, TIER_GLOBAL, has_global,
                   !has_project && has_global);
    }
    return KB_EXIT_OK;
}
