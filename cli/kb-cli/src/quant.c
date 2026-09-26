#include "quant.h"

#include "gguf.h"

#include "platform.h"
#include "simd.h"

bool quant_simd(void) {
#ifdef KB_SIMD4
    return true;
#else
    return false;
#endif
}

float quant_f16(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t man = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign; /* +-0 */
        } else {
            /* Subnormal: renormalise by shifting the mantissa up until its
             * leading bit falls off, paying one exponent step each time. */
            exp = 127 - 15 + 1;
            while ((man & 0x400u) == 0) {
                man <<= 1;
                exp--;
            }
            man &= 0x3FFu;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 0x1Fu) {
        bits = sign | 0x7F800000u | (man << 13); /* inf / NaN */
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    }
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool quant_row_bytes(uint32_t t, uint64_t n, uint64_t *out) {
    switch (t) {
    case GGML_F32:
        *out = n * 4;
        return true;
    case GGML_F16:
        *out = n * 2;
        return true;
    case GGML_Q4_K:
    case GGML_Q5_K:
    case GGML_Q6_K:
        if (n % QK_K != 0)
            return false;
        *out = (n / QK_K) *
               (t == GGML_Q4_K ? Q4_K_BYTES
                               : (t == GGML_Q5_K ? Q5_K_BYTES : Q6_K_BYTES));
        return true;
    default:
        return false;
    }
}

/* ---- the six-bit scale field -------------------------------------------
 *
 * Q4_K and Q5_K carry eight sub-block scales and eight sub-block minimums in
 * twelve bytes. The first four of each fit whole in the low six bits of
 * bytes 0..7. The last four are split: their low four bits sit in the low
 * nibble (scale) or the high nibble (minimum) of bytes 8..11, and their top
 * two bits are stolen from the top two bits of bytes 0..7 — bytes j-4 for a
 * scale and byte j for a minimum.
 *
 * That asymmetry is the trap. `q[j-4] >> 6` and `q[j] >> 6` are the same
 * shift of different bytes, and swapping them produces weights that are
 * wrong by a factor of up to four in a quarter of every super-block, with
 * nothing in the output that looks broken.
 */
static void scale_min_k4(int32_t j, const uint8_t *q, uint8_t *d, uint8_t *m) {
    if (j < 4) {
        *d = q[j] & 63u;
        *m = q[j + 4] & 63u;
    } else {
        *d = (uint8_t)((q[j + 4] & 0xFu) | ((q[j - 4] >> 6) << 4));
        *m = (uint8_t)((q[j + 4] >> 4) | ((q[j] >> 6) << 4));
    }
}

/* ---- one super-block at a time ------------------------------------------
 *
 * Each writes exactly QK_K floats. Within a 64-value group the FIRST 32
 * values come from the LOW nibbles of 32 bytes and the NEXT 32 from the HIGH
 * nibbles of the SAME 32 bytes — not from the next 32 bytes. Reading them in
 * byte order instead would interleave two sub-blocks that have different
 * scales, which again looks like noise rather than like a bug.
 */
static void deq_q4_k(const uint8_t *b, float *y) {
    const float d = quant_f16(rd_u16(b));
    const float dmin = quant_f16(rd_u16(b + 2));
    const uint8_t *scales = b + 4;
    const uint8_t *q = b + 16;
    int32_t is = 0;
    for (int32_t j = 0; j < QK_K; j += 64) {
        uint8_t sc, m;
        scale_min_k4(is, scales, &sc, &m);
        const float d1 = d * sc, m1 = dmin * m;
        scale_min_k4(is + 1, scales, &sc, &m);
        const float d2 = d * sc, m2 = dmin * m;
        for (int32_t l = 0; l < 32; l++)
            y[j + l] = d1 * (float)(q[l] & 0xFu) - m1;
        for (int32_t l = 0; l < 32; l++)
            y[j + 32 + l] = d2 * (float)(q[l] >> 4) - m2;
        q += 32;
        is += 2;
    }
}

static void deq_q5_k(const uint8_t *b, float *y) {
    const float d = quant_f16(rd_u16(b));
    const float dmin = quant_f16(rd_u16(b + 2));
    const uint8_t *scales = b + 4;
    const uint8_t *qh = b + 16;
    const uint8_t *ql = b + 48;
    int32_t is = 0;
    /* The fifth bit of all 256 quants lives in 32 bytes, two bits of every
     * byte per 64-value group, so the masks walk left by two each group. */
    uint8_t u1 = 1, u2 = 2;
    for (int32_t j = 0; j < QK_K; j += 64) {
        uint8_t sc, m;
        scale_min_k4(is, scales, &sc, &m);
        const float d1 = d * sc, m1 = dmin * m;
        scale_min_k4(is + 1, scales, &sc, &m);
        const float d2 = d * sc, m2 = dmin * m;
        for (int32_t l = 0; l < 32; l++)
            y[j + l] =
                d1 * (float)((ql[l] & 0xFu) + ((qh[l] & u1) ? 16 : 0)) - m1;
        for (int32_t l = 0; l < 32; l++)
            y[j + 32 + l] =
                d2 * (float)((ql[l] >> 4) + ((qh[l] & u2) ? 16 : 0)) - m2;
        ql += 32;
        is += 2;
        u1 = (uint8_t)(u1 << 2);
        u2 = (uint8_t)(u2 << 2);
    }
}

/* Q6_K is the odd one: six bits per weight as four low bits plus two high
 * bits, sixteen signed 8-bit sub-block scales, one half-precision
 * super-block scale, and the quant biased by 32 so it is signed. Its 128-
 * value halves are strided rather than sequential — l, l+32, l+64, l+96 —
 * because that is how the two bit-planes line up. */
static void deq_q6_k(const uint8_t *b, float *y) {
    const uint8_t *ql = b;
    const uint8_t *qh = b + 128;
    const int8_t *sc = (const int8_t *)(b + 192);
    const float d = quant_f16(rd_u16(b + 208));
    for (int32_t n = 0; n < QK_K; n += 128) {
        for (int32_t l = 0; l < 32; l++) {
            const int32_t is = l / 16;
            const int32_t q1 =
                (int32_t)((ql[l] & 0xFu) | (((qh[l] >> 0) & 3u) << 4)) - 32;
            const int32_t q2 =
                (int32_t)((ql[l + 32] & 0xFu) | (((qh[l] >> 2) & 3u) << 4)) -
                32;
            const int32_t q3 =
                (int32_t)((ql[l] >> 4) | (((qh[l] >> 4) & 3u) << 4)) - 32;
            const int32_t q4 =
                (int32_t)((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3u) << 4)) - 32;
            y[n + l] = d * (float)sc[is] * (float)q1;
            y[n + l + 32] = d * (float)sc[is + 2] * (float)q2;
            y[n + l + 64] = d * (float)sc[is + 4] * (float)q3;
            y[n + l + 96] = d * (float)sc[is + 6] * (float)q4;
        }
        ql += 64;
        qh += 32;
        sc += 8;
    }
}

static size_t block_bytes(uint32_t t) {
    return t == GGML_Q4_K ? Q4_K_BYTES
                          : (t == GGML_Q5_K ? Q5_K_BYTES : Q6_K_BYTES);
}

static void deq_block(uint32_t t, const uint8_t *b, float *y) {
    if (t == GGML_Q4_K)
        deq_q4_k(b, y);
    else if (t == GGML_Q5_K)
        deq_q5_k(b, y);
    else
        deq_q6_k(b, y);
}

bool quant_row(uint32_t t, const uint8_t *src, uint64_t n, float *out) {
    if (t == GGML_F32) {
        /* memcpy rather than a cast: the mapping has no alignment guarantee
         * beyond the file's, and a misaligned float load is undefined. */
        memcpy(out, src, (size_t)n * sizeof(float));
        return true;
    }
    if (t == GGML_F16) {
        for (uint64_t i = 0; i < n; i++)
            out[i] = quant_f16(rd_u16(src + i * 2));
        return true;
    }
    if (t != GGML_Q4_K && t != GGML_Q5_K && t != GGML_Q6_K)
        return false;
    if (n % QK_K != 0)
        return false;
    const size_t bsz = block_bytes(t);
    for (uint64_t b = 0; b < n / QK_K; b++)
        deq_block(t, src + b * bsz, out + b * QK_K);
    return true;
}

/* ---- the dot product ----------------------------------------------------
 *
 * Four accumulators, joined once per super-block. The order is written out
 * rather than left to the compiler because the SIMD path below has to make
 * the same additions in the same order: floating-point addition is not
 * associative, and two reduction orders would make `--simd` a change in the
 * answer rather than a change in the speed.
 */
static float dot256(const float *w, const float *x) {
#ifdef KB_SIMD4
    v4f a0 = v4_zero(), a1 = a0, a2 = a0, a3 = a0;
    for (int32_t k = 0; k < QK_K; k += 16) {
        a0 = v4_madd(a0, v4_load(w + k), v4_load(x + k));
        a1 = v4_madd(a1, v4_load(w + k + 4), v4_load(x + k + 4));
        a2 = v4_madd(a2, v4_load(w + k + 8), v4_load(x + k + 8));
        a3 = v4_madd(a3, v4_load(w + k + 12), v4_load(x + k + 12));
    }
    return v4_sum(v4_add(v4_add(a0, a1), v4_add(a2, a3)));
#else
    float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    for (int32_t k = 0; k < QK_K; k += 4) {
        a0 += w[k] * x[k];
        a1 += w[k + 1] * x[k + 1];
        a2 += w[k + 2] * x[k + 2];
        a3 += w[k + 3] * x[k + 3];
    }
    return (a0 + a1) + (a2 + a3);
#endif
}

bool quant_matvec(uint32_t t, const uint8_t *w, uint64_t n, uint64_t rows,
                  const float *x, float *out) {
    if (t == GGML_F32 || t == GGML_F16) {
        /* One vector through an F32 or F16 matrix. ModernBERT's matrices
         * are F16, and quant_matmul gives them their own shared-row path;
         * this one serves single vectors and F32 files. */
        const size_t width = t == GGML_F32 ? 4u : 2u;
        for (uint64_t r = 0; r < rows; r++) {
            const uint8_t *row = w + r * n * width;
            float s = 0.0f;
            for (uint64_t i = 0; i < n; i++) {
                float v;
                if (t == GGML_F32)
                    memcpy(&v, row + i * 4, sizeof v);
                else
                    v = quant_f16(rd_u16(row + i * 2));
                s += v * x[i];
            }
            out[r] = s;
        }
        return true;
    }
    if (t != GGML_Q4_K && t != GGML_Q5_K && t != GGML_Q6_K)
        return false;
    if (n % QK_K != 0)
        return false;
    const size_t bsz = block_bytes(t);
    const uint64_t nb = n / QK_K;
    float buf[QK_K];
    for (uint64_t r = 0; r < rows; r++) {
        const uint8_t *row = w + r * nb * bsz;
        float sum = 0.0f;
        for (uint64_t b = 0; b < nb; b++) {
            deq_block(t, row + b * bsz, buf);
            sum += dot256(buf, x + b * QK_K);
        }
        out[r] = sum;
    }
    return true;
}

typedef struct {
    uint32_t t;
    const uint8_t *w;
    uint64_t n, rows;
    const float *x;
    size_t T;
    float *out;
} MatmulJob;

/* Matrix products, four rows and four tokens at a time.
 *
 * WHY BLOCKS. A plain dot product loads one weight and one input for every
 * multiply-add, so the loads, not the arithmetic, set the pace. Holding four
 * weight rows against four tokens' inputs gives sixteen running sums from
 * eight loads, which is what lets the multiply-add units stay busy.
 *
 * Each group of four rows is widened to floats once, whole (F16 converted,
 * a K-quant dequantised), and reused for every token; each output is then
 * written once. The width must be a multiple
 * of 4 and at most F16_MAX_WIDTH, which every matrix these models have is; a
 * group of fewer than four rows, and the tokens left over from the groups of
 * four, take the one-at-a-time path.
 *
 * The sums are added in a different order than a row-by-row dot product
 * would, so the result differs in the last bits, not more; the forward pass is
 * tested against the reference at cosine 0.999. */
#define F16_ROWS 4
#define F16_TOKS 4
#define F16_MAX_WIDTH 4096u

static void widen(const uint8_t *row, uint64_t k, uint64_t m, float *dst) {
    for (uint64_t i = 0; i < m; i++)
        dst[i] = quant_f16(rd_u16(row + (k + i) * 2));
}

static float dot_n(const float *w, const float *x, uint64_t m) {
    float s = 0.0f;
    uint64_t i = 0;
    for (; i + QK_K <= m; i += QK_K)
        s += dot256(w + i, x + i);
    for (; i < m; i++)
        s += w[i] * x[i];
    return s;
}

#ifdef KB_SIMD4
/* acc[r][t] = sum over m of w[r] · x[t]. */
static void f16_block(const float *w, uint64_t ws, const float *const *x, uint64_t m,
                      float acc[F16_ROWS][F16_TOKS]) {
    v4f c[F16_ROWS][F16_TOKS];
    for (int32_t r = 0; r < F16_ROWS; r++)
        for (int32_t t = 0; t < F16_TOKS; t++)
            c[r][t] = v4_zero();
    for (uint64_t k = 0; k < m; k += 4) {
        const v4f x0 = v4_load(x[0] + k), x1 = v4_load(x[1] + k);
        const v4f x2 = v4_load(x[2] + k), x3 = v4_load(x[3] + k);
        for (int32_t r = 0; r < F16_ROWS; r++) {
            const v4f wr = v4_load(w + (uint64_t)r * ws + k);
            c[r][0] = v4_madd(c[r][0], wr, x0);
            c[r][1] = v4_madd(c[r][1], wr, x1);
            c[r][2] = v4_madd(c[r][2], wr, x2);
            c[r][3] = v4_madd(c[r][3], wr, x3);
        }
    }
    for (int32_t r = 0; r < F16_ROWS; r++)
        for (int32_t t = 0; t < F16_TOKS; t++)
            acc[r][t] = v4_sum(c[r][t]);
}
#else
static void f16_block(const float *w, uint64_t ws, const float *const *x, uint64_t m,
                      float acc[F16_ROWS][F16_TOKS]) {
    for (int32_t r = 0; r < F16_ROWS; r++)
        for (int32_t t = 0; t < F16_TOKS; t++)
            acc[r][t] = dot_n(w + (uint64_t)r * ws, x[t], m);
}
#endif

/* Row r of the matrix as floats. */
static void widen_row(const MatmulJob *j, uint64_t r, float *dst) {
    if (j->t == GGML_F16) {
        widen(j->w + r * j->n * 2, 0, j->n, dst);
        return;
    }
    const size_t bsz = block_bytes(j->t);
    const uint64_t nb = j->n / QK_K;
    for (uint64_t b = 0; b < nb; b++)
        deq_block(j->t, j->w + (r * nb + b) * bsz, dst + b * QK_K);
}

/* Groups of F16_ROWS rows: [begin, end) counts groups, not rows. */
static void matmul_groups(size_t begin, size_t end, void *ud) {
    const MatmulJob *j = (const MatmulJob *)ud;
    const uint64_t n = j->n;
    float buf[F16_ROWS * F16_MAX_WIDTH];
    for (size_t g = begin; g < end; g++) {
        const uint64_t r0 = (uint64_t)g * F16_ROWS;
        const uint64_t nr = j->rows - r0 < F16_ROWS ? j->rows - r0 : F16_ROWS;
        for (uint64_t r = 0; r < nr; r++)
            widen_row(j, r0 + r, buf + r * n);
        size_t tt = 0;
        if (nr == F16_ROWS && n % 4 == 0) {
            for (; tt + F16_TOKS <= j->T; tt += F16_TOKS) {
                const float *xs[F16_TOKS];
                for (int32_t t = 0; t < F16_TOKS; t++)
                    xs[t] = j->x + (tt + (size_t)t) * n;
                float acc[F16_ROWS][F16_TOKS];
                f16_block(buf, n, xs, n, acc);
                for (int32_t t = 0; t < F16_TOKS; t++)
                    for (int32_t r = 0; r < F16_ROWS; r++)
                        j->out[(tt + (size_t)t) * j->rows + r0 + (uint64_t)r] = acc[r][t];
            }
        }
        for (; tt < j->T; tt++)
            for (uint64_t r = 0; r < nr; r++)
                j->out[tt * j->rows + r0 + r] = dot_n(buf + r * n, j->x + tt * n, n);
    }
}

bool quant_matmul(uint32_t t, const uint8_t *w, uint64_t n, uint64_t rows,
                  const float *x, size_t T, float *out) {
    const bool kquant = t == GGML_Q4_K || t == GGML_Q5_K || t == GGML_Q6_K;
    if ((t == GGML_F16 || (kquant && n % QK_K == 0)) && n <= F16_MAX_WIDTH) {
        /* Row groups are independent and each is written by one range, so
         * the result is the same for any number of threads. */
        MatmulJob job = {t, w, n, rows, x, T, out};
        plat_parallel((size_t)((rows + F16_ROWS - 1) / F16_ROWS), matmul_groups, &job);
        return true;
    }
    /* Any other format, or a row wider than a group's buffer: one vector at a
     * time, which is correct and shares nothing. */
    float *col = out;
    for (size_t tt = 0; tt < T; tt++) {
        if (!quant_matvec(t, w, n, rows, x + tt * n, col))
            return false;
        col += rows;
    }
    return true;
}

/* ---- int8 matrices ------------------------------------------------------ */

#if !defined(KB_NO_SIMD) && defined(__ARM_FEATURE_DOTPROD)
#define Q8_DOTPROD 1
#elif !defined(KB_NO_SIMD) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#define Q8_NEON 1
#elif !defined(KB_NO_SIMD) && \
    (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#define Q8_SSE2 1
#endif

/* One block's integer dot product. */
static inline int32_t dot_q8(const int8_t *a, const int8_t *b) {
#if defined(Q8_DOTPROD)
    int32x4_t s = vdotq_s32(vdupq_n_s32(0), vld1q_s8(a), vld1q_s8(b));
    s = vdotq_s32(s, vld1q_s8(a + 16), vld1q_s8(b + 16));
    return vaddvq_s32(s);
#elif defined(Q8_NEON)
    int16x8_t p0 = vmull_s8(vld1_s8(a), vld1_s8(b));
    int16x8_t p1 = vmull_s8(vld1_s8(a + 8), vld1_s8(b + 8));
    int16x8_t p2 = vmull_s8(vld1_s8(a + 16), vld1_s8(b + 16));
    int16x8_t p3 = vmull_s8(vld1_s8(a + 24), vld1_s8(b + 24));
    int32x4_t s = vpaddlq_s16(p0);
    s = vpadalq_s16(s, p1);
    s = vpadalq_s16(s, p2);
    s = vpadalq_s16(s, p3);
    return vaddvq_s32(s);
#elif defined(Q8_SSE2)
    __m128i acc = _mm_setzero_si128();
    for (int32_t k = 0; k < Q8_BLOCK; k += 16) {
        __m128i va = _mm_loadu_si128((const __m128i *)(a + k));
        __m128i vb = _mm_loadu_si128((const __m128i *)(b + k));
        /* Sign-extend each half to 16 bits: the byte doubled, then shifted. */
        __m128i al = _mm_srai_epi16(_mm_unpacklo_epi8(va, va), 8);
        __m128i ah = _mm_srai_epi16(_mm_unpackhi_epi8(va, va), 8);
        __m128i bl = _mm_srai_epi16(_mm_unpacklo_epi8(vb, vb), 8);
        __m128i bh = _mm_srai_epi16(_mm_unpackhi_epi8(vb, vb), 8);
        acc = _mm_add_epi32(acc, _mm_madd_epi16(al, bl));
        acc = _mm_add_epi32(acc, _mm_madd_epi16(ah, bh));
    }
    int32_t v[4];
    _mm_storeu_si128((__m128i *)v, acc);
    return v[0] + v[1] + v[2] + v[3];
#else
    int32_t s = 0;
    for (int32_t k = 0; k < Q8_BLOCK; k++)
        s += (int32_t)a[k] * (int32_t)b[k];
    return s;
#endif
}

/* A row of n floats as nb blocks of int8 and their scales. */
static void q8_row(const float *x, uint64_t n, int8_t *q, float *d) {
    for (uint64_t b = 0; b < n / Q8_BLOCK; b++) {
        const float *xb = x + b * Q8_BLOCK;
        float m = 0.0f;
        for (int32_t i = 0; i < Q8_BLOCK; i++) {
            float v = xb[i] < 0 ? -xb[i] : xb[i];
            if (v > m)
                m = v;
        }
        const float scale = m / 127.0f, inv = m > 0 ? 127.0f / m : 0.0f;
        d[b] = scale;
        for (int32_t i = 0; i < Q8_BLOCK; i++) {
            float r = xb[i] * inv;
            q[b * Q8_BLOCK + i] = (int8_t)(r >= 0 ? (int32_t)(r + 0.5f) : (int32_t)(r - 0.5f));
        }
    }
}

typedef struct {
    uint32_t type;
    const uint8_t *w;
    uint64_t n, row_bytes;
    Q8Matrix *m;
} Q8FromJob;

static void q8_from_rows(size_t begin, size_t end, void *ud) {
    const Q8FromJob *j = (const Q8FromJob *)ud;
    float row[4096];
    for (size_t r = begin; r < end; r++) {
        quant_row(j->type, j->w + r * j->row_bytes, j->n, row);
        q8_row(row, j->n, j->m->q + r * j->n, j->m->d + r * j->m->nb);
    }
}

bool q8_from(Arena *a, uint32_t ggml_type, const uint8_t *w, uint64_t n,
             uint64_t rows, Q8Matrix *out) {
    uint64_t row_bytes;
    if (n % Q8_BLOCK != 0 || n > 4096 || !quant_row_bytes(ggml_type, n, &row_bytes))
        return false;
    out->rows = rows;
    out->n = n;
    out->nb = n / Q8_BLOCK;
    out->q = (int8_t *)arena_alloc(a, rows * n);
    out->d = (float *)arena_alloc(a, rows * out->nb * sizeof(float));
    Q8FromJob job = {ggml_type, w, n, row_bytes, out};
    plat_parallel((size_t)rows, q8_from_rows, &job);
    return true;
}

typedef struct {
    const Q8Matrix *w;
    const float *x;
    int8_t *xq;
    float *xd;
    size_t T;
    float *out;
} Q8Job;

static void q8_quantise_inputs(size_t begin, size_t end, void *ud) {
    const Q8Job *j = (const Q8Job *)ud;
    for (size_t t = begin; t < end; t++)
        q8_row(j->x + t * j->w->n, j->w->n, j->xq + t * j->w->n, j->xd + t * j->w->nb);
}

/* Groups of four rows against every token, four tokens at a time, as the
 * float kernel does: each block of a row is loaded once for four tokens. */
static void q8_rows(size_t begin, size_t end, void *ud) {
    const Q8Job *j = (const Q8Job *)ud;
    const Q8Matrix *w = j->w;
    const uint64_t n = w->n, nb = w->nb;
    for (size_t g = begin; g < end; g++) {
        const uint64_t r0 = (uint64_t)g * 4;
        const uint64_t nr = w->rows - r0 < 4 ? w->rows - r0 : 4;
        for (size_t t0 = 0; t0 < j->T; t0 += 4) {
            const size_t nt = j->T - t0 < 4 ? j->T - t0 : 4;
#if defined(Q8_DOTPROD)
            if (nr == 4 && nt == 4) {
                /* Four tokens' blocks loaded once for four rows; each pair's
                 * block sum scaled into four float lanes, summed at the end. */
                float32x4_t acc[4][4];
                for (int32_t r = 0; r < 4; r++)
                    for (int32_t c = 0; c < 4; c++)
                        acc[r][c] = vdupq_n_f32(0.0f);
                for (uint64_t b = 0; b < nb; b++) {
                    int8x16_t xl[4], xh[4];
                    float xs[4];
                    for (int32_t c = 0; c < 4; c++) {
                        const int8_t *xq = j->xq + (t0 + (size_t)c) * n + b * Q8_BLOCK;
                        xl[c] = vld1q_s8(xq);
                        xh[c] = vld1q_s8(xq + 16);
                        xs[c] = j->xd[(t0 + (size_t)c) * nb + b];
                    }
                    for (int32_t r = 0; r < 4; r++) {
                        const int8_t *wq = w->q + (r0 + (uint64_t)r) * n + b * Q8_BLOCK;
                        const int8x16_t wl = vld1q_s8(wq), wh = vld1q_s8(wq + 16);
                        const float ws = w->d[(r0 + (uint64_t)r) * nb + b];
                        for (int32_t c = 0; c < 4; c++) {
                            int32x4_t s = vdotq_s32(vdupq_n_s32(0), wl, xl[c]);
                            s = vdotq_s32(s, wh, xh[c]);
                            acc[r][c] = vfmaq_n_f32(acc[r][c], vcvtq_f32_s32(s), ws * xs[c]);
                        }
                    }
                }
                for (int32_t r = 0; r < 4; r++)
                    for (int32_t c = 0; c < 4; c++)
                        j->out[(t0 + (size_t)c) * w->rows + r0 + (uint64_t)r] = vaddvq_f32(acc[r][c]);
                continue;
            }
#endif
            float acc[4][4] = {{0}};
            for (uint64_t b = 0; b < nb; b++) {
                for (uint64_t r = 0; r < nr; r++) {
                    const int8_t *wq = w->q + (r0 + r) * n + b * Q8_BLOCK;
                    const float wd = w->d[(r0 + r) * nb + b];
                    for (size_t t = 0; t < nt; t++) {
                        const int8_t *xq = j->xq + (t0 + t) * n + b * Q8_BLOCK;
                        acc[r][t] += (float)dot_q8(wq, xq) * wd * j->xd[(t0 + t) * nb + b];
                    }
                }
            }
            for (uint64_t r = 0; r < nr; r++)
                for (size_t t = 0; t < nt; t++)
                    j->out[(t0 + t) * w->rows + r0 + r] = acc[r][t];
        }
    }
}

void q8_matmul(const Q8Matrix *w, const float *x, size_t T, int8_t *xq, float *xd,
               float *out) {
    Q8Job job = {w, x, xq, xd, T, out};
    plat_parallel(T, q8_quantise_inputs, &job);
    plat_parallel((size_t)((w->rows + 3) / 4), q8_rows, &job);
}
