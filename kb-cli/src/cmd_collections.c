#include "cmd.h"

/* GET /collections (§7): the names, with what is under each.
 *
 * §7 asks only for "names with counts", but `index-ui.md` §4 shows the row a
 * reader actually gets — count, byte size, oldest fetch date — and those three
 * are one pass over the same documents. Computing them here rather than making
 * the UI walk every document to find out how big a topic is keeps §4's rule
 * that a list must not flood its caller.
 *
 * A collection is not stored anywhere (§1.3): it is created implicitly on
 * first ingest and exists only as a field on a Source. So the list is derived,
 * and a collection with no documents left simply stops existing — which is the
 * behaviour §7's DELETE /collections/{name} implies anyway. */

static const char *const VALUE_FLAGS[] = {"--store", NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

typedef struct {
    const char *name;
    Tier tier;
    int64_t documents;
    int64_t bytes;
    const char *oldest; /* ISO-8601 UTC, so oldest is a string minimum */
} Row;

/* Collections are per tier, not global: §1.4 is explicit that `win32-iocp` can
 * exist in both, and merging them here would claim a single topic where there
 * are two stores that happen to agree on a name. */
static Row *find(Row *v, size_t n, const char *name, Tier tier) {
    for (size_t i = 0; i < n; i++)
        if (v[i].tier == tier && strcmp(v[i].name, name) == 0)
            return &v[i];
    return NULL;
}

static int compare(const void *x, const void *y) {
    const Row *a = x, *b = y;
    if (a->tier != b->tier)
        return a->tier < b->tier ? -1 : 1;
    return strcmp(a->name, b->name);
}

int32_t cmd_collections(Arena *a, int32_t argc, char **argv) {
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
    if (!tiers_resolve(sel, false, &tiers, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return KB_EXIT_ERR;
    }

    Row *rows = NULL;
    size_t nrows = 0, cap = 0;
    for (size_t t = 0; t < tiers.n; t++) {
        Store s;
        const char *code;
        if (!store_open(a, &s, tiers.dir[t], tiers.tier[t], false, err,
                        sizeof err, &code)) {
            err_out(json, code, "%s", err);
            return KB_EXIT_ERR;
        }
        for (size_t i = 0; i < s.documents.n; i++) {
            const Document *d = &s.documents.v[i];
            const Source *src = src_by_id(&s.sources, d->source);
            if (!src || !src->collection)
                continue;
            Row *r = find(rows, nrows, src->collection, s.tier);
            if (!r) {
                if (nrows == cap) {
                    size_t old_cap = cap;
                    cap = cap ? cap * 2 : 8;
                    rows = arena_realloc(a, rows, sizeof *rows * old_cap,
                                        sizeof *rows * cap);
                }
                r = &rows[nrows++];
                r->name = src->collection;
                r->tier = s.tier;
                r->documents = 0;
                r->bytes = 0;
                r->oldest = NULL;
            }
            r->documents++;
            r->bytes += (int64_t)d->bytes;
            if (d->fetched_at &&
                (!r->oldest || strcmp(d->fetched_at, r->oldest) < 0))
                r->oldest = d->fetched_at;
        }
        store_close(&s);
    }
    /* Sorted, because the caller is a list and an order that depends on which
     * document happened to be filed first is one a reader cannot scan. */
    if (nrows > 1)
        qsort(rows, nrows, sizeof *rows, compare);

    StrBuf sb;
    sb_init(&sb, a);
    if (json) {
        sb_puts(&sb, "{\"ok\":true,\"collections\":[");
        for (size_t i = 0; i < nrows; i++) {
            if (i)
                sb_putc(&sb, ',');
            sb_puts(&sb, "{\"name\":");
            json_escape_c(&sb, rows[i].name);
            sb_printf(&sb, ",\"store\":\"%s\",\"documents\":%lld,\"bytes\":%lld",
                      tier_name(rows[i].tier), (long long)rows[i].documents,
                      (long long)rows[i].bytes);
            sb_puts(&sb, ",\"oldestFetchedAt\":");
            if (rows[i].oldest)
                json_escape_c(&sb, rows[i].oldest);
            else
                sb_puts(&sb, "null");
            sb_putc(&sb, '}');
        }
        sb_printf(&sb, "],\"count\":%lld}", (long long)nrows);
        puts(sb_finish(&sb));
    } else if (nrows == 0) {
        puts("no collections");
    } else {
        for (size_t i = 0; i < nrows; i++) {
            sb_printf(&sb, "%-8s %6lld doc %10lld b  %-21s ",
                      tier_name(rows[i].tier), (long long)rows[i].documents,
                      (long long)rows[i].bytes,
                      rows[i].oldest ? rows[i].oldest : "-");
            /* The name came out of a log line somebody else may have written,
             * so it is printed through the sanitiser like any other stored
             * text. */
            sb_puts_safe(&sb, rows[i].name);
            sb_putc(&sb, '\n');
        }
        fputs(sb_finish(&sb), stdout);
    }
    return KB_EXIT_OK;
}
