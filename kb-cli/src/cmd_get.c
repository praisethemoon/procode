#include "cmd.h"

/* GET /documents/{id} (§2) with ?include=text,chunks,links. */

static const char *const VALUE_FLAGS[] = {"--include", "--store",
                                          "--older-than", "--olderThan", NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

static bool include_has(const char *list, const char *what) {
    if (!list)
        return false;
    size_t n = strlen(what);
    for (const char *p = list; *p;) {
        const char *comma = strchr(p, ',');
        size_t seg = comma ? (size_t)(comma - p) : strlen(p);
        if (seg == n && strncmp(p, what, n) == 0)
            return true;
        if (!comma)
            break;
        p = comma + 1;
    }
    return false;
}

/* Chunks are derived, so they are recomputed from the blob rather than
 * stored — but their identifiers are not derived: the document records the
 * contiguous range reserved at ingest, so the same chunk keeps the same id
 * across a rebuild (§1.1). The parameters come from index/model.json, which
 * is what the index was actually built with; using the current defaults
 * instead would describe chunks the store does not contain. */
static void emit_chunks(StrBuf *sb, Arena *a, Store *s, const Document *d,
                        bool with_text) {
    char *text;
    size_t len;
    Chunks ch;
    if (!doc_chunks(a, s, d, &text, &len, &ch)) {
        sb_puts(sb, "null");
        return;
    }
    sb_putc(sb, '[');
    for (size_t i = 0; i < ch.n; i++) {
        if (i)
            sb_putc(sb, ',');
        sb_printf(sb, "{\"id\":\"C-%lld\",\"document\":\"%s\",\"ordinal\":%zu",
                  (long long)(d->chunk_base + (int64_t)i), d->id, i);
        sb_puts(sb, ",\"heading\":");
        if (ch.v[i].heading)
            json_escape_c(sb, ch.v[i].heading);
        else
            sb_puts(sb, "null");
        sb_printf(sb, ",\"span\":{\"start\":%zu,\"end\":%zu},\"tokens\":%lu",
                  ch.v[i].start, ch.v[i].end, (unsigned long)ch.v[i].tokens);
        if (with_text) {
            sb_puts(sb, ",\"text\":");
            json_escape(sb, text + ch.v[i].start,
                        ch.v[i].end - ch.v[i].start);
        }
        sb_putc(sb, '}');
    }
    sb_putc(sb, ']');
}

int32_t cmd_get(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    const char *id = positional_arg(argc, argv, VALUE_FLAGS, 0);
    if (!id) {
        err_out(json, "usage", "kb get expects a document id, e.g. D-241");
        return KB_EXIT_ERR;
    }
    if (kb_id_num(id, 'D') == 0) {
        err_out(json, "usage", "\"%s\" is not a document id (expected D-<n>)",
                id);
        return KB_EXIT_ERR;
    }
    const char *include = flag_value(argc, argv, VALUE_FLAGS, "--include");
    bool want_text = include_has(include, "text");
    bool want_chunks = include_has(include, "chunks");
    bool want_links = include_has(include, "links");
    StoreSel sel;
    if (!store_sel_parse(flag_value(argc, argv, VALUE_FLAGS, "--store"),
                         &sel)) {
        err_out(json, "usage", "--store expects project, global or all");
        return KB_EXIT_ERR;
    }

    char err[512];
    Staleness st;
    if (!staleness_init(&st, older_than_arg(argc, argv, VALUE_FLAGS), err,
                        sizeof err)) {
        err_out(json, "usage", "%s", err);
        return KB_EXIT_ERR;
    }
    TierSet tiers;
    if (!tiers_resolve(sel, false, &tiers, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return KB_EXIT_ERR;
    }

    /* Identifiers are allocated per store, so D-5 can exist in both tiers.
     * tiers_resolve orders them project first, and the first match wins —
     * the same precedence §1.4 gives a document present in both. */
    for (size_t t = 0; t < tiers.n; t++) {
        Store s;
        const char *code;
        if (!store_open(a, &s, tiers.dir[t], tiers.tier[t], false, err,
                        sizeof err, &code)) {
            err_out(json, code, "%s", err);
            return KB_EXIT_ERR;
        }
        const Document *d = doc_by_id(&s.documents, id);
        if (!d) {
            store_close(&s);
            continue;
        }
        const Source *src = src_by_id(&s.sources, d->source);
        StrBuf sb;
        sb_init(&sb, a);
        if (json) {
            sb_puts(&sb, "{\"ok\":true,\"document\":{");
            json_document(&sb, &s, d, src, &st);
            sb_putc(&sb, '}');
            if (want_links) {
                sb_puts(&sb, ",\"links\":{\"outgoing\":");
                json_links(&sb, &s, d->id, true, &st);
                sb_puts(&sb, ",\"incoming\":");
                json_links(&sb, &s, d->id, false, &st);
                sb_putc(&sb, '}');
            }
            if (want_text) {
                char *text;
                size_t len;
                sb_puts(&sb, ",\"text\":");
                if (store_get_blob(&s, d->content_hash, &text, &len))
                    json_escape(&sb, text, len);
                else
                    sb_puts(&sb, "null");
            }
            if (want_chunks) {
                sb_puts(&sb, ",\"chunks\":");
                emit_chunks(&sb, a, &s, d, want_text);
            }
            sb_putc(&sb, '}');
            puts(sb_finish(&sb));
        } else {
            sb_printf(&sb, "%s  %s  %s\n", d->id, tier_name(s.tier),
                      d->content_hash);
            sb_puts(&sb, "title      ");
            sb_puts_safe(&sb, d->title ? d->title : "");
            sb_puts(&sb, "\ncollection ");
            sb_puts_safe(&sb, src ? src->collection : "");
            sb_puts(&sb, "\nlocator    ");
            sb_puts_safe(&sb, src ? src->locator : "");
            sb_printf(&sb, "\nsource     %s (%s)\n", d->source,
                      src ? src->kind : "?");
            sb_printf(&sb, "mime       %s\n", d->mime ? d->mime : "");
            sb_printf(&sb, "bytes      %llu\n", (unsigned long long)d->bytes);
            sb_printf(&sb, "fetchedAt  %s%s\n",
                      d->fetched_at ? d->fetched_at : "",
                      doc_stale(&st, d) ? "  (stale)" : "");
            sb_printf(&sb, "chunks     %lu (C-%lld..C-%lld)\n",
                      (unsigned long)d->chunk_count, (long long)d->chunk_base,
                      (long long)(d->chunk_base + d->chunk_count - 1));
            if (want_links) {
                for (size_t i = 0; i < s.documents.nlinks; i++) {
                    const Link *l = &s.documents.links[i];
                    bool out = strcmp(l->from, d->id) == 0;
                    if (!out && strcmp(l->to, d->id) != 0)
                        continue;
                    const char *far = out ? l->to : l->from;
                    const Document *fd = doc_by_id(&s.documents, far);
                    sb_printf(&sb, "link       %s %s %s", out ? "->" : "<-",
                              l->rel, far);
                    if (fd) {
                        sb_puts(&sb, "  ");
                        sb_puts_safe(&sb, fd->title ? fd->title : "");
                    } else {
                        sb_puts(&sb, "  (forgotten)");
                    }
                    sb_putc(&sb, '\n');
                }
            }
            fputs(sb_finish(&sb), stdout);
            if (want_chunks) {
                char *text;
                size_t len;
                Chunks ch;
                if (doc_chunks(a, &s, d, &text, &len, &ch)) {
                    for (size_t i = 0; i < ch.n; i++) {
                        StrBuf l;
                        sb_init(&l, a);
                        sb_printf(&l, "C-%lld  [%zu,%zu)  ",
                                  (long long)(d->chunk_base + (int64_t)i),
                                  ch.v[i].start, ch.v[i].end);
                        sb_puts_safe(&l, ch.v[i].heading ? ch.v[i].heading
                                                         : "(no heading)");
                        puts(sb_finish(&l));
                    }
                }
            }
            if (want_text) {
                char *text;
                size_t len;
                if (store_get_blob(&s, d->content_hash, &text, &len))
                    fwrite(text, 1, len, stdout);
            }
        }
        store_close(&s);
        return KB_EXIT_OK;
    }
    err_out(json, "not_found", "no document %s", id);
    return KB_EXIT_ERR;
}
