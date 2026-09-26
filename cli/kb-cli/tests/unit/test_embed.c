/* The embedder's three foundations, each against inputs built by hand:
 *
 * - the GGUF reader, on files this test writes byte by byte, including ones
 *   it must refuse;
 * - f16 and k-quant dequantisation, on blocks packed from chosen scales and
 *   quants, so the expected weight of every element is known exactly — the
 *   six-bit scale unpack is the thing most likely to be wrong and least
 *   likely to look wrong;
 * - WordPiece, on a vocabulary small enough to reason about.
 *
 * None of this needs the real model; the e2e script covers that when
 * KB_TEST_MODEL names one. */

#include "test.h"
#include "test_tmp.h"

#include "../../src/gguf.h"
#include "../../src/quant.h"
#include "../../src/vectors.h"
#include "../../src/wpm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a GGUF writer, just enough for these tests ------------------------ */

typedef struct {
    uint8_t buf[8192];
    size_t n;
} Bytes;

static void put(Bytes *b, const void *p, size_t n) {
    memcpy(b->buf + b->n, p, n);
    b->n += n;
}
static void put_u32(Bytes *b, uint32_t v) { put(b, &v, 4); }
static void put_u64(Bytes *b, uint64_t v) { put(b, &v, 8); }
static void put_str(Bytes *b, const char *s) {
    put_u64(b, strlen(s));
    put(b, s, strlen(s));
}
static void kv_str(Bytes *b, const char *key, const char *val) {
    put_str(b, key);
    put_u32(b, GGUF_STRING);
    put_str(b, val);
}
static void kv_u32(Bytes *b, const char *key, uint32_t val) {
    put_str(b, key);
    put_u32(b, GGUF_U32);
    put_u32(b, val);
}
static void kv_strs(Bytes *b, const char *key, const char *const *v,
                    size_t n) {
    put_str(b, key);
    put_u32(b, GGUF_ARRAY);
    put_u32(b, GGUF_STRING);
    put_u64(b, n);
    for (size_t i = 0; i < n; i++)
        put_str(b, v[i]);
}
static void header(Bytes *b, uint64_t n_tensors, uint64_t n_kv) {
    put(b, "GGUF", 4);
    put_u32(b, 3);
    put_u64(b, n_tensors);
    put_u64(b, n_kv);
}
static void pad_to(Bytes *b, size_t align) {
    while (b->n % align)
        b->buf[b->n++] = 0;
}

static bool write_file(const char *path, const Bytes *b) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(b->buf, 1, b->n, f) == b->n;
    return fclose(f) == 0 && ok;
}

/* ---- the reader -------------------------------------------------------- */

static void gguf_tests(Arena *a, const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/tiny.gguf", dir);

    t_begin("gguf: metadata, a string array and a tensor come back as written");
    static Bytes b;
    b.n = 0;
    header(&b, 1, 4);
    kv_str(&b, "general.architecture", "tiny");
    kv_u32(&b, "general.alignment", 32);
    kv_u32(&b, "tiny.block_count", 7);
    static const char *const toks[] = {"alpha", "beta", ""};
    kv_strs(&b, "tokenizer.ggml.tokens", toks, 3);
    put_str(&b, "w");
    put_u32(&b, 2);
    put_u64(&b, 4); /* ne[0]: a row is 4 values */
    put_u64(&b, 2); /* ne[1]: 2 rows */
    put_u32(&b, GGML_F32);
    put_u64(&b, 0);
    pad_to(&b, 32);
    for (int32_t i = 0; i < 8; i++) {
        float f = (float)i * 0.5f;
        put(&b, &f, 4);
    }
    ASSERT_TRUE(write_file(path, &b));
    Gguf g;
    char err[256];
    bool opened = gguf_open(a, path, &g, err, sizeof err);
    ASSERT_TRUE(opened);
    if (!opened)
        return;
    ASSERT_EQ_S(gguf_str(&g, "general.architecture", "?"), "tiny");
    ASSERT_EQ_I(gguf_u64(&g, "tiny.block_count", 0), 7);
    ASSERT_EQ_I(gguf_u64(&g, "no.such.key", 42), 42);
    Str *v;
    uint64_t nv;
    ASSERT_TRUE(gguf_str_array(a, &g, "tokenizer.ggml.tokens", &v, &nv));
    ASSERT_EQ_I(nv, 3);
    ASSERT_TRUE(str_eq_c(v[1], "beta"));
    ASSERT_EQ_I(v[2].len, 0);
    const GgufTensor *t = gguf_tensor(&g, "w");
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ_I(t->ne[0], 4);
    ASSERT_EQ_I(t->ne[1], 2);
    ASSERT_EQ_I(t->bytes, 32);
    const float *data = (const float *)t->data;
    ASSERT_NEAR(data[7], 3.5, 0);
    gguf_close(&g);

    t_begin("gguf: a wrong magic is refused");
    b.buf[0] = 'X';
    ASSERT_TRUE(write_file(path, &b));
    ASSERT_TRUE(!gguf_open(a, path, &g, err, sizeof err));
    b.buf[0] = 'G';

    t_begin("gguf: a file cut short is refused, never half-read");
    Bytes cut = b;
    cut.n -= 4;
    ASSERT_TRUE(write_file(path, &cut));
    ASSERT_TRUE(!gguf_open(a, path, &g, err, sizeof err));

    t_begin("gguf: a tensor offset past the data is refused");
    b.n = 0;
    header(&b, 1, 1);
    kv_u32(&b, "general.alignment", 32);
    put_str(&b, "w");
    put_u32(&b, 1);
    put_u64(&b, 4);
    put_u32(&b, GGML_F32);
    put_u64(&b, 4096);
    pad_to(&b, 32);
    float zero[4] = {0};
    put(&b, zero, sizeof zero);
    ASSERT_TRUE(write_file(path, &b));
    ASSERT_TRUE(!gguf_open(a, path, &g, err, sizeof err));
}

/* ---- dequantisation ---------------------------------------------------- */

static void put_f16(uint8_t *p, uint16_t h) {
    p[0] = (uint8_t)(h & 0xFF);
    p[1] = (uint8_t)(h >> 8);
}

/* The inverse of the 12-byte six-bit scale/min packing: scales 0-3 and mins
 * 0-3 in the low six bits of bytes 0-7; scales and mins 4-7 as a low nibble
 * in bytes 8-11 and their top two bits in the high bits of bytes 0-7. */
static void pack_scales(const uint8_t sc[8], const uint8_t m[8],
                        uint8_t out[12]) {
    for (int32_t j = 0; j < 4; j++) {
        out[j] = (uint8_t)(sc[j] | ((sc[j + 4] >> 4) << 6));
        out[j + 4] = (uint8_t)(m[j] | ((m[j + 4] >> 4) << 6));
        out[j + 8] = (uint8_t)((sc[j + 4] & 0xF) | ((m[j + 4] & 0xF) << 4));
    }
}

static void quant_tests(void) {
    t_begin("quant: f16 covers normals, signs, the largest, subnormals and inf");
    ASSERT_NEAR(quant_f16(0x3C00), 1.0, 0);
    ASSERT_NEAR(quant_f16(0xC000), -2.0, 0);
    ASSERT_NEAR(quant_f16(0x3555), 0.333251953125, 0);
    ASSERT_NEAR(quant_f16(0x7BFF), 65504.0, 0);
    ASSERT_NEAR(quant_f16(0x0001), 5.9604644775390625e-8, 0);
    ASSERT_NEAR(quant_f16(0x0000), 0.0, 0);
    ASSERT_TRUE(isinf(quant_f16(0x7C00)) && quant_f16(0x7C00) > 0);

    t_begin("quant: a Q4_K super-block, every scale and min unpacked");
    /* Scales and mins above 15 for sub-blocks 4-7, so the two bits that live
     * in another byte matter. */
    static const uint8_t sc[8] = {1, 2, 3, 4, 17, 33, 50, 63};
    static const uint8_t mn[8] = {0, 5, 10, 15, 20, 40, 48, 62};
    uint8_t q4[Q4_K_BYTES];
    memset(q4, 0, sizeof q4);
    put_f16(q4, 0x3C00);     /* d = 1 */
    put_f16(q4 + 2, 0x3800); /* dmin = 0.5 */
    pack_scales(sc, mn, q4 + 4);
    /* Value i's quant is i % 16; in each 64-value group the first 32 values
     * are the low nibbles and the next 32 the high nibbles of the same 32
     * bytes. */
    for (int32_t g = 0; g < 4; g++)
        for (int32_t l = 0; l < 32; l++) {
            uint8_t lo = (uint8_t)((g * 64 + l) % 16);
            uint8_t hi = (uint8_t)((g * 64 + 32 + l) % 16);
            q4[16 + g * 32 + l] = (uint8_t)(lo | (hi << 4));
        }
    float y[QK_K];
    ASSERT_TRUE(quant_row(GGML_Q4_K, q4, QK_K, y));
    bool all = true;
    for (int32_t i = 0; i < QK_K && all; i++) {
        int32_t j = i / 32;
        float want = 1.0f * sc[j] * (float)(i % 16) - 0.5f * mn[j];
        if (y[i] != want) {
            all = false;
            fprintf(stderr, "  Q4_K value %d: %g, want %g\n", i, y[i], want);
        }
    }
    ASSERT_TRUE(all);

    t_begin("quant: a Q6_K super-block, both bit-planes and signed scales");
    uint8_t q6[Q6_K_BYTES];
    memset(q6, 0, sizeof q6);
    int8_t s6[16];
    for (int32_t i = 0; i < 16; i++)
        s6[i] = (int8_t)(i % 2 ? -(i + 1) : (i + 1));
    memcpy(q6 + 192, s6, 16);
    put_f16(q6 + 208, 0x3400); /* d = 0.25 */
    /* Chosen six-bit quants, placed by the inverse of the strided layout:
     * in each 128-value half, value l, l+32, l+64, l+96 share a byte of ql
     * (two nibbles across two bytes) and one byte of qh (two bits each). */
    uint8_t want_q[QK_K];
    for (int32_t i = 0; i < QK_K; i++)
        want_q[i] = (uint8_t)((i * 7 + 3) % 64);
    for (int32_t h = 0; h < 2; h++) {
        uint8_t *ql = q6 + h * 64;
        uint8_t *qh = q6 + 128 + h * 32;
        const uint8_t *w = want_q + h * 128;
        for (int32_t l = 0; l < 32; l++) {
            ql[l] = (uint8_t)((w[l] & 0xF) | ((w[l + 64] & 0xF) << 4));
            ql[l + 32] = (uint8_t)((w[l + 32] & 0xF) | ((w[l + 96] & 0xF) << 4));
            qh[l] = (uint8_t)((w[l] >> 4) | ((w[l + 32] >> 4) << 2) |
                              ((w[l + 64] >> 4) << 4) | ((w[l + 96] >> 4) << 6));
        }
    }
    ASSERT_TRUE(quant_row(GGML_Q6_K, q6, QK_K, y));
    all = true;
    for (int32_t i = 0; i < QK_K && all; i++) {
        /* Sixteen scales, one per 16 values, in value order. */
        float want = 0.25f * (float)s6[i / 16] * (float)((int32_t)want_q[i] - 32);
        if (y[i] != want) {
            all = false;
            fprintf(stderr, "  Q6_K value %d: %g, want %g\n", i, y[i], want);
        }
    }
    ASSERT_TRUE(all);

    t_begin("quant: a row that is not whole super-blocks is refused");
    uint64_t rb;
    ASSERT_TRUE(quant_row_bytes(GGML_Q4_K, 512, &rb) && rb == 2 * Q4_K_BYTES);
    ASSERT_TRUE(!quant_row_bytes(GGML_Q4_K, 300, &rb));
    ASSERT_TRUE(!quant_row_bytes(99, 256, &rb));

    t_begin("quant: the matvec agrees with the rows it multiplies");
    uint8_t w2[2 * Q4_K_BYTES];
    memcpy(w2, q4, Q4_K_BYTES);
    memcpy(w2 + Q4_K_BYTES, q4, Q4_K_BYTES);
    w2[Q4_K_BYTES] = 0x00;
    w2[Q4_K_BYTES + 1] = 0x40; /* the second row's d = 2 */
    float x[QK_K], out[2], row[QK_K];
    for (int32_t i = 0; i < QK_K; i++)
        x[i] = (float)((i % 5) - 2) * 0.25f;
    ASSERT_TRUE(quant_matvec(GGML_Q4_K, w2, QK_K, 2, x, out));
    for (int32_t r = 0; r < 2; r++) {
        ASSERT_TRUE(quant_row(GGML_Q4_K, w2 + r * Q4_K_BYTES, QK_K, row));
        double dot = 0;
        for (int32_t i = 0; i < QK_K; i++)
            dot += (double)row[i] * x[i];
        ASSERT_NEAR(out[r], dot, 1e-3);
    }
}

/* ---- WordPiece --------------------------------------------------------- */

#define M "\xE2\x96\x81" /* U+2581, this vocabulary's start-of-word marker */

static void wpm_tests(Arena *a, const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/vocab.gguf", dir);
    static const char *const vocab[] = {
        "[PAD]",     "[UNK]",  "[CLS]",   "[SEP]",    M "hello", M "world",
        M "io",      M "_",    M "uring", "ing",      M "play",  M "!",
        M "cafe",    M "play", NULL};
    size_t nv = 0;
    while (vocab[nv])
        nv++;
    static Bytes b;
    b.n = 0;
    header(&b, 0, 5);
    kv_str(&b, "tokenizer.ggml.model", "bert");
    kv_strs(&b, "tokenizer.ggml.tokens", vocab, nv);
    kv_u32(&b, "tokenizer.ggml.bos_token_id", 2);
    kv_u32(&b, "tokenizer.ggml.eos_token_id", 3);
    kv_u32(&b, "tokenizer.ggml.unknown_token_id", 1);
    /* No tensors, but the data section still starts on the alignment. */
    pad_to(&b, 32);
    ASSERT_TRUE(write_file(path, &b));
    Gguf g;
    char err[256];
    bool opened = gguf_open(a, path, &g, err, sizeof err);
    ASSERT_TRUE(opened);
    Wpm w;
    bool ready = opened && wpm_init(a, &g, &w, err, sizeof err);
    ASSERT_TRUE(ready);
    if (!ready)
        return; /* everything below reads the vocabulary */

    t_begin("wpm: words split on punctuation, lower-cased, accents gone");
    Str *words;
    const char *text = "io_uring Caf\xC3\xA9!";
    size_t n = wpm_words(a, text, strlen(text), &words);
    ASSERT_EQ_I(n, 5);
    ASSERT_TRUE(n == 5 && str_eq_c(words[0], "io") && str_eq_c(words[1], "_") &&
                str_eq_c(words[2], "uring") && str_eq_c(words[3], "cafe") &&
                str_eq_c(words[4], "!"));

    t_begin("wpm: longest match first, continuation pieces, [CLS] and [SEP]");
    int32_t ids[16];
    bool trunc = false;
    n = wpm_encode(a, &w, "Hello playing", 13, true, ids, 16, &trunc);
    ASSERT_EQ_I(n, 5);
    ASSERT_TRUE(n == 5 && ids[0] == 2 && ids[1] == 4 && ids[2] == 10 &&
                ids[3] == 9 && ids[4] == 3);
    ASSERT_TRUE(!trunc);

    t_begin("wpm: a repeated vocabulary string keeps its first id");
    ASSERT_EQ_I(wpm_lookup(&w, M "play", strlen(M "play")), 10);

    t_begin("wpm: a word with any unmatchable position is one [UNK]");
    n = wpm_encode(a, &w, "hello zzz", 9, false, ids, 16, &trunc);
    ASSERT_TRUE(n == 2 && ids[0] == 4 && ids[1] == 1);

    t_begin("wpm: truncation keeps [SEP] last and says it happened");
    n = wpm_encode(a, &w, "hello world hello world", 23, true, ids, 4, &trunc);
    ASSERT_EQ_I(n, 4);
    ASSERT_TRUE(trunc && ids[0] == 2 && ids[3] == 3);

    t_begin("wpm: a tokenizer this build does not implement is refused");
    b.n = 0;
    header(&b, 0, 2);
    kv_str(&b, "tokenizer.ggml.model", "gpt2");
    kv_strs(&b, "tokenizer.ggml.tokens", vocab, nv);
    pad_to(&b, 32);
    gguf_close(&g); /* before the file under the mapping is rewritten */
    ASSERT_TRUE(write_file(path, &b));
    opened = gguf_open(a, path, &g, err, sizeof err);
    ASSERT_TRUE(opened);
    if (opened) {
        ASSERT_TRUE(!wpm_init(a, &g, &w, err, sizeof err));
        gguf_close(&g);
    }
}

/* ---- the vector file's lookups ---------------------------------------- */

static void vec_tests(void) {
    t_begin("vectors: ids are found by bisection, absent ones are not");
    int64_t ids[] = {3, 7, 8, 120, 4000};
    float scales[] = {0.5f, 1.0f, 1.0f, 1.0f, 0.01f};
    int8_t q[5 * 4] = {1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                       127, -127, 0, 1};
    VecSet v;
    memset(&v, 0, sizeof v);
    v.dim = 4;
    v.n = 5;
    v.ids = ids;
    v.scales = scales;
    v.q = q;
    ASSERT_EQ_I(vec_find(&v, 3), 0);
    ASSERT_EQ_I(vec_find(&v, 120), 3);
    ASSERT_EQ_I(vec_find(&v, 4000), 4);
    ASSERT_EQ_I(vec_find(&v, 5), -1);
    ASSERT_EQ_I(vec_find(&v, 9999), -1);

    t_begin("vectors: a score is the scale times the int8 dot product");
    float x[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    ASSERT_NEAR(vec_score(&v, 0, x), 0.5 * 10, 1e-6);
    float y[4] = {1.0f, 1.0f, 2.0f, 3.0f};
    ASSERT_NEAR(vec_score(&v, 4, y), 0.01 * (127 - 127 + 0 + 3), 1e-6);
}

void test_embed(void) {
    Arena *a = arena_new(1 << 20);
    char dir[512];
    tmp_dir(dir, sizeof dir);
    gguf_tests(a, dir);
    quant_tests();
    wpm_tests(a, dir);
    vec_tests();
    tmp_rm(a, dir);
    arena_free(a);
}

/* The int8 product against the float one, on a matrix whose rows and tokens
 * are not multiples of the kernel's blocks, so every path runs. */
void test_q8(void) {
    t_begin("q8: the int8 product is within 1% of the float one");
    Arena *a = arena_new(1 << 20);
    const uint64_t n = 96, rows = 7;
    const size_t T = 5;
    float *w = (float *)arena_alloc(a, rows * n * sizeof(float));
    float *x = (float *)arena_alloc(a, T * n * sizeof(float));
    uint32_t s = 1;
    for (uint64_t i = 0; i < rows * n; i++) {
        s = s * 1103515245u + 12345u;
        w[i] = (float)((int32_t)(s >> 8) % 2001 - 1000) / 1000.0f;
    }
    for (size_t i = 0; i < T * n; i++) {
        s = s * 1103515245u + 12345u;
        x[i] = (float)((int32_t)(s >> 8) % 2001 - 1000) / 1000.0f;
    }
    Q8Matrix m;
    ASSERT_TRUE(q8_from(a, GGML_F32, (const uint8_t *)w, n, rows, &m));
    float *want = (float *)arena_alloc(a, T * rows * sizeof(float));
    float *got = (float *)arena_alloc(a, T * rows * sizeof(float));
    int8_t *xq = (int8_t *)arena_alloc(a, T * n);
    float *xd = (float *)arena_alloc(a, T * (n / Q8_BLOCK) * sizeof(float));
    ASSERT_TRUE(quant_matmul(GGML_F32, (const uint8_t *)w, n, rows, x, T, want));
    q8_matmul(&m, x, T, xq, xd, got);
    double num = 0, den = 0;
    for (size_t i = 0; i < T * rows; i++) {
        num += (double)(got[i] - want[i]) * (got[i] - want[i]);
        den += (double)want[i] * want[i];
    }
    ASSERT_TRUE(den > 0 && sqrt(num / den) < 0.01);
    t_begin("q8: widths that are not a multiple of the block are refused");
    ASSERT_TRUE(!q8_from(a, GGML_F32, (const uint8_t *)w, 50, 1, &m));
    arena_free(a);
}
