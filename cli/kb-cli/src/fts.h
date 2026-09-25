/* index/fts.db — the keyword index, and BM25 over chunks (§1.6, §4).
 *
 * WHAT IS IN THE FILE AND WHY IT IS THAT SHAPE.
 *
 * Everything under index/ is derived and disposable (§1.6): the logs and
 * the blobs are the truth, and `kb rebuild` reconstructs this file from
 * them alone. That licence is what lets the format be a flat binary image
 * instead of a database — nothing in it has to survive a change of mind,
 * and the worst a bad file can cost is one rebuild.
 *
 *   header      magic, format version, byte-order probe, tokeniser version,
 *               the BM25 parameters, the section table, and a digest of the
 *               log records this index was built from
 *   documents   one record per document: its number, its reserved chunk
 *               range, where its chunks start in the chunk table
 *   chunks      one record per chunk: its document, its ordinal, its length
 *               in primary tokens
 *   terms       sorted by term bytes, so a lookup is a binary search
 *   postings    per term, (chunk, term frequency) pairs in ascending chunk
 *               order
 *   strings     the term bytes, concatenated
 *
 * EVERY INTEGER IS LITTLE-ENDIAN, written and read a byte at a time. The
 * file is therefore the same bytes on every machine, which matters because
 * the logs and blobs are committed and shared (§1.5) and a collaborator's
 * rebuild must be comparable to yours. The byte-order probe is belt and
 * braces: it also catches a file that is not an index at all.
 *
 * WHAT IS NOT IN THE FILE. Headings, spans and text are not stored here.
 * §4 requires a snippet with every hit, so the blob is read for the hits
 * that are actually returned anyway; re-deriving the chunk boundaries from
 * that blob costs nothing extra and makes it impossible for the index to
 * disagree with `kb get --include chunks` about where a chunk starts or
 * what heading it sits under. The index holds retrieval statistics and
 * nothing a reader can already compute.
 *
 * WHAT MAKES IT STALE. The digest is a sha256 over the format and
 * tokeniser versions, the chunking parameters, and every document's id,
 * content hash, chunk range, mime and path, in log order. Anything that
 * would change a chunk's text or its identity changes the digest.
 * `fetchedAt` deliberately does not: §2 says re-filing unchanged content
 * re-indexes nothing, so a touch must not invalidate the index. A digest
 * mismatch, a version this build does not know, a truncated file or a
 * missing one are all the same answer to the caller — `index_stale`, §11 —
 * and all have the same remedy.
 *
 * CHUNK IDS ARE NOT REDERIVED. A document's chunk range was reserved at
 * ingest and is recorded in the log (§1.1); chunk i of a document is
 * C-(chunkBase+i), across rebuilds, forever. The builder never allocates a
 * chunk outside that recorded range, so re-chunking that produces a
 * different count than the log records is reported rather than allowed to
 * hand a neighbouring document's identifier to unrelated text.
 */
#ifndef KB_FTS_H
#define KB_FTS_H

#include "chunk.h"
#include "store.h"
#include "token.h"

/* One document handed to the builder. The text is the blob; the chunk
 * range is what the log reserved for it. */
typedef struct {
    int64_t doc_num;    /* the N in D-N */
    int64_t chunk_base; /* the C the document's first chunk carries */
    uint32_t chunk_count;
    const char *text;
    size_t len;
    Lang lang;
} FtsDocInput;

typedef struct {
    int64_t doc_num;
    int64_t chunk_base;
    uint32_t chunk_count; /* chunks actually indexed */
    uint32_t first_chunk; /* index of its first chunk record */
} FtsDoc;

typedef struct {
    uint32_t doc_index;
    uint32_t ordinal;
    uint32_t length; /* primary tokens; the |D| BM25 normalises against */
} FtsChunk;

typedef struct {
    const char *text; /* into the string blob; NOT NUL-terminated */
    uint32_t len;
    uint32_t post_off; /* byte offset into the postings block */
    uint32_t df;
} FtsTerm;

typedef struct {
    uint32_t version, tokenizer;
    uint32_t k1_milli, b_milli;
    uint64_t total_len; /* Σ chunk length, for avgdl */
    char digest[65];

    FtsDoc *docs;
    uint32_t doc_count;
    FtsChunk *chunks;
    uint32_t chunk_count;
    FtsTerm *terms;
    uint32_t term_count;
    const uint8_t *post;
    size_t post_len;
    const char *strings;
    size_t strings_len;
    uint64_t file_bytes;
} FtsIndex;

/* What the build produced, so a caller can report it without parsing back
 * the image it just wrote. `mismatched` counts documents whose re-chunking
 * did not reproduce the count the log records — those are clamped to the
 * reserved range, never past it. */
typedef struct {
    uint32_t chunks;
    uint32_t terms;
    uint32_t mismatched;
} FtsBuildStats;

/* Builds the serialized image. Returns NULL on failure. stats may be NULL. */
char *fts_build(Arena *a, const FtsDocInput *in, size_t n, size_t target_bytes,
                size_t overlap_bytes, const char *digest, size_t *out_len,
                FtsBuildStats *stats, char *err, size_t errsz);

/* Validates and decodes an image. Every offset and length in the file is
 * checked against the file's own size before anything is read through it,
 * so a truncated or foreign file is an error and never a wild read.
 * *code is "index_stale" for anything wrong with the file itself. */
bool fts_open(Arena *a, const char *data, size_t len, FtsIndex *out,
              const char **code, char *err, size_t errsz);
bool fts_load(Arena *a, const char *path, FtsIndex *out, const char **code,
              char *err, size_t errsz);

/* index/fts.db of this store, checked against the store's own logs.
 * Returns false with *code == "index_stale" when the file is missing, from
 * another version, damaged, or no longer describes these logs. */
bool fts_open_store(Arena *a, const Store *s, FtsIndex *out, const char **code,
                    char *err, size_t errsz);
void fts_path(const Store *s, char *out, size_t outsz);

/* The digest of what an index over this store would have to be built from. */
void fts_store_digest(const Store *s, ChunkParams cp, char out[65]);

typedef struct {
    uint32_t chunk_index;
    double score;
} FtsHit;

/* Called once per document (the answer is memoized) to decide whether its
 * chunks may appear at all. The filters in §4 — collection, source, mime,
 * since — are properties of the document, and the index does not hold them;
 * the caller that has the log does. */
typedef bool (*FtsDocFilter)(uint32_t doc_index, void *ud);

/* Okapi BM25 over chunks, highest first, ties broken by chunk index so the
 * order is a function of the index alone. min_score is applied before the
 * limit, so `k` never has to be spent on hits the caller already excluded. */
size_t fts_search(Arena *a, const FtsIndex *ix, const TermList *q,
                  double min_score, FtsDocFilter filter, void *ud,
                  size_t limit, FtsHit **out);

/* Exposed for tests and for anything that has to reproduce a score. */
double fts_idf(const FtsIndex *ix, uint32_t df);
double fts_term_score(const FtsIndex *ix, uint32_t df, uint32_t tf,
                      uint32_t chunk_len);

#endif /* KB_FTS_H */
