#include "rank.h"

typedef struct {
    uint64_t key;
    void *item;
    uint32_t tag;
    uint32_t list;
    uint32_t rank;
    double part; /* score_fuse: this list's weighted, normalised score */
} Flat;

/* Group by key, and within a key keep the earliest list first so the merge
 * below can take the winning item and list without a second pass. */
static int flat_cmp(const void *x, const void *y) {
    const Flat *a = (const Flat *)x, *b = (const Flat *)y;
    if (a->key != b->key)
        return a->key < b->key ? -1 : 1;
    if (a->list != b->list)
        return a->list < b->list ? -1 : 1;
    return a->rank < b->rank ? -1 : (a->rank > b->rank ? 1 : 0);
}

static int result_cmp(const void *x, const void *y) {
    const RankResult *a = (const RankResult *)x, *b = (const RankResult *)y;
    if (a->score > b->score)
        return -1;
    if (a->score < b->score)
        return 1;
    if (a->list != b->list)
        return a->list < b->list ? -1 : 1;
    if (a->rank != b->rank)
        return a->rank < b->rank ? -1 : 1;
    return a->key < b->key ? -1 : (a->key > b->key ? 1 : 0);
}

size_t rrf_fuse(Arena *a, const RankList *lists, size_t nlists, uint32_t k,
                size_t limit, RankResult **out) {
    *out = NULL;
    size_t total = 0;
    for (size_t i = 0; i < nlists; i++)
        total += lists[i].n;
    if (total == 0 || limit == 0)
        return 0;

    Flat *flat = (Flat *)arena_alloc(a, total * sizeof(Flat));
    size_t n = 0;
    for (size_t i = 0; i < nlists; i++) {
        for (size_t r = 0; r < lists[i].n; r++) {
            flat[n].key = lists[i].v[r].key;
            flat[n].item = lists[i].v[r].item;
            flat[n].tag = lists[i].tag;
            flat[n].list = (uint32_t)i;
            flat[n].rank = (uint32_t)r;
            n++;
        }
    }
    qsort(flat, n, sizeof(Flat), flat_cmp);

    RankResult *res = (RankResult *)arena_alloc(a, n * sizeof(RankResult));
    size_t nres = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        RankResult r;
        memset(&r, 0, sizeof r);
        r.key = flat[i].key;
        r.item = flat[i].item;
        r.list = flat[i].list;
        r.rank = flat[i].rank;
        while (j < n && flat[j].key == r.key) {
            /* Rank is 0-based here and 1-based in the formula: the best
             * answer of a list is rank 1, not rank 0, or it would be worth
             * 1/k however many lists agreed on it. */
            r.score += 1.0 / ((double)k + (double)flat[j].rank + 1.0);
            r.tags |= flat[j].tag;
            j++;
        }
        res[nres++] = r;
        i = j;
    }
    qsort(res, nres, sizeof(RankResult), result_cmp);
    if (nres > limit)
        nres = limit;
    *out = res;
    return nres;
}

size_t score_fuse(Arena *a, const RankList *lists, size_t nlists, const double *weight,
                  size_t limit, RankResult **out) {
    *out = NULL;
    size_t total = 0;
    for (size_t i = 0; i < nlists; i++)
        total += lists[i].n;
    if (total == 0 || limit == 0)
        return 0;
    Flat *flat = (Flat *)arena_alloc(a, total * sizeof(Flat));
    size_t n = 0;
    for (size_t i = 0; i < nlists; i++) {
        double hi = -1e300, lo = 1e300;
        for (size_t r = 0; r < lists[i].n; r++) {
            if (lists[i].v[r].score > hi)
                hi = lists[i].v[r].score;
            if (lists[i].v[r].score < lo)
                lo = lists[i].v[r].score;
        }
        const double span = hi - lo;
        for (size_t r = 0; r < lists[i].n; r++) {
            flat[n].key = lists[i].v[r].key;
            flat[n].item = lists[i].v[r].item;
            flat[n].tag = lists[i].tag;
            flat[n].list = (uint32_t)i;
            flat[n].rank = (uint32_t)r;
            /* A list whose scores are all equal says nothing about order:
             * every entry counts as its best. */
            const double norm = span > 0 ? (lists[i].v[r].score - lo) / span : 1.0;
            flat[n].part = weight[i] * norm;
            n++;
        }
    }
    qsort(flat, n, sizeof(Flat), flat_cmp);
    RankResult *res = (RankResult *)arena_alloc(a, n * sizeof(RankResult));
    size_t nres = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        RankResult r;
        memset(&r, 0, sizeof r);
        r.key = flat[i].key;
        r.item = flat[i].item;
        r.list = flat[i].list;
        r.rank = flat[i].rank;
        while (j < n && flat[j].key == r.key) {
            r.score += flat[j].part;
            r.tags |= flat[j].tag;
            j++;
        }
        res[nres++] = r;
        i = j;
    }
    qsort(res, nres, sizeof(RankResult), result_cmp);
    if (nres > limit)
        nres = limit;
    *out = res;
    return nres;
}
