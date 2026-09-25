/* Reciprocal rank fusion (§1.4, §4).
 *
 * Two places in this design merge result lists, and both merge by RANK
 * rather than by score.
 *
 *   §1.4, now: the project and global tiers are searched separately and
 *   their results combined. The tiers are separate corpora — different
 *   document counts, different average chunk lengths, and later different
 *   embedding models — so their BM25 scores are not on one scale. 12.4 in a
 *   nine-document project store and 12.4 in a four-thousand-document global
 *   store are not the same claim, and comparing them directly would let the
 *   larger store quietly dominate every search.
 *
 *   §4, next: keyword and semantic retrieval are fused. A cosine similarity
 *   and a BM25 score have no common unit at all.
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
 * ITEMS ARE JOINED BY `key`. Today each chunk appears in exactly one list —
 * a chunk lives in one tier, and duplicates across tiers are removed before
 * fusion (§1.4) — so the sum has one term and fusion is an interleave. That
 * is the honest consequence of ranking by rank. When the semantic path
 * arrives, the same chunk will appear in two lists with the same key, the
 * two contributions will add, and a chunk found by both will rise above one
 * found by either. Nothing here has to change for that; `tags` is already
 * the bitmask that §4's `matched` is printed from.
 *
 * TIES ARE NOT BROKEN BY SCORE. Two lists' rank-1 entries tie, and the
 * tie-break is the order the lists were handed in — project before global,
 * matching §1.4's rule that a document in both tiers is attributed to the
 * project — then rank, then key. Reaching for the underlying scores to
 * break the tie would reintroduce exactly the cross-scale comparison this
 * whole mechanism exists to avoid.
 */
#ifndef KB_RANK_H
#define KB_RANK_H

#include "arena.h"

typedef struct {
    uint64_t key; /* two entries with the same key are the same item */
    void *item;
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

#endif /* KB_RANK_H */
