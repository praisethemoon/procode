/* kb's byte-level BPE against the Hugging Face tokenizer's own ids.
 *
 * The ids come from tests/fixtures/modernbert/reference.json, written by
 * tools/modernbert/reference.py from the real models. The vocabulary and the
 * merges are only in the model file, so these tests read it from
 * KB_TEST_MODERNBERT (the Makefile names ~/.kb/models' copy when there is
 * one, read-only) and are skipped without it. The whole-repository check is
 * tools/modernbert/check_tokenizer.py. */

#include "test.h"

#include "../../src/bpe.h"
#include "../../src/json.h"
#include "../../src/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool same_ids(const int32_t *got, size_t n, const JVal *want) {
    if (!want || want->t != J_ARR || want->arr.n != n)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (want->arr.items[i]->i != got[i])
            return false;
    }
    return true;
}

static bool stripped(const char *in, const char *want) {
    const char *s = in;
    size_t n = strlen(in);
    bpe_strip(&s, &n);
    return n == strlen(want) && memcmp(s, want, n) == 0;
}

void test_bpe(void) {
    t_begin("bpe: strip cuts what Python's str.strip cuts, and nothing else");
    ASSERT_TRUE(stripped("", ""));
    ASSERT_TRUE(stripped(" \t\r\n ", ""));
    ASSERT_TRUE(stripped("  a b  ", "a b"));
    ASSERT_TRUE(stripped("\n    call _main\n", "call _main"));
    ASSERT_TRUE(stripped("\xc2\xa0x\xe3\x80\x80", "x"));        /* NBSP, ideographic space */
    ASSERT_TRUE(stripped("\x1fx\x1c", "x"));                      /* Python's separators */
    ASSERT_TRUE(stripped("\xe2\x80\x8bx", "\xe2\x80\x8bx"));  /* ZWSP is not whitespace */
    ASSERT_TRUE(stripped("x\xc3\xa9", "x\xc3\xa9"));
    ASSERT_TRUE(stripped("x \xff", "x \xff"));                    /* invalid UTF-8 stops it */


    const char *model = getenv("KB_TEST_MODERNBERT");
    if (!model || !*model || !plat_is_file(model)) {
        t_begin("bpe: skipped (KB_TEST_MODERNBERT names no model file)");
        return;
    }
    Arena *a = arena_new(1 << 24);
    char err[512];
    Gguf g;
    Bpe b;
    t_begin("bpe: the model file's tokenizer loads");
    ASSERT_TRUE(gguf_open(a, model, &g, err, sizeof err));
    ASSERT_TRUE(bpe_init(a, &g, &b, err, sizeof err));

    char *data;
    size_t len;
    ASSERT_TRUE(plat_read_file(a, "tests/fixtures/modernbert/reference.json", &data, &len));
    JVal *ref = json_parse(a, data, len, err, sizeof err);
    ASSERT_TRUE(ref != NULL);
    JVal *texts = jobj_get(ref, "texts"), *tokens = jobj_get(ref, "tokens");
    JVal *pairs = jobj_get(ref, "pairs"), *pair_tokens = jobj_get(ref, "pair_tokens");
    ASSERT_TRUE(texts && tokens && pairs && pair_tokens);

    int32_t *ids = (int32_t *)arena_alloc(a, 8192 * sizeof(int32_t));
    bool truncated;

    t_begin("bpe: every fixture text gives the reference ids");
    for (size_t k = 0; k < texts->arr.n; k++) {
        const Str *s = &texts->arr.items[k]->s;
        size_t n = bpe_encode(a, &b, s->ptr, s->len, true, ids, 8192, &truncated);
        ASSERT_TRUE(!truncated);
        ASSERT_TRUE(same_ids(ids, n, tokens->arr.items[k]));
    }

    t_begin("bpe: every fixture pair gives the reference ids");
    for (size_t k = 0; k < pairs->arr.n; k++) {
        const JVal *p = pairs->arr.items[k];
        const Str *q = &p->arr.items[0]->s, *d = &p->arr.items[1]->s;
        size_t n = bpe_encode_pair(a, &b, q->ptr, q->len, d->ptr, d->len, ids, 8192, &truncated);
        ASSERT_TRUE(!truncated);
        ASSERT_TRUE(same_ids(ids, n, pair_tokens->arr.items[k]));
    }

    t_begin("bpe: truncation keeps [CLS] first, [SEP] last, and the prefix");
    const JVal *long_ids = tokens->arr.items[texts->arr.n - 1];
    const Str *long_text = &texts->arr.items[texts->arr.n - 1]->s;
    size_t n = bpe_encode(a, &b, long_text->ptr, long_text->len, true, ids, 64, &truncated);
    ASSERT_TRUE(truncated);
    ASSERT_EQ_I((int64_t)n, 64);
    ASSERT_EQ_I(ids[0], b.cls);
    ASSERT_EQ_I(ids[63], b.sep);
    for (size_t i = 1; i < 63; i++)
        ASSERT_EQ_I(ids[i], long_ids->arr.items[i]->i);

    t_begin("bpe: a pair that does not fit cuts the passage, not the query");
    const JVal *p0 = pairs->arr.items[0];
    const Str *q0 = &p0->arr.items[0]->s, *d0 = &p0->arr.items[1]->s;
    n = bpe_encode_pair(a, &b, q0->ptr, q0->len, d0->ptr, d0->len, ids, 20, &truncated);
    ASSERT_TRUE(truncated);
    ASSERT_EQ_I((int64_t)n, 20);
    const JVal *full = pair_tokens->arr.items[0];
    size_t qsep = 0;
    while (full->arr.items[qsep]->i != b.sep)
        qsep++;
    for (size_t i = 0; i <= qsep; i++)
        ASSERT_EQ_I(ids[i], full->arr.items[i]->i);
    ASSERT_EQ_I(ids[19], b.sep);

    t_begin("bpe: a megabyte-long piece is bounded by the output budget");
    size_t big = 1u << 20;
    char *letters = (char *)arena_alloc(a, big);
    for (size_t i = 0; i < big; i++)
        letters[i] = (char)('a' + (i * 7) % 26);
    clock_t t0 = clock();
    n = bpe_encode(a, &b, letters, big, true, ids, 512, &truncated);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    ASSERT_TRUE(truncated);
    ASSERT_EQ_I((int64_t)n, 512);
    ASSERT_TRUE(secs < 2.0);

    arena_free(a);
}
