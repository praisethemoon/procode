/* The two append-only logs: record shapes, encoding, decoding, folding.
 *
 * sources.jsonl and documents.jsonl are the truth (§1.5). Everything under
 * index/ is derived from them and from the blobs.
 *
 * Both logs are JSONL: one record per line, the line terminated by '\n'.
 * A record never contains a raw newline — every string in them goes through
 * json_escape, which escapes control bytes — so '\n' is an exact frame
 * boundary and a reader can tell a complete record from a torn one by
 * nothing more than the presence of the terminator.
 *
 * sources.jsonl holds one `source` record per source and never revises it.
 * Everything §1.2 lists on Source that changes as documents arrive —
 * fetchedAt, contentHash, docCount, bytes, status — is derived from that
 * source's documents instead of stored, so the two can never disagree.
 *
 * documents.jsonl holds `document` records folded last-wins by id, plus
 * `touch` records that carry a new fetchedAt and nothing else. A re-ingest
 * of unchanged content is exactly a touch: §2 says it re-indexes nothing
 * and updates fetchedAt, and in an append-only log an update is a later
 * record that supersedes an earlier one.
 */
#ifndef KB_DOC_H
#define KB_DOC_H

#include "json.h"

/* Identifiers are public, prefixed and self-describing (§1.1): "S-3" is a
 * source wherever it appears — a log line, a citation, a prompt. These two
 * are the only places the spelling is decided, so no caller can invent a
 * second one. kb_id_num returns 0 for anything that is not an id of the
 * requested kind, including NULL, a wrong prefix and trailing garbage. */
int64_t kb_id_num(const char *id, char prefix);
char *kb_id_make(Arena *a, char prefix, int64_t n);

typedef struct {
    const char *id;         /* "S-3" */
    const char *kind;       /* url | file | dir | inline (§1.2) */
    const char *locator;
    const char *title;
    const char *collection;
    const char *created_at; /* ISO-8601 UTC */
} Source;

typedef struct {
    const char *id;     /* "D-241" */
    const char *source; /* "S-3" */
    const char *path;   /* "" when the source addresses one document */
    const char *title;
    const char *mime;
    const char *content_hash;
    const char *fetched_at;
    const char *indexed_at;
    /* The writer's own bytes for a JSON object, or NULL. §1.2 calls meta
     * free-form and filterable but not schema-bound, so it is stored and
     * returned exactly as handed in; kb never interprets it. */
    const char *meta;
    uint64_t bytes;
    uint32_t chunk_count;
    /* Chunk ids are the contiguous range [chunk_base, chunk_base+chunk_count).
     * Chunks themselves live under index/ and are disposable, but their ids
     * are public and must never be reused (§1.1) — so the range is reserved
     * from the durable counter at ingest and recorded here. A rebuild then
     * reproduces the same ids from the log, and a rechunk takes a fresh
     * range rather than reusing this one. */
    int64_t chunk_base;
} Document;

typedef struct {
    Source *v;
    size_t n;
    /* Highest S-id seen across every record, including any later superseded
     * or tombstoned. The id counter floors itself on this, so an id can
     * never be handed out twice even if the entity that held it is gone. */
    int64_t max_id;
    bool torn_tail; /* an unterminated final line was dropped */
} SourceList;

typedef struct {
    Document *v;
    size_t n;
    int64_t max_id;
    int64_t max_chunk_id;
    bool torn_tail;
} DocList;

/* Encoders. Each returns one line WITHOUT its trailing newline. */
char *doc_encode_source(Arena *a, const Source *s, size_t *out_len);
char *doc_encode_document(Arena *a, const Document *d, size_t *out_len);
char *doc_encode_touch(Arena *a, const char *id, const char *fetched_at,
                       size_t *out_len);

/* Loaders. A torn (unterminated) final line is dropped and reported; a
 * complete line that does not parse is an error, because silently skipping
 * it would turn real corruption into quiet data loss.
 */
bool srclog_load(Arena *a, const char *path, SourceList *out, char *err,
                 size_t errsz);
bool doclog_load(Arena *a, const char *path, DocList *out, char *err,
                 size_t errsz);

/* Lookups over a loaded log. NULL when absent. */
const Source *src_by_id(const SourceList *l, const char *id);
const Source *src_by_key(const SourceList *l, const char *kind,
                         const char *locator, const char *collection);
const Document *doc_by_id(const DocList *l, const char *id);
const Document *doc_by_source_path(const DocList *l, const char *source,
                                   const char *path);

#endif /* KB_DOC_H */
