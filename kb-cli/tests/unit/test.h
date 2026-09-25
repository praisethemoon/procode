/* Minimal unit testing API: counters, named test blocks, assertion macros.
 * Failures print file:line and keep going so one run reports everything.
 */
#ifndef KB_TEST_H
#define KB_TEST_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int32_t t_pass;
extern int32_t t_fail;
extern const char *t_current;

void t_begin(const char *name);

#define ASSERT_TRUE(cond)                                                      \
    do {                                                                       \
        if (cond) {                                                            \
            t_pass++;                                                          \
        } else {                                                               \
            t_fail++;                                                          \
            printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, t_current,    \
                   #cond);                                                     \
        }                                                                      \
    } while (0)

#define ASSERT_EQ_I(actual, expected)                                          \
    do {                                                                       \
        int64_t a_ = (int64_t)(actual);                                        \
        int64_t e_ = (int64_t)(expected);                                      \
        if (a_ == e_) {                                                        \
            t_pass++;                                                          \
        } else {                                                               \
            t_fail++;                                                          \
            printf("  FAIL %s:%d [%s] %s == %s: got %" PRId64                  \
                   ", want %" PRId64 "\n",                                     \
                   __FILE__, __LINE__, t_current, #actual, #expected, a_, e_); \
        }                                                                      \
    } while (0)

/* For scores checked against a value computed by hand. The tolerance is an
 * argument because a test that states one is a test that decided what
 * "matches" means rather than hoping. */
#define ASSERT_NEAR(actual, expected, tol)                                     \
    do {                                                                       \
        double a_ = (double)(actual);                                          \
        double e_ = (double)(expected);                                        \
        double d_ = a_ > e_ ? a_ - e_ : e_ - a_;                               \
        if (d_ <= (double)(tol)) {                                             \
            t_pass++;                                                          \
        } else {                                                               \
            t_fail++;                                                          \
            printf("  FAIL %s:%d [%s] %s: got %.10f, want %.10f (±%g)\n",      \
                   __FILE__, __LINE__, t_current, #actual, a_, e_,             \
                   (double)(tol));                                             \
        }                                                                      \
    } while (0)

#define ASSERT_EQ_S(actual, expected)                                          \
    do {                                                                       \
        const char *a_ = (actual);                                             \
        const char *e_ = (expected);                                           \
        if (a_ && e_ && strcmp(a_, e_) == 0) {                                 \
            t_pass++;                                                          \
        } else {                                                               \
            t_fail++;                                                          \
            printf("  FAIL %s:%d [%s] %s: got \"%s\", want \"%s\"\n",          \
                   __FILE__, __LINE__, t_current, #actual,                     \
                   a_ ? a_ : "(null)", e_ ? e_ : "(null)");                    \
        }                                                                      \
    } while (0)

#endif /* KB_TEST_H */
