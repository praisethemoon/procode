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
 * sources.jsonl holds `source` records folded last-wins by id, plus `forget`
 * records that remove one. Everything §1.2 lists on Source that changes as
 * documents arrive — fetchedAt, contentHash, docCount, bytes, status — is
 * derived from that source's documents instead of stored, so the two can
 * never disagree. What a later `source` record CAN revise is the collection,
 * because §7's `PATCH /collections/{name}` renames a topic and a collection
 * exists nowhere but on a Source (§1.3).
 *
 * documents.jsonl holds `document` records folded last-wins by id, plus
 * `touch` records that carry a new fetchedAt and nothing else, and `forget`
 * records that remove one (§2's DELETE /documents/{id}). A re-ingest
 * of unchanged content is exactly a touch: §2 says it re-indexes nothing
 * and updates fetchedAt, and in an append-only log an update is a later
 * record that supersedes an earlier one.
 *
 * IT ALSO HOLDS `link` AND `unlink` RECORDS (§6), AND THAT IS DELIBERATE.
 * §1.6 lists `index/links.bin` as a derived, disposable adjacency, which
 * means the truth is a log — but §1.5 names exactly three committed things,
 * and a fourth top-level file would quietly enlarge that list. Links belong
 * in documents.jsonl for a stronger reason than economy: a link is a fact
 * about two documents, and a separate file could be committed out of step
 * with the one that gives it referents. One file, one commit, one consistent
 * state. The loader already ignores record kinds it does not know, so an
 * older build reads a store with links in it and simply sees no links.
 *
 * The link's own §1.2 field `type` is spelled `rel` in the record, because
 * `type` already names the record KIND in both logs and one word cannot
 * carry both jobs.
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
    const char *kind;       /* url | file | inline (§1.2) */
    const char *locator;
    const char *title;
    const char *collection;
    const char *created_at; /* ISO-8601 UTC */
    /* §1.2's status: NULL or "ok", or "fetch_failed" once reading it again
     * failed. Recorded, not derived, because the failure left no document
     * behind to derive it from; the next successful filing sets it back. */
    const char *status;
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
    /* The HTTP ETag the caller saw when it fetched this text, or NULL. kb
     * fetches nothing itself, so only whoever did can say; a source's etag
     * (§1.2) is its latest document's. */
    const char *etag;
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

/* §1.2's Link, with `type` spelled `rel` (see the header comment). Both ends
 * name documents in THIS store: identifiers are per store (§1.4) and §1.2
 * gives a Link no store field, so a cross-tier edge is not representable and
 * is not invented here. */
typedef struct {
    const char *from;
    const char *rel;
    const char *to;
    const char *created_at;
} Link;

/* §6's six, and nothing else is a link type. One table, so the writer that
 * validates and the reader that prints cannot drift apart. The sixth,
 * `imports`, is kb's own: `kb add --dir` keeps it from the files' code
 * (imports.h), and nobody writes it by hand. */
extern const char *const LINK_TYPES[];
#define LINK_TYPE_COUNT 6
#define LINK_IMPORTS "imports"
/* The canonical spelling, or NULL when `rel` is not one of the six. Returning
 * the table's own pointer means an accepted type is stored as the table spells
 * it rather than as the caller typed it. */
const char *link_type_canon(const char *rel);

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
    /* §6's adjacency, folded from the same single pass over documents.jsonl.
     * It is NOT materialised under index/: every other derived structure is
     * cached because recomputing it means re-chunking or re-embedding, and
     * this one is already in memory the moment the log is read. A cache with
     * no reader cannot be checked for staleness by anything, so there is no
     * links.bin here and `kb rebuild` reconstructs the adjacency by doing
     * what every open already does. */
    Link *links;
    size_t nlinks;
} DocList;

/* Encoders. Each returns one line WITHOUT its trailing newline. */
char *doc_encode_source(Arena *a, const Source *s, size_t *out_len);
char *doc_encode_document(Arena *a, const Document *d, size_t *out_len);
char *doc_encode_touch(Arena *a, const char *id, const char *fetched_at,
                       size_t *out_len);
/* `link` when present, `unlink` when not. The pair folds last-wins over the
 * (from, rel, to) triple, so removing an edge and restoring it are the same
 * append-only motion as superseding a document. */
char *doc_encode_link(Arena *a, const Link *l, bool present, size_t *out_len);
/* Removes a source from the fold (§2's DELETE /sources/{id}, and §7's
 * DELETE /collections/{name} for each source in the topic). Its documents are
 * forgotten first, each with its own record below, so no document is ever
 * left pointing at a source the fold no longer has. */
char *doc_encode_source_forget(Arena *a, const char *id, size_t *out_len);
/* Removes a document from the fold (§2's DELETE /documents/{id}). The id and
 * its chunk range stay reserved — the counters floor on every record ever
 * written — so nothing can be handed a forgotten identifier (§1.1). Links
 * that named it keep reading, as `resolved: false` (§6). Its blob stays until
 * `kb compact` finds nothing referring to it. */
char *doc_encode_document_forget(Arena *a, const char *id, size_t *out_len);

/* Loaders. A torn (unterminated) final line is dropped and reported; a
 * complete line that does not parse is an error, because silently skipping
 * it would turn real corruption into quiet data loss.
 */
bool srclog_load(Arena *a, const char *path, SourceList *out, char *err,
                 size_t errsz);
bool doclog_load(Arena *a, const char *path, DocList *out, char *err,
                 size_t errsz);

/* One time a document was filed, as the log recorded it: a `document` record
 * (the text was new, so it was indexed) or a `touch` (the same text read
 * again). The fold keeps only the latest; this is §2's "fetch history". */
typedef struct {
    const char *at;
    bool changed;
} Fetch;

/* Every fetch of one document, oldest first. */
bool doclog_fetches(Arena *a, const char *path, const char *document_id,
                    Fetch **out, size_t *n, char *err, size_t errsz);

/* Lookups over a loaded log. NULL when absent. */
const Source *src_by_id(const SourceList *l, const char *id);
const Source *src_by_key(const SourceList *l, const char *kind,
                         const char *locator, const char *collection);
const Document *doc_by_id(const DocList *l, const char *id);
const Document *doc_by_source_path(const DocList *l, const char *source,
                                   const char *path);
const Link *link_find(const DocList *l, const char *from, const char *rel,
                      const char *to);

#endif /* KB_DOC_H */
