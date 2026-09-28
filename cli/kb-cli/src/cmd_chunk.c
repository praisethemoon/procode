#include "cmd.h"
#include "help.h"

/* GET /chunks/{id} (§4): the full text of one chunk and its neighbours.
 *
 * This is the other half of "search returns snippets only". A list has to
 * be safe to hand an agent without burying whatever it was doing, so it
 * carries windows; when one of those windows is worth reading, this route
 * is how the passage is actually fetched. Asking for one chunk is an
 * explicit act with a bounded answer, which is what makes it safe to return
 * everything.
 *
 * Chunks are derived and are recomputed from the blob, but their
 * identifiers are not: the document's record holds the contiguous range
 * reserved at ingest, so C-(chunkBase+i) names the same passage before and
 * after a rebuild (§1.1).
 */

/* The flags kb chunk accepts: the ones its --help lists (help.c). */
#define VALUE_FLAGS help_values("chunk")
#define BOOL_FLAGS help_bools("chunk")

static void chunk_json(StrBuf *sb, const Document *d, const Source *src,
                       const Chunk *c, uint32_t ordinal, const char *text,
                       bool with_text) {
    sb_printf(sb,
              "{\"id\":\"C-%lld\",\"document\":\"%s\",\"source\":\"%s\","
              "\"ordinal\":%lu",
              (long long)(d->chunk_base + (int64_t)ordinal), d->id, d->source,
              (unsigned long)ordinal);
    sb_puts(sb, ",\"collection\":");
    json_escape_c(sb, src ? src->collection : "");
    sb_puts(sb, ",\"title\":");
    json_escape_c(sb, d->title ? d->title : "");
    sb_puts(sb, ",\"heading\":");
    if (c->heading)
        json_escape_c(sb, c->heading);
    else
        sb_puts(sb, "null");
    sb_printf(sb, ",\"span\":{\"start\":%zu,\"end\":%zu},\"tokens\":%lu",
              c->start, c->end, (unsigned long)c->tokens);
    sb_printf(sb, ",\"fetchedAt\":\"%s\"", d->fetched_at ? d->fetched_at : "");
    if (with_text) {
        sb_puts(sb, ",\"text\":");
        json_escape(sb, text + c->start, c->end - c->start);
    }
    sb_putc(sb, '}');
}

/* The chunk's own bytes, not flattened: this route exists to hand back the
 * passage exactly as stored. One terminator, and only if the passage does
 * not already end in one. */
static void put_text(const char *text, const Chunk *c) {
    size_t n = c->end - c->start;
    fwrite(text + c->start, 1, n, stdout);
    if (n == 0 || text[c->end - 1] != '\n')
        fputc('\n', stdout);
}

static void chunk_human(Arena *a, const Document *d, const Source *src,
                        const Chunk *c, uint32_t ordinal, const char *text) {
    StrBuf sb;
    sb_init(&sb, a);
    sb_printf(&sb, "C-%lld  %s  [%zu,%zu)  %lu tokens\n",
              (long long)(d->chunk_base + (int64_t)ordinal), d->id, c->start,
              c->end, (unsigned long)c->tokens);
    sb_puts(&sb, "collection ");
    sb_puts_safe(&sb, src ? src->collection : "");
    sb_puts(&sb, "\ntitle      ");
    sb_puts_safe(&sb, d->title ? d->title : "");
    sb_puts(&sb, "\nheading    ");
    sb_puts_safe(&sb, c->heading ? c->heading : "(none)");
    sb_putc(&sb, '\n');
    fputs(sb_finish(&sb), stdout);
    put_text(text, c);
}

int32_t cmd_chunk(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    const char *id = positional_arg(argc, argv, VALUE_FLAGS, 0);
    if (!id) {
        err_out(json, "usage", "kb chunk expects a chunk id, e.g. C-99812");
        return KB_EXIT_ERR;
    }
    int64_t num = kb_id_num(id, 'C');
    if (num == 0) {
        err_out(json, "usage", "\"%s\" is not a chunk id (expected C-<n>)", id);
        return KB_EXIT_ERR;
    }
    /* One neighbour each side by default: §4 says this route returns the
     * chunk and its neighbours, and one is what makes a passage readable in
     * place without turning a single-item request into a document dump. */
    int64_t expand = 1;
    const char *expand_s = flag_value(argc, argv, VALUE_FLAGS, "--expand");
    if (expand_s) {
        expand = strtoll(expand_s, NULL, 10);
        if (expand < 0) {
            err_out(json, "usage", "--expand expects a count");
            return KB_EXIT_ERR;
        }
        if (expand > KB_EXPAND_MAX)
            expand = KB_EXPAND_MAX;
    }
    char err[512];
    char dir[KB_PATH_MAX];
    if (!store_resolve(dir, sizeof dir, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return KB_EXIT_ERR;
    }

    Store s;
    const char *code;
    if (!store_open(a, &s, dir, false, err, sizeof err, &code)) {
        err_out(json, code, "%s", err);
        return KB_EXIT_ERR;
    }
    uint32_t ordinal = 0;
    const Document *d = doc_by_chunk(&s.documents, num, &ordinal);
    if (!d) {
        store_close(&s);
        err_out(json, "not_found", "no chunk %s", id);
        return KB_EXIT_ERR;
    }
    char *text;
    size_t len;
    Chunks ch;
    if (!doc_chunks(a, &s, d, &text, &len, &ch)) {
        store_close(&s);
        err_out(json, "not_found", "%s has no blob for %s", d->id,
                d->content_hash);
        return KB_EXIT_ERR;
    }
    if (ordinal >= ch.n) {
        /* The log reserved a range this blob no longer fills, so the
         * chunker has changed under the store (§7's reindex, not
         * rebuild). §11's index_stale names the structure at fault. */
        store_close(&s);
        static const char *const structures[] = {"chunks"};
        errdet_begin("index_stale");
        errdet_strs("structures", structures, 1);
        errdet_str("document", d->id);
        err_out(json, "index_stale",
                "%s records %lu chunks and its text now splits into %zu; "
                "the chunker changed (run \"kb reindex\" when it exists)",
                d->id, (unsigned long)d->chunk_count, ch.n);
        return KB_EXIT_ERR;
    }
    const Source *src = src_by_id(&s.sources, d->source);
    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_puts(&sb, "{\"ok\":true,\"chunk\":");
        chunk_json(&sb, d, src, &ch.v[ordinal], ordinal, text, true);
        sb_puts(&sb, ",\"neighbours\":[");
        bool first = true;
        for (int64_t o = (int64_t)ordinal - expand;
             o <= (int64_t)ordinal + expand; o++) {
            if (o == (int64_t)ordinal || o < 0 || o >= (int64_t)ch.n)
                continue;
            if (!first)
                sb_putc(&sb, ',');
            first = false;
            chunk_json(&sb, d, src, &ch.v[o], (uint32_t)o, text, true);
        }
        sb_puts(&sb, "]}");
        puts(sb_finish(&sb));
    } else {
        chunk_human(a, d, src, &ch.v[ordinal], ordinal, text);
        for (int64_t o = (int64_t)ordinal - expand;
             o <= (int64_t)ordinal + expand; o++) {
            if (o == (int64_t)ordinal || o < 0 || o >= (int64_t)ch.n)
                continue;
            printf("--- C-%lld (%s)\n", (long long)(d->chunk_base + o),
                   o < (int64_t)ordinal ? "before" : "after");
            put_text(text, &ch.v[o]);
        }
    }
    store_close(&s);
    return KB_EXIT_OK;
}
