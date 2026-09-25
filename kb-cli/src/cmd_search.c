#include "cmd.h"

#include "rank.h"
#include "snippet.h"

/* GET /search (§4).
 *
 * WHAT THIS BUILD CAN ANSWER. §4's default is hybrid, and hybrid needs an
 * embedding model this build does not have (§8). Rather than pretend, the
 * default here is `keyword` and every response says `"mode":"keyword"`, so
 * a caller can see what actually ran instead of inferring it. Asking for
 * `hybrid` or `semantic` explicitly is refused with `model_missing` and a
 * sentence saying why: a search that silently answered a hybrid question
 * with half a hybrid answer would be worse than one that refused, because
 * the caller would go on believing the vector side had been consulted.
 *
 * SNIPPETS ONLY. §4: "a list must not be able to flood a caller's context."
 * Every passage returned here is a window of at most KB_SNIPPET_BYTES,
 * chosen for where the query terms actually are. `kb chunk` is the route to
 * a whole chunk and `kb get --include text` to a whole document, and both
 * are single-item requests by construction.
 *
 * TWO TIERS, FUSED BY RANK. Each tier is searched on its own index and the
 * two ranked lists are combined by reciprocal rank fusion (rank.h). A
 * document present in both with the same content hash is returned once,
 * from the project tier, flagged `alsoGlobal` — and the global copy is
 * removed before its tier is ranked, so it does not silently occupy a rank
 * that a different answer could have had.
 */

static const char *const VALUE_FLAGS[] = {
    "--collection", "--mode",  "--k",     "--expand",   "--store",
    "--source",     "--mime",  "--since", "--min-score", "--minScore",
    NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

typedef enum { MODE_KEYWORD, MODE_HYBRID, MODE_SEMANTIC } SearchMode;

/* ---- filters ----------------------------------------------------------- */

/* §4 takes `collection` as a comma-separated scope. An empty element is
 * skipped rather than matched, so a trailing comma is not a filter that
 * nothing satisfies. */
static bool csv_has(const char *csv, const char *v) {
    if (!csv)
        return true;
    size_t n = strlen(v);
    for (const char *p = csv; *p;) {
        const char *comma = strchr(p, ',');
        size_t seg = comma ? (size_t)(comma - p) : strlen(p);
        if (seg == n && strncmp(p, v, n) == 0)
            return true;
        if (!comma)
            break;
        p = comma + 1;
    }
    return false;
}

typedef struct {
    Store *s;
    const char *collection;
    const char *source;
    const char *mime;
    const char *since;
    /* Content hashes held by the project tier. A global chunk whose document
     * is one of these is not a second answer, it is the same answer (§1.4). */
    const StrSet *exclude;
} DocFilter;

static bool doc_keep(uint32_t doc_index, void *ud) {
    const DocFilter *f = (const DocFilter *)ud;
    if (doc_index >= f->s->documents.n)
        return false;
    const Document *d = &f->s->documents.v[doc_index];
    const Source *src = src_by_id(&f->s->sources, d->source);
    if (f->collection && (!src || !csv_has(f->collection, src->collection)))
        return false;
    if (f->source && strcmp(d->source, f->source) != 0)
        return false;
    if (f->mime && (!d->mime || strcmp(d->mime, f->mime) != 0))
        return false;
    /* ISO-8601 UTC with a fixed layout sorts lexicographically, so "newer
     * than" needs no calendar. */
    if (f->since && (!d->fetched_at || strcmp(d->fetched_at, f->since) < 0))
        return false;
    if (f->exclude && strset_has(f->exclude, d->content_hash))
        return false;
    return true;
}

/* ---- per-tier state ---------------------------------------------------- */

typedef struct {
    Store s;
    FtsIndex ix;
    bool open;
} TierState;

typedef struct {
    size_t tier;
    uint32_t chunk_index;
    double bm25;
} Cand;

/* Blob + chunk boundaries for one document, kept for the handful of hits
 * being rendered so several hits in the same document read it once. */
typedef struct {
    size_t tier;
    uint32_t doc_index;
    char *text;
    size_t len;
    Chunks ch;
    bool ok;
} DocText;

static DocText *doc_text(Arena *a, TierState *ts, size_t tier,
                         uint32_t doc_index, DocText *cache, size_t *ncache) {
    for (size_t i = 0; i < *ncache; i++) {
        if (cache[i].tier == tier && cache[i].doc_index == doc_index)
            return &cache[i];
    }
    DocText *e = &cache[(*ncache)++];
    memset(e, 0, sizeof(*e));
    e->tier = tier;
    e->doc_index = doc_index;
    const Document *d = &ts->s.documents.v[doc_index];
    e->ok = doc_chunks(a, &ts->s, d, &e->text, &e->len, &e->ch);
    return e;
}

static void put_matched(StrBuf *sb, uint32_t tags) {
    static const struct {
        uint32_t bit;
        const char *name;
    } paths[] = {{RANK_TAG_KEYWORD, "keyword"},
                 {RANK_TAG_SEMANTIC, "semantic"},
                 {0, NULL}};
    sb_putc(sb, '[');
    bool first = true;
    for (int32_t i = 0; paths[i].name; i++) {
        if (!(tags & paths[i].bit))
            continue;
        if (!first)
            sb_putc(sb, ',');
        first = false;
        sb_printf(sb, "\"%s\"", paths[i].name);
    }
    sb_putc(sb, ']');
}

/* ---- the command ------------------------------------------------------- */

int32_t cmd_search(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    const char *query = positional_arg(argc, argv, VALUE_FLAGS, 0);
    if (!query || !query[0]) {
        err_out(json, "usage", "kb search expects a query");
        return KB_EXIT_ERR;
    }

    SearchMode mode = MODE_KEYWORD;
    const char *mode_s = flag_value(argc, argv, VALUE_FLAGS, "--mode");
    if (mode_s) {
        if (strcmp(mode_s, "keyword") == 0)
            mode = MODE_KEYWORD;
        else if (strcmp(mode_s, "hybrid") == 0)
            mode = MODE_HYBRID;
        else if (strcmp(mode_s, "semantic") == 0)
            mode = MODE_SEMANTIC;
        else {
            err_out(json, "usage",
                    "--mode expects keyword, hybrid or semantic");
            return KB_EXIT_ERR;
        }
    }
    if (mode != MODE_KEYWORD) {
        /* §11's model_missing. Degrading to keyword and saying nothing would
         * hand back an answer the caller would read as hybrid. */
        err_out(json, "model_missing",
                "mode \"%s\" needs an embedding model and this build has "
                "none; only --mode keyword exists so far",
                mode_s);
        return KB_EXIT_ERR;
    }

    int64_t k = KB_SEARCH_K_DEFAULT;
    const char *k_s = flag_value(argc, argv, VALUE_FLAGS, "--k");
    if (k_s) {
        k = strtoll(k_s, NULL, 10);
        if (k <= 0) {
            err_out(json, "usage", "--k expects a positive number");
            return KB_EXIT_ERR;
        }
        /* §4 caps k at 100. Clamped rather than refused: the caller asked
         * for "as many as you have", and the cap is the store protecting a
         * context window, not a contract the caller broke. */
        if (k > KB_SEARCH_K_MAX)
            k = KB_SEARCH_K_MAX;
    }
    int64_t expand = 0;
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
    double min_score = 0.0;
    const char *min_s = flag_value(argc, argv, VALUE_FLAGS, "--min-score");
    if (!min_s)
        min_s = flag_value(argc, argv, VALUE_FLAGS, "--minScore");
    if (min_s) {
        char *end;
        min_score = strtod(min_s, &end);
        if (end == min_s || *end) {
            err_out(json, "usage", "--min-score expects a number");
            return KB_EXIT_ERR;
        }
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

    TierState ts[2];
    memset(ts, 0, sizeof ts);
    for (size_t t = 0; t < tiers.n; t++) {
        const char *code;
        if (!store_open(a, &ts[t].s, tiers.dir[t], tiers.tier[t], false, err,
                        sizeof err, &code)) {
            err_out(json, code, "%s", err);
            return KB_EXIT_ERR;
        }
        ts[t].open = true;
        /* A stale tier fails the whole search rather than dropping out of
         * it: a result list silently missing a store is a list the reader
         * cannot tell from a store with nothing in it. */
        if (!fts_open_store(a, &ts[t].s, &ts[t].ix, &code, err, sizeof err)) {
            for (size_t u = 0; u < tiers.n; u++) {
                if (ts[u].open)
                    store_close(&ts[u].s);
            }
            err_out(json, code, "%s; or narrow the search with --store", err);
            return KB_EXIT_ERR;
        }
    }

    /* §1.4's dedup, decided before either tier is ranked. Only meaningful
     * when both are in scope: a caller who asked for one tier is not asking
     * about the other, and consulting it would be reading a store that was
     * excluded. */
    bool both = tiers.n == 2;
    StrSet project_hashes, global_hashes;
    strset_init(&project_hashes, a);
    strset_init(&global_hashes, a);
    if (both) {
        /* Keyed off each store's own tier rather than its position, so the
         * rule survives any future change to the order tiers arrive in. */
        for (size_t t = 0; t < tiers.n; t++) {
            StrSet *into = ts[t].s.tier == TIER_PROJECT ? &project_hashes
                                                        : &global_hashes;
            for (size_t i = 0; i < ts[t].s.documents.n; i++)
                strset_add(into, ts[t].s.documents.v[i].content_hash);
        }
    }

    TermList q = token_terms(a, query, strlen(query));

    RankList lists[2];
    RankEntry *entries[2];
    Cand *cands[2];
    memset(lists, 0, sizeof lists);
    for (size_t t = 0; t < tiers.n; t++) {
        DocFilter f;
        memset(&f, 0, sizeof f);
        f.s = &ts[t].s;
        f.collection = flag_value(argc, argv, VALUE_FLAGS, "--collection");
        f.source = flag_value(argc, argv, VALUE_FLAGS, "--source");
        f.mime = flag_value(argc, argv, VALUE_FLAGS, "--mime");
        f.since = flag_value(argc, argv, VALUE_FLAGS, "--since");
        if (both && ts[t].s.tier == TIER_GLOBAL)
            f.exclude = &project_hashes;

        FtsHit *hits = NULL;
        size_t n = fts_search(a, &ts[t].ix, &q, min_score, doc_keep, &f,
                              (size_t)k, &hits);
        cands[t] = (Cand *)arena_alloc(a, (n ? n : 1) * sizeof(Cand));
        entries[t] =
            (RankEntry *)arena_alloc(a, (n ? n : 1) * sizeof(RankEntry));
        for (size_t i = 0; i < n; i++) {
            cands[t][i].tier = t;
            cands[t][i].chunk_index = hits[i].chunk_index;
            cands[t][i].bm25 = hits[i].score;
            /* The tier is part of the identity: project C-5 and global C-5
             * are different chunks in different stores. */
            entries[t][i].key =
                ((uint64_t)(t + 1) << 40) | (uint64_t)hits[i].chunk_index;
            entries[t][i].item = &cands[t][i];
        }
        lists[t].v = entries[t];
        lists[t].n = n;
        lists[t].tag = RANK_TAG_KEYWORD;
    }

    RankResult *fused = NULL;
    size_t nfused =
        rrf_fuse(a, lists, tiers.n, KB_RRF_K, (size_t)k, &fused);

    DocText *cache =
        (DocText *)arena_alloc0(a, ((size_t)k + 1) * sizeof(DocText));
    size_t ncache = 0;

    StrBuf sb;
    sb_init(&sb, a);
    if (json)
        sb_puts(&sb, "{\"ok\":true,\"mode\":\"keyword\",\"hits\":[");
    for (size_t i = 0; i < nfused; i++) {
        Cand *c = (Cand *)fused[i].item;
        TierState *t = &ts[c->tier];
        const FtsChunk *fc = &t->ix.chunks[c->chunk_index];
        const Document *d = &t->s.documents.v[fc->doc_index];
        const Source *src = src_by_id(&t->s.sources, d->source);
        DocText *dt = doc_text(a, t, c->tier, fc->doc_index, cache, &ncache);
        const char *heading = NULL;
        const char *snip = "";
        if (dt->ok && fc->ordinal < dt->ch.n) {
            const Chunk *ch = &dt->ch.v[fc->ordinal];
            heading = ch->heading;
            snip = snippet_of(a, dt->text, ch->start, ch->end, &q);
        }
        int64_t chunk_num = d->chunk_base + (int64_t)fc->ordinal;
        bool also_global =
            both && t->s.tier == TIER_PROJECT &&
            strset_has(&global_hashes, d->content_hash);

        if (json) {
            if (i)
                sb_putc(&sb, ',');
            sb_printf(&sb,
                      "{\"chunk\":\"C-%lld\",\"document\":\"%s\","
                      "\"source\":\"%s\",\"title\":",
                      (long long)chunk_num, d->id, d->source);
            json_escape_c(&sb, d->title ? d->title : "");
            sb_puts(&sb, ",\"heading\":");
            if (heading)
                json_escape_c(&sb, heading);
            else
                sb_puts(&sb, "null");
            sb_puts(&sb, ",\"snippet\":");
            json_escape_c(&sb, snip);
            sb_puts(&sb, ",\"collection\":");
            json_escape_c(&sb, src ? src->collection : "");
            sb_printf(&sb, ",\"store\":\"%s\",\"alsoGlobal\":%s,\"matched\":",
                      tier_name(t->s.tier), also_global ? "true" : "false");
            put_matched(&sb, fused[i].tags);
            /* Only the scores that were actually computed. §4's `vector` and
             * `fused` are absent rather than zero, because a caller must be
             * able to tell "the vector path found nothing" from "the vector
             * path did not run". */
            sb_printf(&sb, ",\"scores\":{\"bm25\":%.6f}", c->bm25);
            sb_printf(&sb, ",\"fetchedAt\":\"%s\"",
                      d->fetched_at ? d->fetched_at : "");
            if (expand) {
                sb_puts(&sb, ",\"neighbours\":[");
                bool first = true;
                for (int64_t o = (int64_t)fc->ordinal - expand;
                     o <= (int64_t)fc->ordinal + expand; o++) {
                    if (o == (int64_t)fc->ordinal || o < 0 || !dt->ok ||
                        o >= (int64_t)dt->ch.n)
                        continue;
                    const Chunk *nc = &dt->ch.v[o];
                    if (!first)
                        sb_putc(&sb, ',');
                    first = false;
                    sb_printf(&sb, "{\"chunk\":\"C-%lld\",\"heading\":",
                              (long long)(d->chunk_base + o));
                    if (nc->heading)
                        json_escape_c(&sb, nc->heading);
                    else
                        sb_puts(&sb, "null");
                    sb_puts(&sb, ",\"snippet\":");
                    json_escape_c(&sb, snippet_of(a, dt->text, nc->start,
                                                  nc->end, &q));
                    sb_putc(&sb, '}');
                }
                sb_putc(&sb, ']');
            }
            sb_putc(&sb, '}');
        } else {
            sb_printf(&sb, "C-%-8lld %-7s %-8s %9.4f  ", (long long)chunk_num,
                      d->id, tier_name(t->s.tier), c->bm25);
            sb_puts_safe(&sb, src ? src->collection : "-");
            sb_puts(&sb, "  ");
            sb_puts_safe(&sb, d->title ? d->title : "");
            if (heading) {
                sb_puts(&sb, " \xc2\xbb ");
                sb_puts_safe(&sb, heading);
            }
            if (also_global)
                sb_puts(&sb, "  (also global)");
            sb_puts(&sb, "\n    ");
            sb_puts_safe(&sb, snip);
            sb_putc(&sb, '\n');
            for (int64_t o = (int64_t)fc->ordinal - expand;
                 o <= (int64_t)fc->ordinal + expand; o++) {
                if (o == (int64_t)fc->ordinal || o < 0 || !dt->ok ||
                    o >= (int64_t)dt->ch.n)
                    continue;
                const Chunk *nc = &dt->ch.v[o];
                sb_printf(&sb, "    ~ C-%lld ", (long long)(d->chunk_base + o));
                sb_puts_safe(&sb, snippet_of(a, dt->text, nc->start, nc->end,
                                             &q));
                sb_putc(&sb, '\n');
            }
        }
    }
    if (json) {
        sb_printf(&sb, "],\"count\":%zu}", nfused);
        puts(sb_finish(&sb));
    } else if (nfused == 0) {
        puts("no hits");
    } else {
        fputs(sb_finish(&sb), stdout);
    }
    for (size_t t = 0; t < tiers.n; t++)
        store_close(&ts[t].s);
    return KB_EXIT_OK;
}
