#include "cmd.h"
#include "modelrec.h"

#include "rank.h"
#include "snippet.h"

/* GET /search (§4).
 *
 * WHAT THIS BUILD CAN ANSWER. §4's default is hybrid, and hybrid needs stored
 * vectors, which this build does not write yet. Rather than pretend, the
 * default here is `keyword` and every response says `"mode":"keyword"`, so
 * a caller can see what actually ran instead of inferring it. Asking for
 * `hybrid` or `semantic` explicitly is refused — `model_missing`,
 * `model_mismatch` or `index_stale` on the vectors, whichever is true (see
 * semantic_ready): a search that silently answered a hybrid question with
 * half a hybrid answer would be worse than one that refused, because the
 * caller would go on believing the vector side had been consulted.
 *
 * SNIPPETS ONLY. §4: "a list must not be able to flood a caller's context."
 * Every passage returned here is a window of at most KB_SNIPPET_BYTES,
 * chosen for where the query terms actually are. `kb chunk` is the route to
 * a whole chunk and `kb get --include text` to a whole document, and both
 * are single-item requests by construction.
 *
 * FUSED BY RANK. The keyword list goes through reciprocal rank fusion
 * (rank.h) even though it is the only list today, so that the semantic list
 * joins it without a second merge path when the model arrives.
 */

static const char *const VALUE_FLAGS[] = {
    "--collection", "--mode",       "--k",         "--expand",
    "--source",     "--mime",       "--since",     "--min-score",
    "--minScore",   "--older-than", "--olderThan", "--meta",
    NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

typedef enum { MODE_KEYWORD, MODE_HYBRID, MODE_SEMANTIC } SearchMode;

/* ---- the model -------------------------------------------------------- */

/* Whether a search that needs vectors can run, and the refusal when it cannot.
 * Degrading to keyword and saying nothing would hand back an answer the caller
 * reads as hybrid, so each way of not being ready is its own §11 error:
 * no model on this machine, a model other than the one the store recorded
 * (§8: vectors from two models are not comparable), or no vectors at all. */
static bool semantic_ready(Arena *a, Store *s, bool json, const char *mode) {
    ModelProbe probe;
    model_probe(a, &probe);
    if (!probe.found) {
        errdet_begin("model_missing");
        errdet_str("path", probe.dir);
        err_out(json, "model_missing", "mode \"%s\" needs the embedding model: %s",
                mode, probe.err);
        return false;
    }
    ModelParams recorded;
    char sha[65];
    if (model_recorded(a, s, &recorded, sha) &&
        !model_params_equal(&recorded, &probe.params)) {
        StrBuf stored, loaded;
        sb_init(&stored, a);
        sb_init(&loaded, a);
        sb_putc(&stored, '{');
        model_params_json(&stored, &recorded);
        sb_putc(&stored, '}');
        sb_putc(&loaded, '{');
        model_params_json(&loaded, &probe.params);
        sb_putc(&loaded, '}');
        errdet_begin("model_mismatch");
        errdet_raw("stored", sb_finish(&stored));
        errdet_raw("loaded", sb_finish(&loaded));
        char why[256];
        model_params_diff(&recorded, &probe.params, why, sizeof why);
        err_out(json, "model_mismatch",
                "the store was indexed with another model configuration (%s); "
                "run \"kb reindex\" to rebuild it under %s",
                why, probe.path);
        return false;
    }
    static const char *const structures[] = {"vectors"};
    errdet_begin("index_stale");
    errdet_strs("structures", structures, 1);
    err_out(json, "index_stale",
            "mode \"%s\" needs stored vectors and this store has none yet; "
            "--mode keyword answers from the keyword index",
            mode);
    return false;
}

/* ---- filters ----------------------------------------------------------- */

/* The scope is `ls`'s (docquery_keep), asked per document as the index
 * scores it. */
typedef struct {
    Arena *a;
    Store *s;
    DocQuery q;
} DocFilter;

static bool doc_keep(uint32_t doc_index, void *ud) {
    const DocFilter *f = (const DocFilter *)ud;
    if (doc_index >= f->s->documents.n)
        return false;
    const Document *d = &f->s->documents.v[doc_index];
    return docquery_keep(f->a, &f->q, d, src_by_id(&f->s->sources, d->source));
}

typedef struct {
    uint32_t chunk_index;
    double bm25;
} Cand;

/* Blob + chunk boundaries for one document, kept for the handful of hits
 * being rendered so several hits in the same document read it once. */
typedef struct {
    uint32_t doc_index;
    char *text;
    size_t len;
    Chunks ch;
    bool ok;
} DocText;

static DocText *doc_text(Arena *a, Store *s, uint32_t doc_index,
                         DocText *cache, size_t *ncache) {
    for (size_t i = 0; i < *ncache; i++) {
        if (cache[i].doc_index == doc_index)
            return &cache[i];
    }
    DocText *e = &cache[(*ncache)++];
    memset(e, 0, sizeof(*e));
    e->doc_index = doc_index;
    const Document *d = &s->documents.v[doc_index];
    e->ok = doc_chunks(a, s, d, &e->text, &e->len, &e->ch);
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
    /* A floor on the keyword (BM25) score, applied before fusion, so a weak
     * match never takes a rank. Not a floor on `fused`: that is
     * 1/(k + rank), which says where a hit landed, not how well it matched —
     * the best hit of a poor query scores the same as the best hit of a good
     * one, and a floor on it would only be a second `k`. */
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
    char err[512];
    DocFilter f;
    memset(&f, 0, sizeof f);
    f.a = a;
    if (!docquery_parse(a, argc, argv, VALUE_FLAGS, &f.q, err, sizeof err)) {
        err_out(json, "usage", "%s", err);
        return KB_EXIT_ERR;
    }
    Staleness st;
    if (!staleness_init(&st, older_than_arg(argc, argv, VALUE_FLAGS), err,
                        sizeof err)) {
        err_out(json, "usage", "%s", err);
        return KB_EXIT_ERR;
    }
    char dir[KB_PATH_MAX];
    if (!store_resolve(dir, sizeof dir, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return KB_EXIT_ERR;
    }

    Store s;
    FtsIndex ix;
    const char *code;
    if (!store_open(a, &s, dir, false, err, sizeof err, &code)) {
        err_out(json, code, "%s", err);
        return KB_EXIT_ERR;
    }
    if (!fts_open_store(a, &s, &ix, &code, err, sizeof err)) {
        store_close(&s);
        err_out(json, code, "%s", err);
        return KB_EXIT_ERR;
    }

    if (mode != MODE_KEYWORD && !semantic_ready(a, &s, json, mode_s)) {
        store_close(&s);
        return KB_EXIT_ERR;
    }

    TermList q = token_terms(a, query, strlen(query));

    f.s = &s;

    FtsHit *hits = NULL;
    size_t n =
        fts_search(a, &ix, &q, min_score, doc_keep, &f, (size_t)k, &hits);
    Cand *cands = (Cand *)arena_alloc(a, (n ? n : 1) * sizeof(Cand));
    RankEntry *entries =
        (RankEntry *)arena_alloc(a, (n ? n : 1) * sizeof(RankEntry));
    for (size_t i = 0; i < n; i++) {
        cands[i].chunk_index = hits[i].chunk_index;
        cands[i].bm25 = hits[i].score;
        entries[i].key = (uint64_t)hits[i].chunk_index;
        entries[i].item = &cands[i];
    }
    RankList list;
    list.v = entries;
    list.n = n;
    list.tag = RANK_TAG_KEYWORD;

    RankResult *fused = NULL;
    size_t nfused = rrf_fuse(a, &list, 1, KB_RRF_K, (size_t)k, &fused);

    DocText *cache =
        (DocText *)arena_alloc0(a, ((size_t)k + 1) * sizeof(DocText));
    size_t ncache = 0;

    StrBuf sb;
    sb_init(&sb, a);
    if (json)
        sb_puts(&sb, "{\"ok\":true,\"mode\":\"keyword\",\"hits\":[");
    for (size_t i = 0; i < nfused; i++) {
        Cand *c = (Cand *)fused[i].item;
        const FtsChunk *fc = &ix.chunks[c->chunk_index];
        const Document *d = &s.documents.v[fc->doc_index];
        const Source *src = src_by_id(&s.sources, d->source);
        DocText *dt = doc_text(a, &s, fc->doc_index, cache, &ncache);
        const char *heading = NULL;
        const char *snip = "";
        if (dt->ok && fc->ordinal < dt->ch.n) {
            const Chunk *ch = &dt->ch.v[fc->ordinal];
            heading = ch->heading;
            snip = snippet_of(a, dt->text, ch->start, ch->end, &q);
        }
        int64_t chunk_num = d->chunk_base + (int64_t)fc->ordinal;

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
            sb_puts(&sb, ",\"matched\":");
            put_matched(&sb, fused[i].tags);
            /* The scores that were computed. `fused` is the reciprocal-rank
             * sum the list is ordered by — fusion runs even over the one
             * keyword list. `vector` is absent rather than zero, because a
             * caller must be able to tell "the vector path found nothing"
             * from "the vector path did not run". */
            sb_printf(&sb, ",\"scores\":{\"bm25\":%.6f,\"fused\":%.6f}",
                      c->bm25, fused[i].score);
            /* §5: every hit carries how old it is AND the verdict on that
             * age, from the one definition in cmd_common.c. */
            sb_printf(&sb, ",\"fetchedAt\":\"%s\",\"stale\":%s",
                      d->fetched_at ? d->fetched_at : "",
                      doc_stale(&st, d) ? "true" : "false");
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
            sb_printf(&sb, "C-%-8lld %-7s %9.4f  ", (long long)chunk_num,
                      d->id, c->bm25);
            sb_puts_safe(&sb, src ? src->collection : "-");
            sb_puts(&sb, "  ");
            sb_puts_safe(&sb, d->title ? d->title : "");
            if (heading) {
                sb_puts(&sb, " \xc2\xbb ");
                sb_puts_safe(&sb, heading);
            }
            if (doc_stale(&st, d))
                sb_puts(&sb, "  (stale)");
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
        /* The threshold that produced every `stale` above, so a caller never
         * has to know which default it got (§5). The cutoff INSTANT is
         * deliberately not here: it is a reading of the clock rather than a
         * fact about the corpus, and a search answer has to be a function of
         * the store and the query alone — that is what makes "rebuild
         * reproduces the previous answer" a thing anyone can check. `kb
         * stale` and `kb refresh` do carry it, because there the cutoff is
         * the question rather than a footnote to it. */
        sb_printf(&sb, "],\"count\":%zu,\"olderThan\":\"%s\"}", nfused,
                  st.spec);
        puts(sb_finish(&sb));
    } else if (nfused == 0) {
        puts("no hits");
    } else {
        fputs(sb_finish(&sb), stdout);
    }
    store_close(&s);
    return KB_EXIT_OK;
}
