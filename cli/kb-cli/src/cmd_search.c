#include "cmd.h"
#include "modelrec.h"
#include "vectors.h"

#include "rank.h"
#include "snippet.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

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
    "--fusion",     "--rerank-depth", "--rerank-tokens", NULL};
static const char *const BOOL_FLAGS[] = {"--json", "--rerank", NULL};

typedef enum { MODE_KEYWORD, MODE_HYBRID, MODE_SEMANTIC } SearchMode;
static const char *const MODE_NAMES[] = {"keyword", "hybrid", "semantic"};

/* How deep each list goes before fusion. Rank fusion needs more than the k
 * that will be shown: a chunk at rank 30 of both lists can outrank one at
 * rank 1 of only one. */
#define FUSE_DEPTH(k) ((size_t)((k) * 3 < 50 ? 50 : (k) * 3))

/* ---- the model -------------------------------------------------------- */

/* Opens what a vector search needs — the model and the stored vectors — or
 * says why it cannot. Degrading to keyword and saying nothing would hand back
 * an answer the caller reads as hybrid, so each way of not being ready is its
 * own §11 error: no model on this machine, a model other than the one the
 * store recorded (§8: vectors from two models are not comparable), or vectors
 * that do not cover the store. With `report` false it only answers whether,
 * which is how the default mode is chosen. */
static bool semantic_open(Arena *a, Store *s, bool json, const char *mode,
                          bool report, Embedder *e, VecSet *v, size_t *unembedded) {
    *unembedded = 0;
    ModelProbe probe;
    model_probe(a, &probe);
    if (!probe.found) {
        if (!report)
            return false;
        errdet_begin("model_missing");
        errdet_str("path", probe.dir);
        err_out(json, "model_missing", "mode \"%s\" needs the embedding model: %s",
                mode, probe.err);
        return false;
    }
    ModelParams recorded;
    char sha[65];
    bool has_recorded = model_recorded(a, s, &recorded, sha);
    if (has_recorded && !model_params_equal(&recorded, &probe.params)) {
        if (!report)
            return false;
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
    char fp[65];
    vec_fingerprint(&probe.params, fp);
    vec_load(a, s, v);
    /* A file written under another model covers nothing. */
    if (strcmp(v->fingerprint, fp) != 0)
        v->n = 0;
    size_t missing = vec_missing(s, v);
    /* Vectors under this model for some chunks and not yet for others (an
     * add that ran out of its embedding budget): the search runs over the
     * ones there are and says how many are missing, rather than refusing or
     * pretending the gap is not there. */
    if (has_recorded && strcmp(v->fingerprint, fp) == 0 && missing > 0) {
        *unembedded = missing;
        missing = 0;
    }
    if (!has_recorded || strcmp(v->fingerprint, fp) != 0 || missing > 0) {
        if (!report)
            return false;
        static const char *const structures[] = {"vectors"};
        errdet_begin("index_stale");
        errdet_strs("structures", structures, 1);
        if (missing)
            errdet_int("missing", (int64_t)missing);
        if (!has_recorded)
            err_out(json, "index_stale",
                    "mode \"%s\" needs vectors and this store records no "
                    "model; \"kb reindex\" records it and embeds every chunk",
                    mode);
        else
            err_out(json, "index_stale",
                    "mode \"%s\" needs vectors for every chunk and %zu have "
                    "none; \"kb rebuild\" embeds them",
                    mode, missing);
        return false;
    }
    char why[512];
    if (!embed_open(a, probe.path, e, why, sizeof why)) {
        if (report)
            err_out(json, "model_missing", "%s", why);
        return false;
    }
    return true;
}

/* The vector list: every stored vector the filter keeps, by similarity to the
 * query, best first, at most `depth`. Chunk ids are mapped back to their
 * documents through the log, so a vector for a chunk no longer in the store
 * is never returned. */
typedef struct {
    int64_t id;
    uint32_t doc_index, ordinal;
    float score;
} VecHit;

static int vechit_cmp(const void *pa, const void *pb) {
    const VecHit *x = (const VecHit *)pa, *y = (const VecHit *)pb;
    if (x->score != y->score)
        return x->score > y->score ? -1 : 1;
    return x->id < y->id ? -1 : (x->id > y->id);
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

/* One chunk either list found, with whatever each list said about it. */
typedef struct {
    int64_t id;
    uint32_t doc_index, ordinal;
    double bm25;
    float vector;
    bool has_bm25, has_vector;
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

/* Whether a query is one word that looks like a name in code: an underscore,
 * an inner capital, a digit, `::`, `->`, a dot or `()` in it, or a leading
 * dash (a flag). Such a query is looking for that exact name, so keyword
 * search, which matches it exactly, is weighted up. */
static bool identifier_shaped(const char *q) {
    while (*q == ' ')
        q++;
    size_t n = strlen(q);
    while (n && q[n - 1] == ' ')
        n--;
    if (n == 0 || memchr(q, ' ', n))
        return false;
    if (q[0] == '-')
        return true;
    for (size_t i = 0; i < n; i++) {
        char c = q[i];
        if (c == '_' || c == '.' || (c >= '0' && c <= '9'))
            return true;
        if (i > 0 && c >= 'A' && c <= 'Z' && q[i - 1] >= 'a' && q[i - 1] <= 'z')
            return true;
        if (i + 1 < n && ((c == ':' && q[i + 1] == ':') || (c == '-' && q[i + 1] == '>') ||
                          (c == '(' && q[i + 1] == ')')))
            return true;
    }
    return false;
}

/* A semantic first answer far enough ahead of its second — by KB_FUSION_LEAD
 * in cosine — is one the model is sure of, and keeps rank 1 whatever the
 * keyword list thinks. */
static void keep_confident_first(const RankList *lists, size_t nlists, RankResult *fused,
                                 size_t nfused) {
    for (size_t i = 0; i < nlists; i++) {
        if (lists[i].tag != RANK_TAG_SEMANTIC || lists[i].n < 2)
            continue;
        if (lists[i].v[0].score - lists[i].v[1].score < KB_FUSION_LEAD)
            return;
        const uint64_t key = lists[i].v[0].key;
        for (size_t j = 1; j < nfused; j++) {
            if (fused[j].key != key)
                continue;
            RankResult winner = fused[j];
            memmove(&fused[1], &fused[0], j * sizeof(RankResult));
            fused[0] = winner;
            return;
        }
    }
}

/* --rerank: the cross-encoder rescores the first KB_RERANK_DEPTH fused
 * candidates, each read as its header line and text (what it was embedded
 * as), and they are put in the order of its scores; those below the depth
 * keep the fused order after them. *rr[i] is the score of the i-th hit after
 * reordering, for the first *nrr hits. */
typedef struct {
    RankResult r;
    float score;
} Rescored;

static int rescored_cmp(const void *pa, const void *pb) {
    const Rescored *x = (const Rescored *)pa, *y = (const Rescored *)pb;
    if (x->score != y->score)
        return x->score > y->score ? -1 : 1;
    return x->r.key < y->r.key ? -1 : x->r.key > y->r.key;
}

static bool rerank_fused(Arena *a, Store *s, bool json, const char *query, RankResult *fused,
                         size_t nfused, size_t want, size_t tokens, DocText *cache,
                         size_t *ncache, float **rr, size_t *nrr) {
    char path[KB_PATH_MAX], err[512];
    Embedder re;
    if (!embed_find_reranker(a, path, sizeof path, err, sizeof err)) {
        errdet_begin("model_missing");
        errdet_str("path", "~/.kb/models");
        err_out(json, "model_missing", "--rerank needs the reranker: %s", err);
        return false;
    }
    if (!rerank_open(a, path, &re, err, sizeof err)) {
        err_out(json, "model_missing", "--rerank needs the reranker: %s", err);
        return false;
    }
    /* Twenty passages: int8 layer products repay their conversion. */
    if (!embed_quantize(&re)) {
        embed_close(&re);
        err_out(json, "internal", "the reranker's layers could not be converted to int8");
        return false;
    }
    size_t depth = nfused < want ? nfused : want;
    Rescored *v = (Rescored *)arena_alloc(a, depth * sizeof(Rescored));
    for (size_t i = 0; i < depth; i++) {
        Cand *c = (Cand *)fused[i].item;
        const Document *d = &s->documents.v[c->doc_index];
        DocText *dt = doc_text(a, s, c->doc_index, cache, ncache);
        v[i].r = fused[i];
        v[i].score = -INFINITY;
        if (!dt->ok || c->ordinal >= dt->ch.n)
            continue;
        const Chunk *ch = &dt->ch.v[c->ordinal];
        const char *body = dt->text + ch->start;
        size_t blen = ch->end - ch->start;
        const char *header = chunk_header(a, doc_lang(d->mime, d->path), d->title, ch);
        if (header) {
            body = arena_printf(a, "%s\n%.*s", header, (int)blen, body);
            blen = strlen(body);
        }
        if (!rerank_score(&re, query, strlen(query), body, blen, tokens,
                          &v[i].score)) {
            embed_close(&re);
            err_out(json, "internal", "reranking C-%lld failed",
                    (long long)(d->chunk_base + (int64_t)c->ordinal));
            return false;
        }
    }
    embed_close(&re);
    qsort(v, depth, sizeof(Rescored), rescored_cmp);
    *rr = (float *)arena_alloc(a, depth * sizeof(float));
    for (size_t i = 0; i < depth; i++) {
        fused[i] = v[i].r;
        (*rr)[i] = v[i].score;
    }
    *nrr = depth;
    return true;
}

/* KB_TIMING=1 in the environment: where a search's time went, one line per
 * phase on stderr, for the performance benchmark (bench/perf.py). */
static double timing_t0, timing_last;
static bool timing_on;

static double timing_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void mark(const char *phase) {
    if (!timing_on)
        return;
    double now = timing_ms();
    fprintf(stderr, "kb timing %-16s %8.2f ms\n", phase, now - timing_last);
    timing_last = now;
}

int32_t cmd_search(Arena *a, int32_t argc, char **argv) {
    timing_on = getenv("KB_TIMING") != NULL;
    timing_t0 = timing_last = timing_ms();
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

    /* §4's default is hybrid. A store without vectors — no model, or one
     * not embedded yet — answers keyword instead, and the response's `mode`
     * says which ran; asking for hybrid by name is refused with the reason. */
    SearchMode mode = MODE_HYBRID;
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
    mark("open store");
    Embedder emb;
    VecSet vs;
    size_t unembedded = 0;
    if (mode == MODE_HYBRID && !mode_s) {
        if (!semantic_open(a, &s, json, "hybrid", false, &emb, &vs, &unembedded))
            mode = MODE_KEYWORD;
    } else if (mode != MODE_KEYWORD &&
               !semantic_open(a, &s, json, mode_s, true, &emb, &vs, &unembedded)) {
        store_close(&s);
        return KB_EXIT_ERR;
    }
    mark("open model");
    if (mode != MODE_SEMANTIC &&
        !fts_open_store(a, &s, &ix, &code, err, sizeof err)) {
        if (mode != MODE_KEYWORD)
            embed_close(&emb);
        store_close(&s);
        err_out(json, code, "%s", err);
        return KB_EXIT_ERR;
    }

    mark("load index");
    TermList q = token_terms(a, query, strlen(query));
    f.s = &s;
    const size_t depth = FUSE_DEPTH(k);
    RankList lists[2];
    size_t nlists = 0;

    /* The keyword list. min_score is a floor on BM25, applied here, before
     * fusion (§4). */
    size_t n = 0;
    Cand *cands = NULL;
    if (mode != MODE_SEMANTIC) {
        FtsHit *hits = NULL;
        n = fts_search(a, &ix, &q, min_score, doc_keep, &f,
                       mode == MODE_KEYWORD ? (size_t)k : depth, &hits);
        cands = (Cand *)arena_alloc0(a, (n ? n : 1) * sizeof(Cand));
        RankEntry *entries =
            (RankEntry *)arena_alloc(a, (n ? n : 1) * sizeof(RankEntry));
        for (size_t i = 0; i < n; i++) {
            const FtsChunk *fc = &ix.chunks[hits[i].chunk_index];
            Cand *c = &cands[i];
            c->doc_index = fc->doc_index;
            c->ordinal = fc->ordinal;
            c->id = s.documents.v[fc->doc_index].chunk_base + fc->ordinal;
            c->bm25 = hits[i].score;
            c->has_bm25 = true;
            entries[i].key = (uint64_t)c->id;
            entries[i].item = c;
            entries[i].score = c->bm25;
        }
        lists[nlists].v = entries;
        lists[nlists].n = n;
        lists[nlists].tag = RANK_TAG_KEYWORD;
        nlists++;
    }

    mark("keyword search");

    /* The vector list: the query embedded with the query prefix (§8), scored
     * against every stored vector the filter keeps. A chunk the keyword list
     * already holds is the same candidate, so its row carries both scores. */
    if (mode != MODE_KEYWORD) {
        float *qv = (float *)arena_alloc(a, emb.n_embd * sizeof(float));
        bool truncated;
        bool ok = embed_text(&emb, query, strlen(query), true, qv, &truncated);
        embed_close(&emb);
        mark("embed query");
        if (!ok) {
            store_close(&s);
            err_out(json, "internal", "the query could not be embedded");
            return KB_EXIT_FATAL;
        }
        VecHit *vh = (VecHit *)arena_alloc(a, (vs.n ? vs.n : 1) * sizeof(VecHit));
        size_t nv = 0;
        for (size_t di = 0; di < s.documents.n; di++) {
            const Document *d = &s.documents.v[di];
            if (d->chunk_count == 0 || !doc_keep((uint32_t)di, &f))
                continue;
            for (uint32_t j = 0; j < d->chunk_count; j++) {
                int64_t at = vec_find(&vs, d->chunk_base + (int64_t)j);
                /* Scale 0: a chunk indexed for keywords only. */
                if (at < 0 || vs.scales[at] == 0.0f)
                    continue;
                vh[nv].id = d->chunk_base + (int64_t)j;
                vh[nv].doc_index = (uint32_t)di;
                vh[nv].ordinal = j;
                vh[nv].score = vec_score(&vs, (size_t)at, qv);
                nv++;
            }
        }
        qsort(vh, nv, sizeof(VecHit), vechit_cmp);
        mark("vector scan");
        size_t take = nv < (mode == MODE_SEMANTIC ? (size_t)k : depth)
                          ? nv
                          : (mode == MODE_SEMANTIC ? (size_t)k : depth);
        Cand *vc = (Cand *)arena_alloc0(a, (take ? take : 1) * sizeof(Cand));
        RankEntry *entries =
            (RankEntry *)arena_alloc(a, (take ? take : 1) * sizeof(RankEntry));
        for (size_t i = 0; i < take; i++) {
            Cand *c = NULL;
            for (size_t j = 0; j < n && !c; j++)
                if (cands[j].id == vh[i].id)
                    c = &cands[j];
            if (!c) {
                c = &vc[i];
                c->id = vh[i].id;
                c->doc_index = vh[i].doc_index;
                c->ordinal = vh[i].ordinal;
            }
            c->vector = vh[i].score;
            c->has_vector = true;
            entries[i].key = (uint64_t)vh[i].id;
            entries[i].item = c;
            entries[i].score = vh[i].score;
        }
        lists[nlists].v = entries;
        lists[nlists].n = take;
        lists[nlists].tag = RANK_TAG_SEMANTIC;
        nlists++;
    }

    /* With --rerank the fused list goes at least KB_RERANK_DEPTH deep, so
     * the cross-encoder has candidates to promote from below the cut. */
    const bool rerank = has_flag(argc, argv, VALUE_FLAGS, "--rerank");
    /* How many candidates the reranker reads, and how many tokens of each
     * pair: the two knobs its latency turns on (bench/bench.py measures). */
    size_t rr_depth = KB_RERANK_DEPTH, rr_tokens = KB_RERANK_TOKENS;
    const char *rd = flag_value(argc, argv, VALUE_FLAGS, "--rerank-depth");
    const char *rt = flag_value(argc, argv, VALUE_FLAGS, "--rerank-tokens");
    if (rd || rt) {
        char *e1 = NULL, *e2 = NULL;
        long d = rd ? strtol(rd, &e1, 10) : (long)rr_depth;
        long tk = rt ? strtol(rt, &e2, 10) : (long)rr_tokens;
        if ((rd && (*e1 || d < 1 || d > 100)) || (rt && (*e2 || tk < 32 || tk > 1024)) ||
            !rerank) {
            store_close(&s);
            err_out(json, "usage", rerank ? "--rerank-depth is 1 to 100 and --rerank-tokens "
                                            "32 to 1024"
                                          : "--rerank-depth and --rerank-tokens go with --rerank");
            return KB_EXIT_ERR;
        }
        rr_depth = (size_t)d;
        rr_tokens = (size_t)tk;
    }
    size_t fuse_k = (size_t)k;
    if (rerank && fuse_k < rr_depth)
        fuse_k = rr_depth;
    RankResult *fused = NULL;
    size_t nfused;
    const char *fusion = flag_value(argc, argv, VALUE_FLAGS, "--fusion");
    if (fusion && strcmp(fusion, "rrf") != 0 && strcmp(fusion, "score") != 0) {
        store_close(&s);
        err_out(json, "usage", "--fusion is rrf or score, not \"%s\"", fusion);
        return KB_EXIT_ERR;
    }
    if (fusion && strcmp(fusion, "rrf") == 0) {
        nfused = rrf_fuse(a, lists, nlists, KB_RRF_K, fuse_k, &fused);
    } else {
        const double sem = identifier_shaped(query) ? KB_FUSION_SEMANTIC_IDENT
                                                    : KB_FUSION_SEMANTIC;
        double weight[2];
        for (size_t i = 0; i < nlists; i++)
            weight[i] = lists[i].tag == RANK_TAG_SEMANTIC ? sem : 1.0 - sem;
        /* A list alone is its own order, whatever its weight. */
        if (nlists == 1)
            weight[0] = 1.0;
        nfused = score_fuse(a, lists, nlists, weight, fuse_k, &fused);
        keep_confident_first(lists, nlists, fused, nfused);
    }

    DocText *cache =
        (DocText *)arena_alloc0(a, (fuse_k + 1) * sizeof(DocText));
    size_t ncache = 0;

    float *rr = NULL;
    size_t nrr = 0;
    if (rerank && nfused) {
        if (!rerank_fused(a, &s, json, query, fused, nfused, rr_depth, rr_tokens, cache,
                          &ncache, &rr, &nrr)) {
            store_close(&s);
            return KB_EXIT_ERR;
        }
    }
    if (nfused > (size_t)k)
        nfused = (size_t)k;

    StrBuf sb;
    sb_init(&sb, a);
    if (json)
        sb_printf(&sb, "{\"ok\":true,\"mode\":\"%s\",\"hits\":[",
                  MODE_NAMES[mode]);
    for (size_t i = 0; i < nfused; i++) {
        Cand *c = (Cand *)fused[i].item;
        const Document *d = &s.documents.v[c->doc_index];
        const Source *src = src_by_id(&s.sources, d->source);
        DocText *dt = doc_text(a, &s, c->doc_index, cache, &ncache);
        const char *heading = NULL;
        const char *snip = "";
        if (dt->ok && c->ordinal < dt->ch.n) {
            const Chunk *ch = &dt->ch.v[c->ordinal];
            heading = ch->heading;
            snip = snippet_of(a, dt->text, ch->start, ch->end, &q);
        }
        int64_t chunk_num = d->chunk_base + (int64_t)c->ordinal;

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
             * sum the list is ordered by. `bm25` and `vector` are each
             * present only when that path found the chunk, rather than zero,
             * because a caller must be able to tell "this path found nothing
             * here" from "this path did not run". */
            sb_puts(&sb, ",\"scores\":{");
            if (c->has_bm25)
                sb_printf(&sb, "\"bm25\":%.6f,", c->bm25);
            if (c->has_vector)
                sb_printf(&sb, "\"vector\":%.6f,", (double)c->vector);
            if (i < nrr)
                sb_printf(&sb, "\"rerank\":%.6f,", (double)rr[i]);
            sb_printf(&sb, "\"fused\":%.6f}", fused[i].score);
            /* §5: every hit carries how old it is AND the verdict on that
             * age, from the one definition in cmd_common.c. */
            sb_printf(&sb, ",\"fetchedAt\":\"%s\",\"stale\":%s",
                      d->fetched_at ? d->fetched_at : "",
                      doc_stale(&st, d) ? "true" : "false");
            if (expand) {
                sb_puts(&sb, ",\"neighbours\":[");
                bool first = true;
                for (int64_t o = (int64_t)c->ordinal - expand;
                     o <= (int64_t)c->ordinal + expand; o++) {
                    if (o == (int64_t)c->ordinal || o < 0 || !dt->ok ||
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
            /* Which paths found it, where §4's JSON says `matched`. */
            const uint32_t tags = fused[i].tags;
            sb_printf(&sb, "C-%-8lld %-7s %-7s ", (long long)chunk_num, d->id,
                      tags == (RANK_TAG_KEYWORD | RANK_TAG_SEMANTIC) ? "kw+sem"
                      : tags == RANK_TAG_SEMANTIC                     ? "sem"
                                                                      : "kw");
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
            for (int64_t o = (int64_t)c->ordinal - expand;
                 o <= (int64_t)c->ordinal + expand; o++) {
                if (o == (int64_t)c->ordinal || o < 0 || !dt->ok ||
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
        if (unembedded)
            sb_printf(&sb, "],\"unembedded\":%zu,\"count\":%zu,\"olderThan\":\"%s\"}",
                      unembedded, nfused, st.spec);
        else
            sb_printf(&sb, "],\"count\":%zu,\"olderThan\":\"%s\"}", nfused,
                      st.spec);
        puts(sb_finish(&sb));
    } else if (nfused == 0) {
        puts("no hits");
        if (unembedded)
            printf("(%zu chunks are not embedded yet; kb embed finishes them)\n", unembedded);
    } else {
        if (unembedded)
            sb_printf(&sb, "(%zu chunks are not embedded yet: the semantic side of this "
                           "search did not see them; kb embed finishes them)\n",
                      unembedded);
        fputs(sb_finish(&sb), stdout);
    }
    mark("fuse and render");
    if (timing_on)
        fprintf(stderr, "kb timing %-16s %8.2f ms\n", "total", timing_ms() - timing_t0);
    store_close(&s);
    return KB_EXIT_OK;
}
