#include "cmd.h"

/* §2's GET /sources and GET /sources/{id}:
 *
 *   kb sources [--collection C] [--kind K] [--status S] [--q text]
 *                                           one row per source
 *   kb sources show S-n                      the source, its documents, and
 *                                            every time they were fetched
 *
 * A Source's counts, dates and hash are derived from its documents rather
 * than stored (see source_facts), so a row here can never disagree with the
 * documents it describes. Reading a source again is `kb refresh S-n`.
 */

static const char *const VALUE_FLAGS[] = {"--collection", "--kind",
                                          "--status", "--q", NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

static int32_t show(Arena *a, bool json, Store *s, const char *id) {
    const Source *src = src_by_id(&s->sources, id);
    if (!src) {
        err_out(json, "not_found", "no source %s", id);
        return KB_EXIT_ERR;
    }
    char err[512];
    Staleness st;
    staleness_init(&st, NULL, err, sizeof err);
    StrBuf sb;
    sb_init(&sb, a);
    if (json) {
        sb_puts(&sb, "{\"ok\":true,\"source\":{");
        json_source(&sb, s, src);
        sb_puts(&sb, "},\"documents\":[");
    } else {
        sb_printf(&sb, "%s  %s  ", src->id, src->kind);
        sb_puts_safe(&sb, src->collection);
        sb_puts(&sb, "\nlocator    ");
        sb_puts_safe(&sb, src->locator);
        sb_putc(&sb, '\n');
    }
    bool first = true;
    StrBuf hist;
    sb_init(&hist, a);
    bool first_fetch = true;
    for (size_t i = 0; i < s->documents.n; i++) {
        const Document *d = &s->documents.v[i];
        if (strcmp(d->source, src->id) != 0)
            continue;
        Fetch *fetches;
        size_t nf;
        if (!doclog_fetches(a, s->documents_path, d->id, &fetches, &nf, err,
                            sizeof err)) {
            err_out(json, "internal", "%s", err);
            return KB_EXIT_FATAL;
        }
        if (json) {
            sb_printf(&sb, "%s{", first ? "" : ",");
            json_document(&sb, d, src, &st);
            sb_putc(&sb, '}');
            for (size_t k = 0; k < nf; k++) {
                sb_printf(&hist,
                          "%s{\"document\":\"%s\",\"fetchedAt\":\"%s\","
                          "\"changed\":%s}",
                          first_fetch ? "" : ",", d->id, fetches[k].at,
                          fetches[k].changed ? "true" : "false");
                first_fetch = false;
            }
        } else {
            sb_printf(&sb, "document   %s  ", d->id);
            sb_puts_safe(&sb, d->title ? d->title : "");
            sb_putc(&sb, '\n');
            for (size_t k = 0; k < nf; k++)
                sb_printf(&sb, "  fetched  %s  %s\n", fetches[k].at,
                          fetches[k].changed ? "(new text)" : "(unchanged)");
        }
        first = false;
    }
    if (json) {
        sb_puts(&sb, "],\"history\":[");
        sb_puts(&sb, sb_finish(&hist));
        sb_puts(&sb, "]}");
        puts(sb_finish(&sb));
    } else {
        fputs(sb_finish(&sb), stdout);
    }
    return KB_EXIT_OK;
}

int32_t cmd_sources(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    const char *verb = positional_arg(argc, argv, VALUE_FLAGS, 0);
    const char *id = positional_arg(argc, argv, VALUE_FLAGS, 1);
    if (verb && (strcmp(verb, "show") != 0 || !id || kb_id_num(id, 'S') == 0 ||
                 positional_arg(argc, argv, VALUE_FLAGS, 2))) {
        err_out(json, "usage",
                "kb sources lists every source; kb sources show S-n shows one");
        return KB_EXIT_ERR;
    }
    const char *collection = flag_value(argc, argv, VALUE_FLAGS, "--collection");
    const char *kind = flag_value(argc, argv, VALUE_FLAGS, "--kind");
    const char *status = flag_value(argc, argv, VALUE_FLAGS, "--status");
    const char *q = flag_value(argc, argv, VALUE_FLAGS, "--q");
    if (verb && (collection || kind || status || q)) {
        err_out(json, "usage",
                "--collection, --kind, --status and --q narrow the list, not show");
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
    if (!store_open(a, &s, dir, false, err, sizeof err, &code)) {
        err_out(json, code, "%s", err);
        return KB_EXIT_ERR;
    }
    int32_t rc;
    if (verb) {
        rc = show(a, json, &s, id);
    } else {
        StrBuf sb;
        sb_init(&sb, a);
        if (json)
            sb_puts(&sb, "{\"ok\":true,\"sources\":[");
        size_t shown = 0;
        for (size_t i = 0; i < s.sources.n; i++) {
            const Source *src = &s.sources.v[i];
            if (collection && strcmp(src->collection, collection) != 0)
                continue;
            if (kind && strcmp(src->kind, kind) != 0)
                continue;
            if (status && strcmp(src->status ? src->status : "ok", status) != 0)
                continue;
            if (q && !icase_contains(src->title ? src->title : "", q) &&
                !icase_contains(src->locator, q))
                continue;
            if (json) {
                sb_printf(&sb, "%s{", shown ? "," : "");
                json_source(&sb, &s, src);
                sb_putc(&sb, '}');
            } else {
                SourceFacts f = source_facts(a, &s, src->id);
                sb_printf(&sb, "%-6s %-7s %3lu doc  ", src->id, src->kind,
                          (unsigned long)f.doc_count);
                sb_puts_safe(&sb, src->collection);
                sb_puts(&sb, "  ");
                sb_puts_safe(&sb, src->locator);
                sb_putc(&sb, '\n');
            }
            shown++;
        }
        if (json) {
            sb_printf(&sb, "],\"count\":%zu}", shown);
            puts(sb_finish(&sb));
        } else if (shown == 0) {
            puts("no sources");
        } else {
            fputs(sb_finish(&sb), stdout);
        }
        rc = KB_EXIT_OK;
    }
    store_close(&s);
    return rc;
}
