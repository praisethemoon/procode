#include "cmd.h"

#include "sha256.h"

/* GET /stale and POST /refresh (§5).
 *
 * WHY THIS SECTION EXISTS. Documentation moves — Win32 pages get rewritten,
 * io_uring's surface changes release to release — and a passage that cannot
 * say how old it is will eventually be believed when it should not be. So
 * every row here carries `fetchedAt` and the verdict on it, and the verdict
 * comes from `doc_stale` in cmd_common.c, which is also what `search`, `ls`
 * and `get` call. Two definitions that could disagree is the bug this section
 * exists to prevent.
 *
 * NEWEST SOURCES FIRST (§5). The rows are documents, and they are ordered by
 * the SOURCE they came from: a source's date is the newest of its documents,
 * sources sort newest first, and a source's own documents sort newest first
 * within it. The reason is the use: this list is read to decide what to go
 * and re-read, and the source touched most recently is the one still in play.
 * A source with no readable date sorts last, because "newest first" cannot
 * claim a date it does not have.
 *
 * REFRESH REPORTS AND DOES NOT ACT. §12.2 asked whether the CLI should carry
 * an HTTP client; it does not, and content arrives through `POST /documents`
 * (§2) instead — so there is nothing here that could fetch. `POST /refresh`
 * therefore refetches nothing, re-embeds nothing, and says so in the payload
 * rather than returning a shape a caller would read as work done. It takes no
 * lock either, because a report is a read, and a reader that took the write
 * lock would block an ingest for nothing.
 */

/* Two tables, not one. `--limit` narrows a list of documents and means
 * nothing to a report about sources, and a flag a command accepts and then
 * ignores is worse than one it refuses: the caller believes it was heard. */
static const char *const STALE_FLAGS[] = {"--older-than", "--olderThan",
                                          "--collection", "--limit", NULL};
static const char *const REFRESH_FLAGS[] = {"--older-than", "--olderThan",
                                            "--collection", NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

typedef struct {
    const Document *d;
    const Source *src;
    /* The source's own date: the newest fetchedAt among its documents, or ""
     * when it has none that can be read. The sort key, kept on the row so the
     * response can show what it was ordered by. */
    const char *source_at;
} Row;

/* Newest source first, then newest document, then a stable total order so the
 * same store always produces the same list. Two sources with the same date
 * are told apart by id BEFORE their documents are compared: refresh groups a
 * source's documents by adjacency, and letting two sources' documents
 * interleave would report one source twice. */
static int row_cmp(const void *x, const void *y) {
    const Row *a = (const Row *)x, *b = (const Row *)y;
    int c = strcmp(b->source_at, a->source_at);
    if (c)
        return c;
    int64_t as = kb_id_num(a->d->source, 'S'), bs = kb_id_num(b->d->source, 'S');
    if (as != bs)
        return as < bs ? -1 : 1;
    const char *af = a->d->fetched_at ? a->d->fetched_at : "";
    const char *bf = b->d->fetched_at ? b->d->fetched_at : "";
    c = strcmp(bf, af);
    if (c)
        return c;
    int64_t an = kb_id_num(a->d->id, 'D'), bn = kb_id_num(b->d->id, 'D');
    return an < bn ? -1 : (an > bn ? 1 : 0);
}

/* Shared by both routes: open the store, keep the stale documents that pass
 * the collection filter, and order them. Returns false having already
 * reported the failure, with the store left closed. */
static bool collect(Arena *a, int32_t argc, char **argv,
                    const char *const *flags, bool json, const Staleness *st,
                    Store *s, Row **out, size_t *nout) {
    const char *collection = flag_value(argc, argv, flags, "--collection");
    char err[512];
    char dir[KB_PATH_MAX];
    if (!store_resolve(dir, sizeof dir, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return false;
    }
    const char *code;
    if (!store_open(a, s, dir, false, err, sizeof err, &code)) {
        err_out(json, code, "%s", err);
        return false;
    }
    Row *rows = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < s->documents.n; i++) {
        const Document *d = &s->documents.v[i];
        const Source *src = src_by_id(&s->sources, d->source);
        if (collection && (!src || strcmp(src->collection, collection) != 0))
            continue;
        if (!doc_stale(st, d))
            continue;
        SourceFacts f = source_facts(a, s, d->source);
        ARENA_GROW(a, rows, n, cap, Row);
        rows[n].d = d;
        rows[n].src = src;
        rows[n].source_at = f.fetched_at ? f.fetched_at : "";
        n++;
    }
    if (n > 1)
        qsort(rows, n, sizeof *rows, row_cmp);
    *out = rows;
    *nout = n;
    return true;
}

int32_t cmd_stale(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, STALE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, STALE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    char err[512];
    Staleness st;
    if (!staleness_init(&st, older_than_arg(argc, argv, STALE_FLAGS), err,
                        sizeof err)) {
        err_out(json, "usage", "%s", err);
        return KB_EXIT_ERR;
    }
    const char *limit_s = flag_value(argc, argv, STALE_FLAGS, "--limit");
    int64_t limit = limit_s ? strtoll(limit_s, NULL, 10) : 0;
    if (limit_s && limit <= 0) {
        err_out(json, "usage", "--limit expects a positive number");
        return KB_EXIT_ERR;
    }

    Store s;
    Row *rows = NULL;
    size_t n = 0;
    if (!collect(a, argc, argv, STALE_FLAGS, json, &st, &s, &rows, &n))
        return KB_EXIT_ERR;
    if (limit && (int64_t)n > limit)
        n = (size_t)limit;

    StrBuf sb;
    sb_init(&sb, a);
    if (json) {
        sb_printf(&sb,
                  "{\"ok\":true,\"olderThan\":\"%s\",\"staleBefore\":\"%s\","
                  "\"documents\":[",
                  st.spec, st.cutoff_iso);
        for (size_t i = 0; i < n; i++) {
            if (i)
                sb_putc(&sb, ',');
            sb_putc(&sb, '{');
            json_document(&sb, rows[i].d, rows[i].src, &st);
            /* The sort key, shown rather than implied: §5 orders by source
             * and a caller reading a flat list of documents cannot otherwise
             * see why they came in this order. */
            sb_puts(&sb, ",\"sourceFetchedAt\":");
            json_escape_c(&sb, rows[i].source_at);
            sb_putc(&sb, '}');
        }
        sb_printf(&sb, "],\"count\":%zu}", n);
        puts(sb_finish(&sb));
    } else if (n == 0) {
        printf("nothing older than %s\n", st.spec);
    } else {
        for (size_t i = 0; i < n; i++) {
            sb_printf(&sb, "%-8s %-14s %-21s %-8s ", rows[i].d->id,
                      rows[i].src ? rows[i].src->collection : "-",
                      rows[i].d->fetched_at ? rows[i].d->fetched_at : "-",
                      rows[i].d->source);
            sb_puts_safe(&sb, rows[i].d->title ? rows[i].d->title : "");
            sb_putc(&sb, '\n');
        }
        fputs(sb_finish(&sb), stdout);
    }
    store_close(&s);
    return KB_EXIT_OK;
}

/* ---- refresh ----------------------------------------------------------- */

/* Whether a locator could be read again at all, which is the one thing a
 * report about refetching can usefully say. A `url` needs the HTTP client
 * this binary does not have; a `file` or `dir` is still on disk and can be
 * re-ingested with `kb add --file`; an `inline` source is content somebody
 * handed over and there is nowhere to go back to. */
static const char *refetch_route(const char *kind) {
    if (strcmp(kind, "url") == 0)
        return "post-documents";
    if (strcmp(kind, "file") == 0 || strcmp(kind, "dir") == 0)
        return "re-read";
    return "none";
}

/* §2's POST /sources/{id}/refresh: read one source again, compare by hash,
 * and re-index only if the text changed — through refile_document, the same
 * path `kb add` takes for a document it has seen before. Only a `file`
 * source can be read again here: a `url` needs the HTTP client this binary
 * does not have (§12.2), and an `inline` source was content handed over,
 * with nowhere to go back to. Both are refused and say how to re-file. */
static int32_t refresh_source(Arena *a, bool json, const char *id) {
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
    const Source *src = src_by_id(&s.sources, id);
    const Document *doc = src ? doc_by_source_path(&s.documents, src->id, "") : NULL;
    if (!src || !doc) {
        store_close(&s);
        err_out(json, "not_found", src ? "source %s has no document to refresh"
                                       : "no source %s",
                id);
        return KB_EXIT_ERR;
    }
    if (strcmp(src->kind, "url") == 0) {
        store_close(&s);
        errdet_begin("fetch_failed");
        errdet_str("locator", src->locator);
        err_out(json, "fetch_failed",
                "%s is %s, and kb has no HTTP client to fetch it: re-file the "
                "page with kb add --url",
                id, src->locator);
        return KB_EXIT_ERR;
    }
    if (strcmp(src->kind, "file") != 0) {
        store_close(&s);
        err_out(json, "usage",
                "%s was filed from content handed over directly, so there is "
                "nothing to read again: re-file it with kb add",
                id);
        return KB_EXIT_ERR;
    }
    char *text;
    size_t len;
    if (!read_text_arg(a, src->locator, &text, &len)) {
        /* Recorded, so `kb sources` shows which sources could not be read
         * again. If even that append fails, the refusal below still stands. */
        char ignored[256];
        (void)source_set_status(a, &s, src, "fetch_failed", ignored,
                                sizeof ignored);
        store_close(&s);
        errdet_begin("fetch_failed");
        errdet_str("locator", src->locator);
        err_out(json, "fetch_failed", "cannot read %s for %s", src->locator, id);
        return KB_EXIT_ERR;
    }
    char hash[65];
    sha256_hex(text, len, hash);
    char now[32];
    plat_timestamp(now);
    Document d;
    bool changed = false, blob_written = false;
    if (!refile_document(a, &s, doc, text, len, hash, doc->title, doc->mime,
                         doc->meta, NULL, now, &d, &changed, &blob_written,
                         err, sizeof err) ||
        !source_set_status(a, &s, src, "ok", err, sizeof err)) {
        store_close(&s);
        err_out(json, "internal", "%s", err);
        return KB_EXIT_FATAL;
    }
    if (changed) {
        uint32_t nd = 0, missing = 0;
        FtsBuildStats stats;
        if (!doclog_load(a, s.documents_path, &s.documents, err, sizeof err) ||
            !index_rebuild(a, &s, &nd, &missing, &stats, err, sizeof err)) {
            store_close(&s);
            err_out(json, "internal", "%s", err);
            return KB_EXIT_FATAL;
        }
    }
    if (json) {
        printf("{\"ok\":true,\"action\":\"refresh\",\"source\":\"%s\","
               "\"document\":\"%s\",\"changed\":%s,\"contentHash\":\"%s\","
               "\"fetchedAt\":\"%s\"}\n",
               src->id, d.id, changed ? "true" : "false", d.content_hash, now);
    } else {
        printf("%s %s: %s\n", src->id, d.id,
               changed ? "changed, re-indexed" : "unchanged, fetchedAt updated");
    }
    store_close(&s);
    return KB_EXIT_OK;
}

int32_t cmd_refresh(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, REFRESH_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, REFRESH_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    /* A source id names one source to read again; without one this is §5's
     * report over a scope. The two take different arguments, and mixing them
     * is refused rather than half-honoured. */
    const char *one = positional_arg(argc, argv, REFRESH_FLAGS, 0);
    if (one) {
        if (kb_id_num(one, 'S') == 0 || positional_arg(argc, argv, REFRESH_FLAGS, 1) ||
            flag_value(argc, argv, REFRESH_FLAGS, "--collection") ||
            older_than_arg(argc, argv, REFRESH_FLAGS)) {
            err_out(json, "usage", "kb refresh takes one source id (S-n) on its "
                                   "own, or --collection and --older-than");
            return KB_EXIT_ERR;
        }
        return refresh_source(a, json, one);
    }
    char err[512];
    Staleness st;
    if (!staleness_init(&st, older_than_arg(argc, argv, REFRESH_FLAGS), err,
                        sizeof err)) {
        err_out(json, "usage", "%s", err);
        return KB_EXIT_ERR;
    }

    Store s;
    Row *rows = NULL;
    size_t n = 0;
    if (!collect(a, argc, argv, REFRESH_FLAGS, json, &st, &s, &rows, &n))
        return KB_EXIT_ERR;

    /* Grouped by source, because a refetch is a thing you do to a source.
     * The rows are already ordered newest source first, so a source's
     * documents are adjacent and the first of them opens the group. */
    StrBuf sb;
    sb_init(&sb, a);
    size_t nsources = 0;
    if (json)
        sb_puts(&sb, "{\"ok\":true,\"action\":\"report\",\"refetched\":0,"
                     "\"reembedded\":0,\"sources\":[");
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && strcmp(rows[j].d->source, rows[i].d->source) == 0)
            j++;
        const Source *src = rows[i].src;
        const char *kind = src ? src->kind : "";
        if (json) {
            if (nsources)
                sb_putc(&sb, ',');
            sb_printf(&sb, "{\"id\":\"%s\",\"kind\":\"%s\"", rows[i].d->source,
                      kind);
            sb_puts(&sb, ",\"locator\":");
            json_escape_c(&sb, src ? src->locator : "");
            sb_puts(&sb, ",\"collection\":");
            json_escape_c(&sb, src ? src->collection : "");
            sb_printf(&sb, ",\"staleDocuments\":%zu,\"fetchedAt\":\"%s\","
                           "\"refetchBy\":\"%s\"}",
                      j - i, rows[i].source_at, refetch_route(kind));
        } else {
            sb_printf(&sb, "%-8s %-7s %-21s %zu stale  ", rows[i].d->source,
                      kind,
                      rows[i].source_at[0] ? rows[i].source_at : "-", j - i);
            sb_puts_safe(&sb, src ? src->locator : "");
            sb_putc(&sb, '\n');
        }
        nsources++;
        i = j;
    }
    if (json) {
        /* Said in three ways on purpose — the verb, the two zero counts, and
         * a sentence — because a caller that reads this as an action would go
         * on believing the corpus had just been brought up to date. */
        sb_printf(&sb,
                  "],\"count\":%zu,\"staleDocuments\":%zu,\"olderThan\":\"%s\","
                  "\"staleBefore\":\"%s\",\"note\":",
                  nsources, n, st.spec, st.cutoff_iso);
        json_escape_c(&sb,
                      "this is a report, not an action: kb has no HTTP client "
                      "and fetched nothing. Re-file the content through "
                      "\"kb add\" to bring a source up to date.");
        sb_putc(&sb, '}');
        puts(sb_finish(&sb));
    } else {
        if (nsources == 0)
            printf("nothing older than %s\n", st.spec);
        else
            fputs(sb_finish(&sb), stdout);
        printf("reported %zu source%s, %zu stale document%s; fetched nothing "
               "(kb has no HTTP client). Re-file with \"kb add\".\n",
               nsources, nsources == 1 ? "" : "s", n, n == 1 ? "" : "s");
    }
    store_close(&s);
    return KB_EXIT_OK;
}
