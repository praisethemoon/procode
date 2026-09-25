#include "cmd.h"

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

static const char *const VALUE_FLAGS[] = {
    "--older-than", "--olderThan", "--collection",
    "--store",      "--limit",     NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

typedef struct {
    Store *s;
    const Document *d;
    const Source *src;
    /* The source's own date: the newest fetchedAt among its documents, or ""
     * when it has none that can be read. The sort key, kept on the row so the
     * response can show what it was ordered by. */
    const char *source_at;
} Row;

/* Newest source first, then newest document, then a stable total order so the
 * same store always produces the same list. */
static int row_cmp(const void *x, const void *y) {
    const Row *a = (const Row *)x, *b = (const Row *)y;
    int c = strcmp(b->source_at, a->source_at);
    if (c)
        return c;
    const char *af = a->d->fetched_at ? a->d->fetched_at : "";
    const char *bf = b->d->fetched_at ? b->d->fetched_at : "";
    c = strcmp(bf, af);
    if (c)
        return c;
    if (a->s->tier != b->s->tier)
        return a->s->tier < b->s->tier ? -1 : 1;
    int64_t an = kb_id_num(a->d->id, 'D'), bn = kb_id_num(b->d->id, 'D');
    return an < bn ? -1 : (an > bn ? 1 : 0);
}

/* Shared by both routes: open every tier in scope, keep the stale documents
 * that pass the collection filter, and order them. Returns false having
 * already reported the failure. */
static bool collect(Arena *a, int32_t argc, char **argv, bool json,
                    const Staleness *st, Store *stores, size_t *nstores,
                    Row **out, size_t *nout, int32_t *rc) {
    const char *collection = flag_value(argc, argv, VALUE_FLAGS,
                                        "--collection");
    StoreSel sel;
    char err[512];
    if (!store_sel_parse(flag_value(argc, argv, VALUE_FLAGS, "--store"),
                         &sel)) {
        err_out(json, "usage", "--store expects project, global or all");
        *rc = KB_EXIT_ERR;
        return false;
    }
    TierSet tiers;
    if (!tiers_resolve(sel, false, &tiers, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        *rc = KB_EXIT_ERR;
        return false;
    }
    Row *rows = NULL;
    size_t n = 0, cap = 0;
    *nstores = 0;
    for (size_t t = 0; t < tiers.n; t++) {
        const char *code;
        Store *s = &stores[*nstores];
        if (!store_open(a, s, tiers.dir[t], tiers.tier[t], false, err,
                        sizeof err, &code)) {
            err_out(json, code, "%s", err);
            *rc = KB_EXIT_ERR;
            return false;
        }
        (*nstores)++;
        for (size_t i = 0; i < s->documents.n; i++) {
            const Document *d = &s->documents.v[i];
            const Source *src = src_by_id(&s->sources, d->source);
            if (collection &&
                (!src || strcmp(src->collection, collection) != 0))
                continue;
            if (!doc_stale(st, d))
                continue;
            SourceFacts f = source_facts(a, s, d->source);
            ARENA_GROW(a, rows, n, cap, Row);
            rows[n].s = s;
            rows[n].d = d;
            rows[n].src = src;
            rows[n].source_at = f.fetched_at ? f.fetched_at : "";
            n++;
        }
    }
    if (n > 1)
        qsort(rows, n, sizeof *rows, row_cmp);
    *out = rows;
    *nout = n;
    return true;
}

static void close_all(Store *stores, size_t n) {
    for (size_t i = 0; i < n; i++)
        store_close(&stores[i]);
}

int32_t cmd_stale(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    char err[512];
    Staleness st;
    if (!staleness_init(&st, older_than_arg(argc, argv, VALUE_FLAGS), err,
                        sizeof err)) {
        err_out(json, "usage", "%s", err);
        return KB_EXIT_ERR;
    }
    const char *limit_s = flag_value(argc, argv, VALUE_FLAGS, "--limit");
    int64_t limit = limit_s ? strtoll(limit_s, NULL, 10) : 0;
    if (limit_s && limit <= 0) {
        err_out(json, "usage", "--limit expects a positive number");
        return KB_EXIT_ERR;
    }

    Store stores[2];
    size_t nstores = 0;
    Row *rows = NULL;
    size_t n = 0;
    int32_t rc = KB_EXIT_OK;
    if (!collect(a, argc, argv, json, &st, stores, &nstores, &rows, &n, &rc)) {
        close_all(stores, nstores);
        return rc;
    }
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
            json_document(&sb, rows[i].s, rows[i].d, rows[i].src, &st);
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
            sb_printf(&sb, "%-8s %-8s %-14s %-21s %-8s ", rows[i].d->id,
                      tier_name(rows[i].s->tier),
                      rows[i].src ? rows[i].src->collection : "-",
                      rows[i].d->fetched_at ? rows[i].d->fetched_at : "-",
                      rows[i].d->source);
            sb_puts_safe(&sb, rows[i].d->title ? rows[i].d->title : "");
            sb_putc(&sb, '\n');
        }
        fputs(sb_finish(&sb), stdout);
    }
    close_all(stores, nstores);
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

int32_t cmd_refresh(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    char err[512];
    Staleness st;
    if (!staleness_init(&st, older_than_arg(argc, argv, VALUE_FLAGS), err,
                        sizeof err)) {
        err_out(json, "usage", "%s", err);
        return KB_EXIT_ERR;
    }

    Store stores[2];
    size_t nstores = 0;
    Row *rows = NULL;
    size_t n = 0;
    int32_t rc = KB_EXIT_OK;
    if (!collect(a, argc, argv, json, &st, stores, &nstores, &rows, &n, &rc)) {
        close_all(stores, nstores);
        return rc;
    }

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
        while (j < n && rows[j].s == rows[i].s &&
               strcmp(rows[j].d->source, rows[i].d->source) == 0)
            j++;
        const Source *src = rows[i].src;
        const char *kind = src ? src->kind : "";
        if (json) {
            if (nsources)
                sb_putc(&sb, ',');
            sb_printf(&sb, "{\"id\":\"%s\",\"store\":\"%s\",\"kind\":\"%s\"",
                      rows[i].d->source, tier_name(rows[i].s->tier), kind);
            sb_puts(&sb, ",\"locator\":");
            json_escape_c(&sb, src ? src->locator : "");
            sb_puts(&sb, ",\"collection\":");
            json_escape_c(&sb, src ? src->collection : "");
            sb_printf(&sb, ",\"staleDocuments\":%zu,\"fetchedAt\":\"%s\","
                           "\"refetchBy\":\"%s\"}",
                      j - i, rows[i].source_at, refetch_route(kind));
        } else {
            sb_printf(&sb, "%-8s %-8s %-7s %-21s %zu stale  ",
                      rows[i].d->source, tier_name(rows[i].s->tier), kind,
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
    close_all(stores, nstores);
    return KB_EXIT_OK;
}
