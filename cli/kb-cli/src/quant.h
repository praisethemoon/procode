/* Dequantisation of the block formats this model file actually uses (§8).
 *
 * WHY THERE IS ANY OF THIS. The weights on disk are k-quants: values are
 * stored in SUPER-BLOCKS of 256, each split into eight sub-blocks of 32, and
 * each sub-block has its own scale and its own minimum. The two per-sub-block
 * numbers are themselves quantised to six bits and are reconstructed by two
 * half-precision floats that belong to the whole super-block. So getting one
 * weight back costs two multiplies and a subtract, and getting the SCALE
 * costs a six-bit unpack out of a twelve-byte field where four of the eight
 * scales are split across two bytes. That unpack is the single most
 * error-prone thing in this file, and it is silent when it is wrong: a scale
 * read from the wrong nibble still produces a plausible number.
 *
 * NOTHING IS EXPANDED TO FLOAT AHEAD OF TIME. Dequantised, this model's
 * weights are 453 MB; quantised they are 84 MB and stay in the page cache
 * shared with every other process that has the file open. A matrix-vector
 * product is memory-bound long before it is compute-bound, so unpacking a
 * super-block into 1 KB of stack and consuming it immediately is both
 * smaller and faster than reading four bytes per weight would be. The
 * matvec below is the only place that touches quantised bytes.
 *
 * BLOCK SIZES ARE NOT ASSUMED, THEY ARE CHECKED. 144, 176 and 210 bytes for
 * Q4_K, Q5_K and Q6_K are confirmed against the model file itself: the
 * distance between consecutive tensor offsets in the directory equals rows ×
 * blocks-per-row × block size for every tensor in it.
 */
#ifndef KB_QUANT_H
#define KB_QUANT_H

#include "arena.h"
#include "kb.h"

/* Every k-quant in this family packs 256 values per super-block. */
#define QK_K 256

#define Q4_K_BYTES 144u
#define Q5_K_BYTES 176u
#define Q6_K_BYTES 210u

/* IEEE-754 binary16 to binary32. Subnormals and infinities included: a
 * super-block scale that came out of a quantiser as a subnormal is a real
 * number the weights depend on, not a rounding curiosity. */
float quant_f16(uint16_t h);

/* Bytes one row of `n` elements occupies, or false when this build cannot
 * read the type at all or `n` is not a whole number of blocks. Both are
 * refusals rather than approximations: a row length that is not a multiple
 * of 256 means the file disagrees with the format about where the next row
 * begins. */
bool quant_row_bytes(uint32_t ggml_type, uint64_t n, uint64_t *out);

/* Expands one row of `n` elements into out[n]. Used for the small tensors
 * (norm weights, a token's embedding) where the whole row is wanted. */
bool quant_row(uint32_t ggml_type, const uint8_t *src, uint64_t n, float *out);

/* out[j] = Σ_i W[j][i] · x[i] for j in [0, rows), where W is `rows` rows of
 * `n` elements each, laid out contiguously as GGUF stores them. This is the
 * whole of the forward pass's arithmetic.
 *
 * The accumulation order is fixed — four running sums, one per lane, joined
 * at the end of every super-block — so that the scalar and SIMD paths do the
 * same additions in the same order and cannot disagree about the last bit.
 */
bool quant_matvec(uint32_t ggml_type, const uint8_t *w, uint64_t n,
                  uint64_t rows, const float *x, float *out);

/* The same product for T input vectors at once: out[t*rows + j] = Σ_i
 * W[j][i] · x[t*n + i]. Each super-block of W is dequantised ONCE and used for
 * all T vectors, where T calls to quant_matvec would dequantise it T times —
 * for a 400-token chunk, the difference between unpacking the weights once
 * and four hundred times. Every output is accumulated in exactly the order
 * quant_matvec uses, so the two agree to the last bit. */
bool quant_matmul(uint32_t ggml_type, const uint8_t *w, uint64_t n,
                  uint64_t rows, const float *x, size_t T, float *out);

/* True when this build compiled the SIMD matvec in. Reported by `kb status`
 * so a timing can be read against what actually ran. */
bool quant_simd(void);

/* ---- int8 matrices ------------------------------------------------------
 *
 * The same product in 8-bit integers: each row of W is cut into blocks of
 * Q8_BLOCK values, and each block kept as int8 with one float scale (its
 * largest magnitude over 127); the input vectors are quantised the same way
 * on each call, and a block's dot product is an integer dot product scaled
 * by the two blocks' scales. The CPU does four times as many 8-bit
 * multiply-adds per instruction as float ones, where it has int8 dot product
 * instructions (ARM's since 8.2, which every Apple and most current ARM
 * chips have); elsewhere NEON's widening multiply, SSE2's multiply-add of
 * 16-bit pairs, or plain C compute the same integers.
 *
 * The result is not the float one: relative error around 5e-3 per product,
 * well under what changes a nearest neighbour (tests/unit/test_modernbert.c
 * measures the embeddings against the reference). kb uses it where it embeds
 * many texts at once (filing, `kb embed`, reranking) and keeps the float
 * path for a search's one query. */
#define Q8_BLOCK 32

typedef struct {
    uint64_t rows, n, nb; /* nb = n / Q8_BLOCK */
    float *d;             /* rows x nb scales */
    int8_t *q;            /* rows x n values */
} Q8Matrix;

/* W (any type quant_row reads) as an int8 matrix. n must be a multiple of
 * Q8_BLOCK and at most 4096. */
bool q8_from(Arena *a, uint32_t ggml_type, const uint8_t *w, uint64_t n,
             uint64_t rows, Q8Matrix *out);

/* out[t*rows + j] = sum_i W[j][i] * x[t*n + i], in int8. xq and xd are the
 * caller's scratch: T*n bytes and T*nb floats. */
void q8_matmul(const Q8Matrix *w, const float *x, size_t T, int8_t *xq, float *xd,
               float *out);

#endif /* KB_QUANT_H */
