#include "cmd.h"

/* §7 — maintenance: GET /stats, POST /reindex, POST /compact.
 *
 * THE HOLE BETWEEN §1.6 AND §8, AND WHERE IT IS RESOLVED. §1.6 calls
 * everything under index/ derived and says `kb rebuild` reconstructs all of
 * it from the logs and the blobs. §8 defines index/model.json as the record
 * of the configuration the index was BUILT with — which is precisely what the
 * logs do not contain and cannot, because it is a fact about a past run
 * rather than about the corpus. The two cannot both be honoured by one
 * command, so they are split between two:
 *
 *   rebuild  writes model.json only when it is absent, and otherwise OBEYS
 *            it. Reproducing the chunk ids the log reserved requires chunking
 *            the way the store was chunked, not the way this build would.
 *   reindex  REWRITES it. This is the one command whose whole job is to
 *            re-derive every chunk under new parameters, so it is the one
 *            command entitled to change the record of what they are.
 *
 * That leaves model.json derived in the sense §1.6 needs — a clone with no
 * index/ at all rebuilds and gets one — while keeping it evidence in the
 * sense §8 needs, because nothing but an explicit reindex can overwrite it.
 */

static const char *const VALUE_FLAGS[] = {NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

/* ---- GET /stats -------------------------------------------------------- */

typedef struct {
    const char *name;
    int64_t documents;
    int64_t chunks;
    int64_t bytes;
} Stat;

static int stat_cmp(const void *x, const void *y) {
    const Stat *a = (const Stat *)x, *b = (const Stat *)y;
    return strcmp(a->name, b->name);
}

int32_t cmd_stats(Arena *a, int32_t argc, char **argv) {
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

    Stat *rows = NULL;
    size_t n = 0, cap = 0;
    int64_t td = 0, tc = 0, tb = 0;
    Store s;
    const char *code;
    if (!store_open(a, &s, dir, false, err, sizeof err, &code)) {
        err_out(json, code, "%s", err);
        return KB_EXIT_ERR;
    }
    for (size_t i = 0; i < s.documents.n; i++) {
        const Document *d = &s.documents.v[i];
        const Source *src = src_by_id(&s.sources, d->source);
        if (!src || !src->collection)
            continue;
        Stat *r = NULL;
        for (size_t k = 0; k < n && !r; k++) {
            if (strcmp(rows[k].name, src->collection) == 0)
                r = &rows[k];
        }
        if (!r) {
            ARENA_GROW(a, rows, n, cap, Stat);
            r = &rows[n++];
            r->name = src->collection;
            r->documents = r->chunks = r->bytes = 0;
        }
        r->documents++;
        r->chunks += d->chunk_count;
        r->bytes += (int64_t)d->bytes;
        td++;
        tc += d->chunk_count;
        tb += (int64_t)d->bytes;
    }
    store_close(&s);
    if (n > 1)
        qsort(rows, n, sizeof *rows, stat_cmp);

    StrBuf sb;
    sb_init(&sb, a);
    if (json) {
        sb_puts(&sb, "{\"ok\":true,\"collections\":[");
        for (size_t i = 0; i < n; i++) {
            if (i)
                sb_putc(&sb, ',');
            sb_puts(&sb, "{\"name\":");
            json_escape_c(&sb, rows[i].name);
            sb_printf(&sb,
                      ",\"documents\":%lld,\"chunks\":%lld,\"bytes\":%lld}",
                      (long long)rows[i].documents,
                      (long long)rows[i].chunks, (long long)rows[i].bytes);
        }
        sb_printf(&sb,
                  "],\"count\":%zu,\"totals\":{\"documents\":%lld,"
                  "\"chunks\":%lld,\"bytes\":%lld}}",
                  n, (long long)td, (long long)tc, (long long)tb);
        puts(sb_finish(&sb));
    } else if (n == 0) {
        puts("no collections");
    } else {
        for (size_t i = 0; i < n; i++) {
            sb_printf(&sb, "%6lld doc %8lld chunk %10lld b  ",
                      (long long)rows[i].documents,
                      (long long)rows[i].chunks, (long long)rows[i].bytes);
            sb_puts_safe(&sb, rows[i].name);
            sb_putc(&sb, '\n');
        }
        sb_printf(&sb, "%6lld doc %8lld chunk %10lld b  total\n",
                  (long long)td, (long long)tc, (long long)tb);
        fputs(sb_finish(&sb), stdout);
    }
    return KB_EXIT_OK;
}

/* ---- POST /reindex ----------------------------------------------------- */

/* Two splits of the same text differ when any boundary moves, not merely
 * when the count does. Comparing counts alone would let a document keep a
 * chunk range whose C-ids now name different passages, which is exactly what
 * §1.1 forbids. */
static bool chunks_same(const Chunks *x, const Chunks *y) {
    if (x->n != y->n)
        return false;
    for (size_t i = 0; i < x->n; i++) {
        if (x->v[i].start != y->v[i].start || x->v[i].end != y->v[i].end)
            return false;
    }
    return true;
}

int32_t cmd_reindex(Arena *a, int32_t argc, char **argv) {
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
    ChunkParams old = store_chunk_params(a, &s);
    size_t n = s.documents.n;
    Chunks *fresh = (Chunks *)arena_alloc0(a, (n ? n : 1) * sizeof(Chunks));
    bool *rechunk = (bool *)arena_alloc0(a, (n ? n : 1) * sizeof(bool));
    uint32_t missing = 0, changed = 0;
    uint64_t total_new = 0;
    for (size_t i = 0; i < n; i++) {
        const Document *d = &s.documents.v[i];
        char *text;
        size_t len;
        if (!store_get_blob(&s, d->content_hash, &text, &len)) {
            /* No text, no basis for a new range. Its record is left
             * exactly as it is and the damage is reported. */
            missing++;
            continue;
        }
        Lang lang = doc_lang(d->mime, d->path);
        fresh[i] = chunk_split(a, text, len, lang,
                               (size_t)KB_CHUNK_TOKENS * KB_BYTES_PER_TOKEN,
                               (size_t)KB_CHUNK_OVERLAP *
                                   KB_BYTES_PER_TOKEN);
        Chunks was = chunk_split(a, text, len, lang,
                                 (size_t)old.chunk_tokens *
                                     KB_BYTES_PER_TOKEN,
                                 (size_t)old.chunk_overlap *
                                     KB_BYTES_PER_TOKEN);
        /* Two reasons to re-range, and both are needed. The splits
         * differing means the chunker or its parameters moved under the
         * store. The recorded count differing from what the OLD
         * parameters produce means the log and the blobs were already out
         * of step — a store half-way through an interrupted reindex looks
         * exactly like this, and re-running must finish the job rather
         * than decide there is nothing to do. */
        if (!chunks_same(&fresh[i], &was) ||
            was.n != (size_t)d->chunk_count) {
            rechunk[i] = true;
            changed++;
            total_new += fresh[i].n;
        }
    }

    int64_t base = 0;
    if (total_new) {
        /* One reservation for the whole pass, handed out in contiguous
         * per-document runs. A FRESH range every time: the ids a document
         * held described a particular splitting, and handing them to
         * different passages would reuse an identifier §1.1 says is never
         * reused. Ids are monotonic and never reused, not dense. */
        int64_t unused = 0;
        if (!store_reserve(&s, 0, 0, (uint32_t)total_new, &unused, &unused,
                           &base, err, sizeof err)) {
            store_close(&s);
            err_out(json, "internal", "%s", err);
            return KB_EXIT_FATAL;
        }
    }
    char now[32];
    plat_timestamp(now); /* one instant for the whole pass */
    for (size_t i = 0; i < n; i++) {
        if (!rechunk[i])
            continue;
        Document d = s.documents.v[i];
        d.chunk_base = base;
        d.chunk_count = (uint32_t)fresh[i].n;
        base += (int64_t)fresh[i].n;
        /* indexedAt moves and fetchedAt does not: the text was not
         * fetched again, it was cut up again. §5 reads fetchedAt to
         * decide staleness, and a reindex that reset it would make a
         * three-year-old page look freshly read. */
        d.indexed_at = now;
        size_t len;
        char *line = doc_encode_document(a, &d, &len);
        if (!store_append(&s, STORE_DOCUMENTS, line, len, err,
                          sizeof err)) {
            store_close(&s);
            err_out(json, "internal", "%s", err);
            return KB_EXIT_FATAL;
        }
    }
    /* AFTER the records, deliberately. A crash in between leaves the file
     * saying the old parameters while the log holds the new ranges, and
     * that state is detected — a re-split under the recorded parameters no
     * longer matches the log — so a second reindex converges. The other
     * order leaves a state a second reindex reads as already correct. */
    if (!store_rewrite_chunk_params(&s, err, sizeof err)) {
        store_close(&s);
        err_out(json, "internal", "%s", err);
        return KB_EXIT_FATAL;
    }
    uint32_t ndocs = 0, mb = 0;
    FtsBuildStats stats;
    if (!doclog_load(a, s.documents_path, &s.documents, err, sizeof err) ||
        !index_rebuild(a, &s, &ndocs, &mb, &stats, err, sizeof err)) {
        store_close(&s);
        err_out(json, "internal", "%s", err);
        return KB_EXIT_FATAL;
    }

    StrBuf sb;
    sb_init(&sb, a);
    if (json) {
        sb_puts(&sb, "{\"ok\":true,\"path\":");
        json_escape_c(&sb, s.dir);
        sb_printf(&sb,
                  ",\"documents\":%lu,\"rechunked\":%lu,"
                  "\"missingBlobs\":%lu,\"chunks\":%lu,\"terms\":%lu,"
                  "\"chunker\":\"%s\",\"chunkTokens\":%lu,"
                  "\"chunkOverlap\":%lu,\"wasChunker\":\"%s\","
                  "\"wasChunkTokens\":%lu,\"wasChunkOverlap\":%lu,"
                  "\"reembedded\":0",
                  (unsigned long)ndocs, (unsigned long)changed,
                  (unsigned long)missing, (unsigned long)stats.chunks,
                  (unsigned long)stats.terms, KB_CHUNKER_ID,
                  (unsigned long)KB_CHUNK_TOKENS,
                  (unsigned long)KB_CHUNK_OVERLAP, old.chunker,
                  (unsigned long)old.chunk_tokens,
                  (unsigned long)old.chunk_overlap);
        /* There are no embeddings in this build (§8), so a reindex re-chunks
         * and re-indexes keywords and nothing else. Said in the payload
         * rather than implied by an absent field. */
        sb_puts(&sb, ",\"note\":");
        json_escape_c(&sb, "rechunked and re-indexed for keyword retrieval; "
                           "this build has no embedding model, so nothing was "
                           "re-embedded.");
        sb_putc(&sb, '}');
        puts(sb_finish(&sb));
    } else {
        sb_printf(&sb, "%lu documents, %lu rechunked, %lu chunks, %lu terms\n",
                  (unsigned long)ndocs, (unsigned long)changed,
                  (unsigned long)stats.chunks, (unsigned long)stats.terms);
        sb_printf(&sb, "chunking %s tokens=%lu overlap=%lu",
                  KB_CHUNKER_ID, (unsigned long)KB_CHUNK_TOKENS,
                  (unsigned long)KB_CHUNK_OVERLAP);
        if (strcmp(old.chunker, KB_CHUNKER_ID) != 0 ||
            old.chunk_tokens != KB_CHUNK_TOKENS ||
            old.chunk_overlap != KB_CHUNK_OVERLAP)
            sb_printf(&sb, "  (was %s tokens=%lu overlap=%lu)",
                      old.chunker, (unsigned long)old.chunk_tokens,
                      (unsigned long)old.chunk_overlap);
        sb_putc(&sb, '\n');
        if (missing)
            sb_printf(&sb,
                      "%lu document%s blob is missing and kept its chunk "
                      "range\n",
                      (unsigned long)missing, missing == 1 ? "'s" : "s'");
        fputs(sb_finish(&sb), stdout);
    }
    store_close(&s);
    return KB_EXIT_OK;
}

/* ---- POST /compact ----------------------------------------------------- */

typedef struct {
    Arena *a;
    char **names;
    size_t n, cap;
} BlobNames;

static WalkAction blob_visit(const char *rel, bool is_dir, void *ud) {
    BlobNames *b = (BlobNames *)ud;
    if (is_dir)
        return WALK_CONT;
    ARENA_GROW(b->a, b->names, b->n, b->cap, char *);
    b->names[b->n++] = arena_strdup(b->a, rel);
    return WALK_CONT;
}

int32_t cmd_compact(Arena *a, int32_t argc, char **argv) {
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
    /* THE REFERENCED SET IS BUILT FIRST, FROM THE FOLD, UNDER THE LOCK.
     * The fold is last-wins by id, so it is exactly the set of LIVE
     * documents, and a blob it names is a blob some document still reads
     * through. Nothing is unlinked until every one of them is known, and
     * nothing whose name is in the set is ever a candidate — so "compact
     * never removes a referenced blob" is a property of the order these
     * two loops run in, not of getting a condition right. */
    StrSet live;
    strset_init(&live, a);
    for (size_t i = 0; i < s.documents.n; i++)
        strset_add(&live, s.documents.v[i].content_hash);

    BlobNames b;
    memset(&b, 0, sizeof b);
    b.a = a;
    plat_walk(a, s.blobs_dir, blob_visit, &b);

    uint32_t dropped = 0, kept = 0, skipped = 0;
    uint64_t freed = 0;
    for (size_t i = 0; i < b.n; i++) {
        const char *name = b.names[i];
        if (strset_has(&live, name)) {
            kept++;
            continue;
        }
        /* Anything that is not a blob name is left alone: a temp file a
         * crashed atomic write left behind, or something a person put
         * here. compact drops superseded BLOBS (§7), and a file this
         * directory cannot explain is not one. */
        if (!store_is_blob_name(name)) {
            skipped++;
            continue;
        }
        char path[KB_PATH_MAX];
        store_blob_path(&s, name, path, sizeof path);
        uint64_t sz = 0;
        plat_file_size(path, &sz);
        if (!store_drop_blob(&s, name)) {
            skipped++;
            continue;
        }
        dropped++;
        freed += sz;
    }

    StrBuf sb;
    sb_init(&sb, a);
    if (json) {
        sb_puts(&sb, "{\"ok\":true,\"path\":");
        json_escape_c(&sb, s.dir);
        sb_printf(&sb,
                  ",\"dropped\":%lu,\"kept\":%lu,\"skipped\":%lu,"
                  "\"bytesFreed\":%llu}",
                  (unsigned long)dropped, (unsigned long)kept,
                  (unsigned long)skipped, (unsigned long long)freed);
        puts(sb_finish(&sb));
    } else {
        sb_printf(&sb,
                  "dropped %lu blob%s (%llu bytes), kept %lu, left %lu "
                  "alone\n",
                  (unsigned long)dropped, dropped == 1 ? "" : "s",
                  (unsigned long long)freed, (unsigned long)kept,
                  (unsigned long)skipped);
        fputs(sb_finish(&sb), stdout);
    }
    store_close(&s);
    return KB_EXIT_OK;
}
