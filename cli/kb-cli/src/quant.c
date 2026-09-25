#include "quant.h"

#include "gguf.h"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define QUANT_NEON 1
#endif

bool quant_simd(void) {
#ifdef QUANT_NEON
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
#ifdef QUANT_NEON
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = vdupq_n_f32(0.0f);
    float32x4_t a2 = vdupq_n_f32(0.0f), a3 = vdupq_n_f32(0.0f);
    for (int32_t k = 0; k < QK_K; k += 16) {
        a0 = vfmaq_f32(a0, vld1q_f32(w + k), vld1q_f32(x + k));
        a1 = vfmaq_f32(a1, vld1q_f32(w + k + 4), vld1q_f32(x + k + 4));
        a2 = vfmaq_f32(a2, vld1q_f32(w + k + 8), vld1q_f32(x + k + 8));
        a3 = vfmaq_f32(a3, vld1q_f32(w + k + 12), vld1q_f32(x + k + 12));
    }
    float32x4_t s = vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3));
    return vaddvq_f32(s);
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
        /* Neither appears as a weight matrix in this family of models — the
         * norms are vectors, not matrices — but the path exists so an f32
         * model is not a special case somewhere else. */
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
