/* Store discovery, layout, locking, identifier allocation, blobs, appends.
 *
 * Layout (§1.6):
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

typedef enum { STORE_SOURCES, STORE_DOCUMENTS } StoreLogId;

typedef struct {
    Arena *a;
    char dir[KB_PATH_MAX];           /* <root>/.kb */
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
 *
 * This is the only way a store is found. There is no store in the home
 * directory and no environment variable naming one: the knowledge base
 * belongs to the workspace it sits in, and nothing here computes a path
 * outside it.
 */
bool store_find(char *out, size_t outsz);
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
bool store_open(Arena *a, Store *s, const char *dir, bool for_write, char *err,
                size_t errsz, const char **code);
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
/* 64 lowercase hex digits and nothing else. The one predicate every path that
 * turns a name into a blob path goes through. */
bool store_is_blob_name(const char *hash);

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

/* Overwrites it with what this build would use. `POST /reindex` (§7) is the
 * ONLY caller, and the reason is the one hole §1.6 and §8 leave between them:
 * §1.6 calls everything under index/ reconstructible from the logs and blobs,
 * while §8 defines this file as the record of the configuration the index was
 * BUILT with — which the logs do not contain and cannot. So `rebuild` writes
 * it only when absent and otherwise obeys it, and `reindex` is the one
 * command that re-derives every chunk under new parameters and therefore the
 * one command entitled to say so. */
bool store_rewrite_chunk_params(Store *s, char *err, size_t errsz);

/* Deletes one blob by hash. `POST /compact` (§7) is the only caller. The name
 * is validated exactly as store_get_blob validates it, so nothing that is not
 * 64 hex digits can ever be turned into a path and unlinked. */
bool store_drop_blob(Store *s, const char *hash);

/* Total bytes of everything under the store, and of index/ alone. */
uint64_t store_disk_bytes(Arena *a, const Store *s, uint64_t *index_bytes);

#endif /* KB_STORE_H */
