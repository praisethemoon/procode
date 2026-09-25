/* Store discovery, layout, locking, identifier allocation, blobs, appends.
 *
 * Layout, identical in both tiers (§1.6):
 *
 *   <store>/
 *     .gitignore        ignores index/ and nothing else (§1.5)
 *     sources.jsonl     append-only; the truth
 *     documents.jsonl   append-only; the truth
 *     blobs/<hash>      document text, content-addressed with sha256
 *     index/            every derived structure, all disposable
 *       model.json        chunking parameters now, the model later (§8)
 *       counters.json     next id per kind
 *       lock              exclusive writer lock, stamped with the holder pid
 *
 * HOW A TORN APPEND IS KEPT HARMLESS. A record is built whole in memory and
 * written by one synced append that includes its terminating newline. A
 * crash can therefore leave a final line without its newline, and nothing
 * else — records never contain a raw newline, because every string in them
 * goes through json_escape.
 *
 * That gives readers an exact test for "complete", and they drop an
 * unterminated tail. But dropping it is not enough on its own: if the next
 * writer simply appended, its record would be glued onto the fragment and
 * the result would be a single line that *is* terminated and is garbage —
 * corruption a reader could not detect. So a writer truncates the fragment
 * away first, under the lock, before appending anything (store_open does
 * this for for_write callers).
 *
 * The record in the fragment is not lost data: it was never acknowledged.
 * Its identifiers are lost, though — deliberately. store_reserve makes the
 * counter durable BEFORE the record is written, so a crash between the two
 * burns those ids and leaves a gap. §1.1 asks for monotonic and never
 * reused, not dense.
 */
#ifndef KB_STORE_H
#define KB_STORE_H

#include "doc.h"
#include "platform.h"

typedef enum { TIER_PROJECT, TIER_GLOBAL } Tier;
typedef enum { STORE_SOURCES, STORE_DOCUMENTS } StoreLogId;

/* The one spelling of a tier's name. Every hit, row and error that names a
 * tier goes through it, so "project" cannot become "local" in one message. */
const char *tier_name(Tier t);

typedef struct {
    Arena *a;
    Tier tier;
    char dir[KB_PATH_MAX];           /* <root>/.kb or ~/.kb */
    char sources_path[KB_PATH_MAX];
    char documents_path[KB_PATH_MAX];
    char blobs_dir[KB_PATH_MAX];
    char index_dir[KB_PATH_MAX];

    /* Loaded by store_open. For a writer this happens with the lock already
     * held, so every read-then-write decision an ingest makes — does this
     * source exist, has the content changed, which id is next — is taken
     * from state nobody else can be changing. */
    SourceList sources;
    DocList documents;

    int64_t next_source, next_document, next_chunk;
    PlatLock *lock; /* NULL for readers, which never take it */
} Store;

/* Walks up from the current directory looking for a `.kb` DIRECTORY, the
 * way git finds `.git`. Stops after probing the filesystem root. A plain
 * file named `.kb` is not a store and does not stop the walk.
 */
bool store_find_project(char *out, size_t outsz);
/* $KB_STORE when set and non-empty, else ~/.kb (§1.4). */
bool store_global_dir(char *out, size_t outsz);
/* Absolute, '/'-separated, with "." and ".." collapsed. A locator has to be
 * the same string every time the same thing is filed, or idempotency by
 * locator means nothing. */
bool store_abs_path(const char *in, char *out, size_t outsz);

/* Creates a store at dir (absolute, '/' separators). Fails if one is there. */
bool store_create(Arena *a, const char *dir, char *err, size_t errsz);

/* Opens an existing store. for_write takes the exclusive lock, repairs any
 * torn log tail, and heals the id counters; readers do none of those and
 * write nothing at all.
 *
 * On failure *code names the error from §11 where one applies —
 * "store_locked" (with the holding pid in the message), "not_found" — and
 * "internal" otherwise.
 */
bool store_open(Arena *a, Store *s, const char *dir, Tier tier, bool for_write,
                char *err, size_t errsz, const char **code);
void store_close(Store *s);

/* Reserves contiguous id ranges and makes the counters durable before
 * returning. Pass 0 for a kind that is not needed; the matching out-param
 * is then untouched. One durable write per reservation.
 */
bool store_reserve(Store *s, uint32_t nsources, uint32_t ndocuments,
                   uint32_t nchunks, int64_t *src, int64_t *doc,
                   int64_t *chunk_base, char *err, size_t errsz);

/* Appends one record line (without its newline; this adds it) and returns
 * only once the bytes are on disk. Refuses without the lock: every append
 * goes through it, and a caller that forgot should fail loudly rather than
 * race quietly.
 */
bool store_append(Store *s, StoreLogId which, const char *line, size_t len,
                  char *err, size_t errsz);

/* Content-addressed blob write. Fills hash[65] with the sha256 of the
 * content; *written reports whether any bytes were actually put on disk,
 * which is false when the blob was already there — re-ingesting unchanged
 * text writes nothing (§1.6).
 */
bool store_put_blob(Store *s, const void *data, size_t len, char hash[65],
                    bool *written, char *err, size_t errsz);
void store_blob_path(const Store *s, const char *hash, char *out,
                     size_t outsz);
bool store_get_blob(Store *s, const char *hash, char **data, size_t *len);

/* index/model.json (§8). This slice owns only the chunking parameters; the
 * model's own identity joins them when the embedder exists. Written at
 * first ingest and never silently rewritten, so a later reindex can see
 * that the parameters it would use now differ from the ones the index was
 * built with.
 */
typedef struct {
    const char *chunker;
    uint32_t chunk_tokens;
    uint32_t chunk_overlap;
    bool present;
} ChunkParams;

ChunkParams store_chunk_params(Arena *a, const Store *s);
bool store_write_chunk_params(Store *s, char *err, size_t errsz);

/* Total bytes of everything under the store, and of index/ alone. */
uint64_t store_disk_bytes(Arena *a, const Store *s, uint64_t *index_bytes);

#endif /* KB_STORE_H */
