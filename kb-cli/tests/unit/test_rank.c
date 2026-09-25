#include "rank.h"
#include "test.h"

/* Reciprocal rank fusion. With k = 60, rank 1 is worth 1/61 = 0.016393,
 * rank 2 is 1/62 = 0.016129, rank 3 is 1/63 = 0.015873. An item in two
 * lists at rank 1 is worth 2/61 = 0.032787, which beats anything a single
 * list can offer — that is the whole point of the mechanism. */

#define TOL 1e-12

static RankEntry *entries(Arena *a, const uint64_t *keys, size_t n) {
    RankEntry *e = (RankEntry *)arena_alloc0(a, n * sizeof(RankEntry));
    for (size_t i = 0; i < n; i++) {
        e[i].key = keys[i];
        /* The payload is the caller's; here a pointer to its own entry, so
         * "which list did this come from" is checkable. */
        e[i].item = &e[i];
    }
    return e;
}

static void test_single(Arena *a) {
    t_begin("rank: one list keeps its own order");
    static const uint64_t k[] = {10, 20, 30};
    RankList l;
    l.v = entries(a, k, 3);
    l.n = 3;
    l.tag = RANK_TAG_KEYWORD;
    RankResult *out;
    size_t n = rrf_fuse(a, &l, 1, KB_RRF_K, 100, &out);
    ASSERT_EQ_I(n, 3);
    ASSERT_EQ_I(out[0].key, 10);
    ASSERT_EQ_I(out[1].key, 20);
    ASSERT_EQ_I(out[2].key, 30);
    ASSERT_NEAR(out[0].score, 1.0 / 61.0, TOL);
    ASSERT_NEAR(out[1].score, 1.0 / 62.0, TOL);
    ASSERT_EQ_I(out[0].tags, RANK_TAG_KEYWORD);
}

static void test_interleave(Arena *a) {
    t_begin("rank: two disjoint lists interleave by rank, not by score");
    /* This is §1.4's rule made visible. The two tiers hold no item in
     * common, so every item is worth 1/(60+rank) and the merge is an
     * interleave — whatever the underlying BM25 numbers were. A merge that
     * sorted by score would produce a different order here, and does. */
    static const uint64_t p[] = {1, 2, 3};
    static const uint64_t g[] = {11, 12};
    RankList l[2];
    l[0].v = entries(a, p, 3);
    l[0].n = 3;
    l[0].tag = RANK_TAG_KEYWORD;
    l[1].v = entries(a, g, 2);
    l[1].n = 2;
    l[1].tag = RANK_TAG_KEYWORD;
    RankResult *out;
    size_t n = rrf_fuse(a, l, 2, KB_RRF_K, 100, &out);
    ASSERT_EQ_I(n, 5);
    ASSERT_EQ_I(out[0].key, 1);
    ASSERT_EQ_I(out[1].key, 11);
    ASSERT_EQ_I(out[2].key, 2);
    ASSERT_EQ_I(out[3].key, 12);
    ASSERT_EQ_I(out[4].key, 3);

    t_begin("rank: a tie goes to the list that was handed in first");
    /* §1.4 attributes a document present in both tiers to the project, and
     * the tiers are passed project first, so ties resolve the same way. */
    ASSERT_NEAR(out[0].score, out[1].score, TOL);
    ASSERT_EQ_I(out[0].list, 0);
    ASSERT_EQ_I(out[1].list, 1);
}

static void test_agreement(Arena *a) {
    t_begin("rank: an item two lists agree on outranks one either found");
    /* Nothing produces this case yet — a chunk lives in one tier — but it
     * is what the semantic path will produce next, and the seam has to be
     * right before it arrives. */
    static const uint64_t x[] = {7, 1, 2};
    static const uint64_t y[] = {9, 7};
    RankList l[2];
    l[0].v = entries(a, x, 3);
    l[0].n = 3;
    l[0].tag = RANK_TAG_KEYWORD;
    l[1].v = entries(a, y, 2);
    l[1].n = 2;
    l[1].tag = RANK_TAG_SEMANTIC;
    RankResult *out;
    size_t n = rrf_fuse(a, l, 2, KB_RRF_K, 100, &out);
    ASSERT_EQ_I(n, 4); /* 7 appears twice and is returned once */
    ASSERT_EQ_I(out[0].key, 7);
    ASSERT_NEAR(out[0].score, 1.0 / 61.0 + 1.0 / 62.0, TOL);
    ASSERT_TRUE(out[0].score > out[1].score);

    t_begin("rank: tags name every path that produced the item");
    ASSERT_EQ_I(out[0].tags, RANK_TAG_KEYWORD | RANK_TAG_SEMANTIC);
    /* 9 is the semantic list's best and beats 1, which is only the keyword
     * list's second: 1/61 against 1/62. */
    ASSERT_EQ_I(out[1].key, 9);
    ASSERT_EQ_I(out[1].tags, RANK_TAG_SEMANTIC);
    ASSERT_EQ_I(out[2].key, 1);
    ASSERT_EQ_I(out[2].tags, RANK_TAG_KEYWORD);
    ASSERT_EQ_I(out[3].key, 2);

    t_begin("rank: the payload comes from the earliest list holding it");
    ASSERT_TRUE(out[0].item == (void *)&l[0].v[0]);
}

static void test_limit(Arena *a) {
    t_begin("rank: the limit truncates the fused list, keeping the best");
    static const uint64_t p[] = {1, 2, 3};
    static const uint64_t g[] = {11, 12, 13};
    RankList l[2];
    l[0].v = entries(a, p, 3);
    l[0].n = 3;
    l[0].tag = RANK_TAG_KEYWORD;
    l[1].v = entries(a, g, 3);
    l[1].n = 3;
    l[1].tag = RANK_TAG_KEYWORD;
    RankResult *out;
    size_t n = rrf_fuse(a, l, 2, KB_RRF_K, 3, &out);
    ASSERT_EQ_I(n, 3);
    ASSERT_EQ_I(out[0].key, 1);
    ASSERT_EQ_I(out[1].key, 11);
    ASSERT_EQ_I(out[2].key, 2);

    t_begin("rank: fusing nothing returns nothing");
    l[0].n = 0;
    l[1].n = 0;
    ASSERT_EQ_I(rrf_fuse(a, l, 2, KB_RRF_K, 10, &out), 0);
    ASSERT_EQ_I(rrf_fuse(a, l, 0, KB_RRF_K, 10, &out), 0);
}

void test_rank(void) {
    Arena *a = arena_new(1 << 14);
    test_single(a);
    test_interleave(a);
    test_agreement(a);
    test_limit(a);
    arena_free(a);
}
