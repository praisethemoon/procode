/* kb's ModernBERT forward pass against the reference embeddings in
 * tests/fixtures/modernbert/reference.json (tools/modernbert/reference.py,
 * float32 on CPU). Needs the model file: KB_TEST_MODERNBERT names it (the
 * Makefile uses ~/.kb/models' copy, read-only), and the test is skipped
 * without it. Cosine similarity must be at least 0.999 for every text.
 *
 * The reference embeddings are sentence-transformers', which strips each
 * text first; the fixture's `tokens` are of the unstripped text (they test
 * the tokenizer). kb strips too, so its vectors are compared as they are. */

#include "test.h"

#include "../../src/embed.h"
#include "../../src/json.h"
#include "../../src/platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double cosine(const float *a, const JVal *b, uint32_t n) {
    double dot = 0, na = 0, nb = 0;
    for (uint32_t i = 0; i < n; i++) {
        double x = a[i], y = b->arr.items[i]->num;
        dot += x * y;
        na += x * x;
        nb += y * y;
    }
    return dot / (sqrt(na) * sqrt(nb));
}

/* The reranker against the reference logits (sentence-transformers'
 * CrossEncoder, float32 weights). kb runs the F16 file, whose weights are
 * rounded, and the logit is an unnormalised sum over 768 values: measured, it
 * lands within 1.3e-3 of the reference (1e-5 for most pairs), so the test
 * allows 2e-3. What ranking depends on is exact: the pairs sort in the
 * reference's order. Needs KB_TEST_RERANKER. */
static void test_reranker(void) {
    const char *model = getenv("KB_TEST_RERANKER");
    if (!model || !*model || !plat_is_file(model)) {
        t_begin("reranker: skipped (KB_TEST_RERANKER names no model file)");
        return;
    }
    Arena *a = arena_new(1 << 24);
    char err[512];
    Embedder e;
    t_begin("reranker: the file opens as a reranker and not as an embedder");
    ASSERT_TRUE(rerank_open(a, model, &e, err, sizeof err));
    ASSERT_TRUE(e.reranker && e.pool_mean);
    Embedder not_an_embedder;
    ASSERT_TRUE(!embed_open(a, model, &not_an_embedder, err, sizeof err));
    ASSERT_TRUE(strstr(err, "reranker") != NULL);

    char *data;
    size_t len;
    ASSERT_TRUE(plat_read_file(a, "tests/fixtures/modernbert/reference.json", &data, &len));
    JVal *ref = json_parse(a, data, len, err, sizeof err);
    JVal *pairs = jobj_get(ref, "pairs"), *logits = jobj_get(ref, "reranker_logits");
    ASSERT_TRUE(pairs && logits && pairs->arr.n == logits->arr.n);

    t_begin("reranker: every fixture pair scores the reference logit (within 2e-3), in its order");
    double worst = 0;
    float scores[16];
    for (size_t k = 0; pairs && k < pairs->arr.n; k++) {
        const Str *q = &pairs->arr.items[k]->arr.items[0]->s;
        const Str *p = &pairs->arr.items[k]->arr.items[1]->s;
        float got = 0;
        ASSERT_TRUE(rerank_score(&e, q->ptr, q->len, p->ptr, p->len, 0, &got));
        double d = fabs((double)got - logits->arr.items[k]->num);
        if (getenv("KB_TEST_VERBOSE"))
            fprintf(stderr, "  pair %zu: %.5f vs %.5f\n", k, got, logits->arr.items[k]->num);
        if (d > worst)
            worst = d;
        ASSERT_TRUE(d <= 2e-3);
        if (k < 16)
            scores[k] = got;
    }
    for (size_t i = 0; pairs && i < pairs->arr.n && i < 16; i++)
        for (size_t j = 0; j < pairs->arr.n && j < 16; j++)
            if (logits->arr.items[i]->num > logits->arr.items[j]->num)
                ASSERT_TRUE(scores[i] > scores[j]);
    if (getenv("KB_TEST_VERBOSE"))
        fprintf(stderr, "  worst difference %.2e\n", worst);
    t_begin("reranker: in int8 it keeps the reference's order (within 0.1)");
    ASSERT_TRUE(embed_quantize(&e));
    for (size_t k = 0; pairs && k < pairs->arr.n && k < 16; k++) {
        const Str *q = &pairs->arr.items[k]->arr.items[0]->s;
        const Str *p = &pairs->arr.items[k]->arr.items[1]->s;
        float got = 0;
        ASSERT_TRUE(rerank_score(&e, q->ptr, q->len, p->ptr, p->len, 0, &got));
        ASSERT_TRUE(fabs((double)got - logits->arr.items[k]->num) <= 0.1);
        scores[k] = got;
    }
    for (size_t i = 0; pairs && i < pairs->arr.n && i < 16; i++)
        for (size_t j = 0; j < pairs->arr.n && j < 16; j++)
            if (logits->arr.items[i]->num > logits->arr.items[j]->num)
                ASSERT_TRUE(scores[i] > scores[j]);
    embed_close(&e);
    arena_free(a);
}

void test_modernbert(void) {
    test_reranker();
    const char *model = getenv("KB_TEST_MODERNBERT");
    if (!model || !*model || !plat_is_file(model)) {
        t_begin("modernbert: skipped (KB_TEST_MODERNBERT names no model file)");
        return;
    }
    Arena *a = arena_new(1 << 24);
    char err[512];
    Embedder e;
    t_begin("modernbert: the model file opens as an embedder");
    ASSERT_TRUE(embed_open(a, model, &e, err, sizeof err));
    ASSERT_TRUE(e.kind == EMBED_MODERNBERT);
    ASSERT_EQ_I(e.n_embd, 768);
    ASSERT_TRUE(!e.pool_mean);

    char *data;
    size_t len;
    ASSERT_TRUE(plat_read_file(a, "tests/fixtures/modernbert/reference.json", &data, &len));
    JVal *ref = json_parse(a, data, len, err, sizeof err);
    ASSERT_TRUE(ref != NULL);
    JVal *texts = jobj_get(ref, "texts"), *embs = jobj_get(ref, "embeddings");
    ASSERT_TRUE(texts && embs && texts->arr.n == embs->arr.n);

    t_begin("modernbert: every fixture text embeds to the reference (cosine >= 0.999)");
    float *v = (float *)arena_alloc(a, e.n_embd * sizeof(float));
    double worst = 1.0;
    for (size_t k = 0; k < texts->arr.n; k++) {
        const Str *s = &texts->arr.items[k]->s;
        bool truncated;
        ASSERT_TRUE(embed_text(&e, s->ptr, s->len, false, v, &truncated));
        double c = cosine(v, embs->arr.items[k], e.n_embd);
        if (getenv("KB_TEST_VERBOSE"))
            fprintf(stderr, "  text %zu: cosine %.6f\n", k, c);
        if (c < worst)
            worst = c;
        ASSERT_TRUE(c >= 0.999);
    }
    if (getenv("KB_TEST_VERBOSE"))
        fprintf(stderr, "  worst cosine %.6f\n", worst);

    /* Real text stays within 0.9996 of the float model in int8. The fixture's
     * last two texts are degenerate — nothing at all, and one word six
     * hundred times — and a repeated pattern compounds the rounding: they
     * are held to 0.995 (they measure 0.9969 to 0.9990 across the kernels). */
    t_begin("modernbert: in int8 real text is within cosine 0.999, degenerate text 0.995");
    {
        Embedder q;
        ASSERT_TRUE(embed_open(a, model, &q, err, sizeof err));
        ASSERT_TRUE(embed_quantize(&q));
        float *u = (float *)arena_alloc(a, q.n_embd * sizeof(float));
        for (size_t k = 0; k < texts->arr.n; k++) {
            const Str *s = &texts->arr.items[k]->s;
            bool tr;
            ASSERT_TRUE(embed_text(&q, s->ptr, s->len, false, u, &tr));
            double qc = cosine(u, embs->arr.items[k], q.n_embd);
            if (getenv("KB_TEST_VERBOSE"))
                fprintf(stderr, "  int8 text %zu: cosine %.5f\n", k, qc);
            const Str *txt = &texts->arr.items[k]->s;
            const bool degenerate = txt->len == 0 || k + 1 == texts->arr.n;
            ASSERT_TRUE(qc >= (degenerate ? 0.995 : 0.999));
        }
        embed_close(&q);
    }

    t_begin("modernbert: a query and a document embed the same (no prefixes)");
    const Str *s0 = &texts->arr.items[0]->s;
    float *w = (float *)arena_alloc(a, e.n_embd * sizeof(float));
    bool tr;
    ASSERT_TRUE(embed_text(&e, s0->ptr, s0->len, true, v, &tr));
    ASSERT_TRUE(embed_text(&e, s0->ptr, s0->len, false, w, &tr));
    ASSERT_TRUE(memcmp(v, w, e.n_embd * sizeof(float)) == 0);

    t_begin("modernbert: whitespace at the edges is stripped, as sentence-transformers does");
    const char *code = "static int f(void);";
    const char *indented = "\n\t  static int f(void);  \r\n";
    ASSERT_TRUE(embed_text(&e, code, strlen(code), false, v, &tr));
    ASSERT_TRUE(embed_text(&e, indented, strlen(indented), false, w, &tr));
    ASSERT_TRUE(memcmp(v, w, e.n_embd * sizeof(float)) == 0);

    embed_close(&e);
    arena_free(a);
}
