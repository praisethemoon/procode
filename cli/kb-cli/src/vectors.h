/* index/vectors.bin (§8): one vector per chunk, stored int8 and scanned flat.
 *
 * KEYED BY CHUNK ID, AND THAT IS WHAT MAKES IT CHEAP TO KEEP. A chunk id names
 * one passage of one version of one document forever (§1.1): a re-filed
 * document gets a fresh range, a forgotten one leaves its ids unused. So a
 * vector computed for C-812 under a given model is right for as long as that
 * model is — keeping the file current after an ingest means embedding the new
 * ids and dropping the dead ones, never re-embedding the rest. Only a change
 * of model (`kb reindex`) throws them all away.
 *
 * THE FILE.
 *
 *     "KBVEC001"                          8 bytes
 *     dim                                 u32
 *     reserved                            u32
 *     count                               u64
 *     model fingerprint, hex              64 bytes
 *     count × { chunk id i64, scale f32, int8[dim] }, ascending by id
 *
 * Little-endian. Each vector is quantised symmetrically, scale = max|v|/127,
 * so v ≈ scale × q; the vectors are unit-length before quantisation, which
 * makes scale × (q · query) an approximate cosine against a float query.
 * Derived and disposable like everything under index/: `kb rebuild` makes it
 * again from the logs, the blobs and the model.
 */
#ifndef KB_VECTORS_H
#define KB_VECTORS_H

#include "embed.h"
#include "store.h"

typedef struct {
    uint32_t dim;
    size_t n;
    int64_t *ids;   /* ascending */
    float *scales;
    int8_t *q;      /* n × dim */
    char fingerprint[65];
} VecSet;

/* Reads index/vectors.bin. A missing or unreadable file is an empty set with
 * no fingerprint — the same answer as "nothing embedded yet". */
/* What vectors.bin is stamped with: the model's fingerprint and the chunker's
 * version. A vector is the embedding of a chunk's text under its header line
 * (chunk_header), so a chunker that changes the header changes every vector
 * as surely as a new model does, even where the chunk ids stay the same. */
void vec_fingerprint(const ModelParams *m, char out[65]);

void vec_load(Arena *a, const Store *s, VecSet *out);
bool vec_save(Arena *a, const Store *s, const VecSet *v, char *err,
              size_t errsz);

/* Index of `id` in the set, or -1. */
int64_t vec_find(const VecSet *v, int64_t id);

/* scale × (q · x): the similarity of stored vector i to a float query. */
float vec_score(const VecSet *v, size_t i, const float *x);

/* How many of the store's live chunks have no vector in `v`. */
size_t vec_missing(const Store *s, const VecSet *v);

typedef struct {
    size_t kept;     /* vectors carried over */
    size_t embedded; /* chunks embedded now */
    size_t dropped;  /* vectors for chunks no longer in the store */
    size_t truncated;/* chunks longer than the model's context */
    size_t skipped;  /* chunks not worth embedding (chunk_embeddable) */
    size_t pending;  /* chunks left for later: the time budget ran out */
} VecSync;

/* The time an embedding pass may take, in seconds, for vec_sync and
 * vec_update. VEC_NO_BUDGET embeds everything; 0 embeds nothing now. A pass
 * that runs out leaves the rest `pending`: their chunks are searchable by
 * keyword already, a vector search uses the vectors that exist and says how
 * many are missing, and `kb embed` (or the next add, rebuild or reindex)
 * finishes them. */
#define VEC_NO_BUDGET (-1.0)

/* Brings vectors.bin in line with the store under an open model: embeds every
 * live chunk that has no vector, drops vectors for chunks that are gone, and
 * with `all` (or a file written under another fingerprint) starts from
 * nothing. `progress` prints a line per chunk to stderr when set. Needs the
 * write lock. */
bool vec_sync(Arena *a, Store *s, Embedder *e, bool all, bool progress,
              double budget_s, VecSync *stats, char *err, size_t errsz);

/* vec_sync when it can run: the store records a model and the one in
 * ~/.kb/models is that model. Otherwise nothing is written and *ran is false —
 * a keyword-only store, a missing model or a mismatch leaves the vectors as
 * they were, and `kb status` and a vector search say why. */
bool vec_update(Arena *a, Store *s, bool all, bool progress, double budget_s,
                VecSync *stats, bool *ran, char *err, size_t errsz);

#endif
