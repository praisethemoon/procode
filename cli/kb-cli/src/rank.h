/* Reciprocal rank fusion (§4).
 *
 * Keyword and semantic retrieval are fused by RANK rather than by score: a
 * cosine similarity and a BM25 score have no common unit at all.
 *
 * Rank is the one thing every list agrees on: "this was my best answer" means
 * the same in all of them. So each list contributes 1/(k + rank) and the
 * contributions are summed:
 *
 *     score(item) = Σ over lists holding it of  1 / (k + rank_in_that_list)
 *
 * k = 60 is the constant from the original paper; its job is to flatten the
 * difference between rank 1 and rank 2 enough that agreement between lists
 * matters more than the exact position within one.
 *
 * ITEMS ARE JOINED BY `key`. Today there is one list, the keyword one, so
 * the sum has one term and fusion preserves its order. When the semantic path
 * arrives, the same chunk will appear in two lists with the same key, the
 * two contributions will add, and a chunk found by both will rise above one
 * found by either. Nothing here has to change for that; `tags` is already
 * the bitmask that §4's `matched` is printed from.
 *
 * TIES ARE NOT BROKEN BY SCORE. Two lists' rank-1 entries tie, and the
 * tie-break is the order the lists were handed in, then rank, then key. Reaching for the underlying scores to
 * break the tie would reintroduce exactly the cross-scale comparison this
 * whole mechanism exists to avoid.
 */
#ifndef KB_RANK_H
#define KB_RANK_H

#include "arena.h"

typedef struct {
    uint64_t key; /* two entries with the same key are the same item */
    void *item;
    double score; /* the list's own score, for score_fuse; rrf_fuse ignores it */
} RankEntry;

typedef struct {
    const RankEntry *v; /* best first */
    size_t n;
    uint32_t tag; /* which retrieval path this list is */
} RankList;

typedef struct {
    uint64_t key;
    void *item;    /* from the earliest list that produced it */
    double score;  /* the reciprocal rank sum */
    uint32_t tags; /* OR of the tags of every list that produced it */
    uint32_t list; /* earliest list index that produced it */
    uint32_t rank; /* its rank there, 0-based */
} RankResult;

/* Tags are a bitmask so `matched` can name more than one path (§4). */
#define RANK_TAG_KEYWORD 1u
#define RANK_TAG_SEMANTIC 2u

size_t rrf_fuse(Arena *a, const RankList *lists, size_t nlists, uint32_t k,
                size_t limit, RankResult **out);

/* SCORE FUSION, the default since the benchmark (bench/tune_fusion.py) showed
 * it ahead of RRF: each list's scores are min-max normalised over that list —
 * the best entry 1, the worst 0, so a cosine and a BM25 score meet on a common
 * scale for this one query — and weighted, and an item's score is the sum of
 * its weighted scores over the lists that hold it. An item a list does not
 * hold scores 0 there. RRF discards how far ahead the first answer is; this
 * keeps it. Ties break as in rrf_fuse. `weight[i]` is list i's weight. */
size_t score_fuse(Arena *a, const RankList *lists, size_t nlists, const double *weight,
                  size_t limit, RankResult **out);

#endif /* KB_RANK_H */
