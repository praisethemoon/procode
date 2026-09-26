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

void test_modernbert(void) {
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
