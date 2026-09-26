#include "cmd.h"

/* GET /status (§7): the store's path, counts and disk use.
 *
 * §7 also asks for index freshness and model identity. There is no model
 * yet, so this reports the chunking parameters the index was
 * built with instead, and whether they still match what this build would
 * produce. That is the same question one layer down: a store whose
 * chunkTokens or chunker differ from the running binary owes a reindex
 * exactly as a store built with another model would (§8).
 */

static const char *const VALUE_FLAGS[] = {"--older-than", "--olderThan",
                                          NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

/* Emits the fields without enclosing braces. dir is NULL when there is no
 * store at or above the current directory: status still succeeds then,
 * because "there is nothing here yet" is the answer, not a failure to get
 * one. */
static void store_json(StrBuf *sb, Arena *a, const char *dir,
                       const Staleness *st) {
    sb_puts(sb, "\"path\":");
    if (!dir) {
        sb_puts(sb, "null,\"present\":false");
        return;
    }
    json_escape_c(sb, dir);
    Store s;
    char err[512];
    const char *code;
    if (!store_open(a, &s, dir, false, err, sizeof err, &code)) {
        sb_puts(sb, ",\"present\":true,\"readable\":false,\"error\":");
        json_escape_c(sb, err);
        return;
    }
    uint64_t index_bytes = 0;
    uint64_t total = store_disk_bytes(a, &s, &index_bytes);
    uint64_t chunks = 0, content = 0, stale = 0;
    for (size_t i = 0; i < s.documents.n; i++) {
        chunks += s.documents.v[i].chunk_count;
        content += s.documents.v[i].bytes;
        if (doc_stale(st, &s.documents.v[i]))
            stale++;
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
    /* §5 and §6, the two things this slice adds that a reader would otherwise
     * have to go and count. The threshold is stated beside the count, because
     * a number of stale documents means nothing without the question it
     * answers. */
    sb_printf(sb,
              ",\"links\":%zu,\"stale\":{\"documents\":%llu,"
              "\"olderThan\":\"%s\",\"before\":\"%s\"}",
              s.documents.nlinks, (unsigned long long)stale, st->spec,
              st->cutoff_iso);
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
        const char *details = errdet_json("index_stale");
        if (details) {
            sb_puts(sb, ",\"details\":");
            sb_puts(sb, details);
        }
    }
    sb_puts(sb, "}}");
    /* The model is the one thing §7 reports that this slice cannot: say so
     * rather than omit the field, so a caller can tell "no model yet" from
     * "this build does not know about models". */
    sb_puts(sb, ",\"model\":null");
    sb_puts(sb, ",\"torn\":");
    sb_puts(sb, (s.sources.torn_tail || s.documents.torn_tail) ? "true"
                                                              : "false");
    store_close(&s);
}

static void store_human(Arena *a, const char *dir, const Staleness *st) {
    if (!dir) {
        puts("no kb store at or above the current directory (run \"kb "
             "init\")");
        return;
    }
    printf("store    %s\n", dir);
    Store s;
    char err[512];
    const char *code;
    if (!store_open(a, &s, dir, false, err, sizeof err, &code)) {
        printf("         unreadable: %s\n", err);
        return;
    }
    uint64_t index_bytes = 0;
    uint64_t total = store_disk_bytes(a, &s, &index_bytes);
    uint64_t chunks = 0, stale = 0;
    for (size_t i = 0; i < s.documents.n; i++) {
        chunks += s.documents.v[i].chunk_count;
        if (doc_stale(st, &s.documents.v[i]))
            stale++;
    }
    ChunkParams cp = store_chunk_params(a, &s);
    printf("         %zu sources, %zu documents, %llu chunks\n", s.sources.n,
           s.documents.n, (unsigned long long)chunks);
    printf("         %zu links, %llu documents older than %s\n",
           s.documents.nlinks, (unsigned long long)stale, st->spec);
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

    char serr[512];
    Staleness st;
    if (!staleness_init(&st, older_than_arg(argc, argv, VALUE_FLAGS), serr,
                        sizeof serr)) {
        err_out(json, "usage", "%s", serr);
        return KB_EXIT_ERR;
    }

    char found[KB_PATH_MAX];
    const char *dir = store_find(found, sizeof found) ? found : NULL;
    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_puts(&sb, "{\"ok\":true,");
        store_json(&sb, a, dir, &st);
        sb_printf(&sb, ",\"olderThan\":\"%s\"}", st.spec);
        puts(sb_finish(&sb));
    } else {
        store_human(a, dir, &st);
    }
    return KB_EXIT_OK;
}
