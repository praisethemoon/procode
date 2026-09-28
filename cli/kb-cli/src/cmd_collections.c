#include "cmd.h"
#include "help.h"

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

/* PATCH and DELETE /collections/{name} (§7) live here too, as verbs in the
 * first positional:
 *
 *   kb collections                     list
 *   kb collections rename <old> <new>  PATCH
 *   kb collections delete <name>       DELETE
 *
 * A COLLECTION IS NOT STORED ANYWHERE, so neither is a write to one. §1.3
 * makes a collection a field on a Source, created implicitly on first ingest;
 * renaming a topic is therefore appending a revised `source` record for each
 * source in it, and deleting one is appending a `forget` record for each.
 * Both are ordinary append-only motions, and both are refused before the
 * first append when they would be wrong — so a half-renamed collection is not
 * a state this can produce except by crashing, and a crash mid-rename leaves
 * the sources it reached under the new name and the rest under the old, which
 * a second rename finishes.
 *
 * NEITHER TOUCHES THE KEYWORD INDEX. fts.db's digest covers what decides a
 * chunk's text and identity — id, content hash, chunk range, mime, path — and
 * a collection is none of those; it is read live from the log as a search
 * filter. So a rename cannot stale the index, and rebuilding after one would
 * be work for nothing. */

/* The flags kb collections accepts: the ones its --help lists (help.c). */
#define VALUE_FLAGS help_values("collections")
#define BOOL_FLAGS help_bools("collections")

typedef struct {
    const char *name;
    int64_t documents;
    int64_t bytes;
    const char *oldest; /* ISO-8601 UTC, so oldest is a string minimum */
} Row;

static Row *find(Row *v, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(v[i].name, name) == 0)
            return &v[i];
    return NULL;
}

static int compare(const void *x, const void *y) {
    const Row *a = x, *b = y;
    return strcmp(a->name, b->name);
}

/* The write half. `to` is NULL for a delete; `with_documents` lets a delete
 * forget the documents the collection holds instead of refusing. */
static int32_t collection_write(Arena *a, bool json, const char *from,
                                const char *to, bool with_documents) {
    if (!from || !from[0]) {
        err_out(json, "usage", "kb collections %s expects a collection name",
                to ? "rename" : "delete");
        return KB_EXIT_ERR;
    }
    if (to && (!to[0] || strchr(to, '/') || strchr(to, '\\'))) {
        /* §1.3: collections are a flat named scope. A separator suggests
         * nesting, which does not exist. */
        err_out(json, "usage",
                "a collection name must be non-empty and must not nest");
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

    /* Everything below is decided from the log this locked open just read. */
    uint32_t nsources = 0, ndocs = 0;
    bool target_exists = false;
    for (size_t i = 0; i < s.sources.n; i++) {
        if (strcmp(s.sources.v[i].collection, from) == 0)
            nsources++;
        else if (to && strcmp(s.sources.v[i].collection, to) == 0)
            target_exists = true;
    }
    for (size_t i = 0; i < s.documents.n; i++) {
        const Source *src = src_by_id(&s.sources, s.documents.v[i].source);
        if (src && strcmp(src->collection, from) == 0)
            ndocs++;
    }
    if (nsources == 0) {
        store_close(&s);
        err_out(json, "not_found", "no collection \"%s\"", from);
        return KB_EXIT_ERR;
    }
    if (!to && ndocs > 0 && !with_documents) {
        /* §11's collection_in_use, with the count §11 asks for. Forgetting a
         * topic forgets what it holds, so that is asked for by name rather
         * than done because a collection happened to be non-empty. */
        store_close(&s);
        errdet_begin("collection_in_use");
        errdet_str("collection", from);
        errdet_int("documents", (int64_t)ndocs);
        err_out(json, "collection_in_use",
                "\"%s\" still holds %lu document%s; pass --with-documents to "
                "forget them with it",
                from, (unsigned long)ndocs, ndocs == 1 ? "" : "s");
        return KB_EXIT_ERR;
    }
    if (to && strcmp(from, to) == 0) {
        store_close(&s);
        err_out(json, "usage", "\"%s\" is already its own name", from);
        return KB_EXIT_ERR;
    }

    char now[32];
    plat_timestamp(now);
    if (to) {
        for (size_t i = 0; i < s.sources.n; i++) {
            const Source *src = &s.sources.v[i];
            if (strcmp(src->collection, from) != 0)
                continue;
            Source rev = *src;
            rev.collection = to;
            size_t len;
            char *line = doc_encode_source(a, &rev, &len);
            if (!store_append(&s, STORE_SOURCES, line, len, err, sizeof err)) {
                store_close(&s);
                err_out(json, "internal", "%s", err);
                return KB_EXIT_FATAL;
            }
        }
    } else {
        /* The documents, then their sources, through the one path every
         * forgetting command takes. */
        const char **docs =
            (const char **)arena_alloc(a, (ndocs ? ndocs : 1) * sizeof(char *));
        const char **srcs = (const char **)arena_alloc(
            a, (nsources ? nsources : 1) * sizeof(char *));
        size_t nd = 0, ns = 0;
        for (size_t i = 0; i < s.documents.n; i++) {
            const Source *src = src_by_id(&s.sources, s.documents.v[i].source);
            if (src && strcmp(src->collection, from) == 0)
                docs[nd++] = s.documents.v[i].id;
        }
        for (size_t i = 0; i < s.sources.n; i++) {
            if (strcmp(s.sources.v[i].collection, from) == 0)
                srcs[ns++] = s.sources.v[i].id;
        }
        if (!forget_records(a, &s, docs, nd, srcs, ns, err, sizeof err)) {
            store_close(&s);
            err_out(json, "internal", "%s", err);
            return KB_EXIT_FATAL;
        }
    }

    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_printf(&sb, "{\"ok\":true,\"action\":\"%s\",",
                  to ? "rename" : "delete");
        sb_puts(&sb, "\"collection\":");
        json_escape_c(&sb, from);
        if (to) {
            sb_puts(&sb, ",\"renamedTo\":");
            json_escape_c(&sb, to);
            /* Renaming onto a name that already exists MERGES the two, which
             * is a reasonable thing to want and a terrible thing to discover
             * later. It is allowed and it is reported; it is never silent. */
            sb_printf(&sb, ",\"merged\":%s", target_exists ? "true" : "false");
        }
        sb_printf(&sb, ",\"sources\":%lu,\"documents\":%lu,\"at\":\"%s\"}",
                  (unsigned long)nsources, (unsigned long)ndocs, now);
        puts(sb_finish(&sb));
    } else if (to) {
        printf("renamed %s to %s (%lu source%s, %lu document%s)%s\n", from, to,
               (unsigned long)nsources, nsources == 1 ? "" : "s",
               (unsigned long)ndocs, ndocs == 1 ? "" : "s",
               target_exists ? "  (merged into an existing collection)" : "");
    } else {
        printf("forgot %s (%lu source%s, %lu document%s)\n", from,
               (unsigned long)nsources, nsources == 1 ? "" : "s",
               (unsigned long)ndocs, ndocs == 1 ? "" : "s");
    }
    store_close(&s);
    return KB_EXIT_OK;
}

int32_t cmd_collections(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    /* A verb in the first positional. A collection genuinely named "rename"
     * is still listable — `kb collections` lists everything — and is the only
     * thing this costs. */
    const char *verb = positional_arg(argc, argv, VALUE_FLAGS, 0);
    if (verb && strcmp(verb, "rename") == 0) {
        const char *from = positional_arg(argc, argv, VALUE_FLAGS, 1);
        const char *to = positional_arg(argc, argv, VALUE_FLAGS, 2);
        /* Checked here rather than inside: a missing new name would reach
         * collection_write as a NULL `to`, which is how a delete is spelled,
         * and "rename" must never be able to become a delete. */
        if (!to) {
            err_out(json, "usage",
                    "kb collections rename expects <old> <new>");
            return KB_EXIT_ERR;
        }
        if (has_flag(argc, argv, VALUE_FLAGS, "--with-documents")) {
            err_out(json, "usage", "--with-documents belongs to delete");
            return KB_EXIT_ERR;
        }
        return collection_write(a, json, from, to, false);
    }
    if (verb && strcmp(verb, "delete") == 0)
        return collection_write(
            a, json, positional_arg(argc, argv, VALUE_FLAGS, 1), NULL,
            has_flag(argc, argv, VALUE_FLAGS, "--with-documents"));
    if (verb) {
        err_out(json, "usage",
                "kb collections takes no argument, or \"rename <old> <new>\", "
                "or \"delete <name>\"");
        return KB_EXIT_ERR;
    }
    char err[512];
    char dir[KB_PATH_MAX];
    if (!store_resolve(dir, sizeof dir, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return KB_EXIT_ERR;
    }

    Row *rows = NULL;
    size_t nrows = 0, cap = 0;
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
        Row *r = find(rows, nrows, src->collection);
        if (!r) {
            if (nrows == cap) {
                size_t old_cap = cap;
                cap = cap ? cap * 2 : 8;
                rows = arena_realloc(a, rows, sizeof *rows * old_cap,
                                    sizeof *rows * cap);
            }
            r = &rows[nrows++];
            r->name = src->collection;
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
            sb_printf(&sb, ",\"documents\":%lld,\"bytes\":%lld",
                      (long long)rows[i].documents,
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
            sb_printf(&sb, "%6lld doc %10lld b  %-21s ",
                      (long long)rows[i].documents,
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
