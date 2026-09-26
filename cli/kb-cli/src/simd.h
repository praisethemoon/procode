/* Four floats at a time, on whichever instruction set the target is sure to
 * have: NEON on every AArch64 CPU, SSE2 on every x86-64 CPU. The kernels in
 * quant.c and embed.c are written once against these few operations, so a
 * speed-up made for one architecture is a speed-up on the other.
 *
 * CHOSEN AT COMPILE TIME, and only from what the target guarantees: nothing
 * here needs a CPU check, so no machine can meet an instruction it lacks.
 * Wider sets (AVX2, AVX-512) are not guaranteed by x86-64 and would need a
 * runtime check; they are not used.
 *
 * KB_SIMD4 is defined when one of the two is in use. Without it — another
 * architecture, or a build with KB_NO_SIMD (`make SIMD=0`) — callers take
 * their plain C loops, which compute the same values up to rounding.
 */
#ifndef KB_SIMD_H
#define KB_SIMD_H

#if !defined(KB_NO_SIMD) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define KB_SIMD4 "neon"
typedef float32x4_t v4f;
static inline v4f v4_zero(void) { return vdupq_n_f32(0.0f); }
static inline v4f v4_set1(float x) { return vdupq_n_f32(x); }
static inline v4f v4_load(const float *p) { return vld1q_f32(p); }
static inline void v4_store(float *p, v4f a) { vst1q_f32(p, a); }
static inline v4f v4_add(v4f a, v4f b) { return vaddq_f32(a, b); }
/* acc + a·b */
static inline v4f v4_madd(v4f acc, v4f a, v4f b) { return vfmaq_f32(acc, a, b); }
static inline float v4_sum(v4f a) { return vaddvq_f32(a); }

#elif !defined(KB_NO_SIMD) && \
    (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#define KB_SIMD4 "sse2"
typedef __m128 v4f;
static inline v4f v4_zero(void) { return _mm_setzero_ps(); }
static inline v4f v4_set1(float x) { return _mm_set1_ps(x); }
static inline v4f v4_load(const float *p) { return _mm_loadu_ps(p); }
static inline void v4_store(float *p, v4f a) { _mm_storeu_ps(p, a); }
static inline v4f v4_add(v4f a, v4f b) { return _mm_add_ps(a, b); }
/* acc + a·b; SSE2 has no fused multiply-add, so it rounds twice. */
static inline v4f v4_madd(v4f acc, v4f a, v4f b) { return _mm_add_ps(acc, _mm_mul_ps(a, b)); }
static inline float v4_sum(v4f a) {
    v4f s = _mm_add_ps(a, _mm_movehl_ps(a, a));
    s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
}
#endif

#endif /* KB_SIMD_H */
