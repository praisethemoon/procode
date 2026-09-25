#include "cmd.h"

/* GET /documents (§2) with its ?collection=&source=&mime=&since= filters. */

static const char *const VALUE_FLAGS[] = {
    "--collection", "--source", "--mime",  "--since",
    "--limit",      "--store",  "--older-than", "--olderThan", NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

typedef struct {
    const char *collection;
    const char *source;
    const char *mime;
    const char *since;
} Filters;

static bool keep(const Filters *f, const Document *d, const Source *src) {
    if (f->collection &&
        (!src || strcmp(src->collection, f->collection) != 0))
        return false;
    if (f->source && strcmp(d->source, f->source) != 0)
        return false;
    if (f->mime && (!d->mime || strcmp(d->mime, f->mime) != 0))
        return false;
    /* ISO-8601 UTC with a fixed layout sorts lexicographically, so "newer
     * than" is a string comparison and needs no calendar. */
    if (f->since && (!d->fetched_at || strcmp(d->fetched_at, f->since) < 0))
        return false;
    return true;
}

int32_t cmd_ls(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    Filters f;
    f.collection = flag_value(argc, argv, VALUE_FLAGS, "--collection");
    f.source = flag_value(argc, argv, VALUE_FLAGS, "--source");
    f.mime = flag_value(argc, argv, VALUE_FLAGS, "--mime");
    f.since = flag_value(argc, argv, VALUE_FLAGS, "--since");
    const char *limit_s = flag_value(argc, argv, VALUE_FLAGS, "--limit");
    int64_t limit = limit_s ? strtoll(limit_s, NULL, 10) : 0;
    if (limit_s && limit <= 0) {
        err_out(json, "usage", "--limit expects a positive number");
        return KB_EXIT_ERR;
    }
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

    StrBuf sb;
    sb_init(&sb, a);
    if (json)
        sb_puts(&sb, "{\"ok\":true,\"documents\":[");
    int64_t shown = 0;
    bool first = true;
    for (size_t t = 0; t < tiers.n; t++) {
        Store s;
        const char *code;
        /* A reader takes no lock and writes nothing: a stale read is
         * recoverable, a reader that mutates the store is not. */
        if (!store_open(a, &s, tiers.dir[t], tiers.tier[t], false, err,
                        sizeof err, &code)) {
            err_out(json, code, "%s", err);
            return KB_EXIT_ERR;
        }
        for (size_t i = 0; i < s.documents.n; i++) {
            const Document *d = &s.documents.v[i];
            const Source *src = src_by_id(&s.sources, d->source);
            if (!keep(&f, d, src))
                continue;
            if (limit && shown >= limit)
                break;
            shown++;
            if (json) {
                if (!first)
                    sb_putc(&sb, ',');
                first = false;
                sb_putc(&sb, '{');
                json_document(&sb, &s, d, src, &st);
                sb_putc(&sb, '}');
            } else {
                sb_printf(&sb, "%-8s %-8s %-14s %-21s %-5s ", d->id,
                          tier_name(s.tier), src ? src->collection : "-",
                          d->fetched_at ? d->fetched_at : "-",
                          doc_stale(&st, d) ? "stale" : "");
                sb_puts_safe(&sb, d->title ? d->title : "");
                sb_putc(&sb, '\n');
            }
        }
        store_close(&s);
    }
    if (json) {
        /* The threshold that produced every `stale` above, so a caller never
         * has to know which default it got (§5). Not the cutoff instant: a
         * list of rows out of the log must stay a function of the log, and a
         * reading of the clock in it would make two identical stores answer
         * differently. */
        sb_printf(&sb, "],\"count\":%lld,\"olderThan\":\"%s\"}",
                  (long long)shown, st.spec);
        puts(sb_finish(&sb));
    } else if (shown == 0) {
        puts("no documents");
    } else {
        fputs(sb_finish(&sb), stdout);
    }
    return KB_EXIT_OK;
}
