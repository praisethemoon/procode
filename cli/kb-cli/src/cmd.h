/* Command entry points and shared CLI helpers. */
#ifndef KB_CMD_H
#define KB_CMD_H

#include "chunk.h"
#include "errdet.h"
#include "fts.h"
#include "store.h"

int32_t cmd_init(Arena *a, int32_t argc, char **argv);
int32_t cmd_add(Arena *a, int32_t argc, char **argv);
int32_t cmd_ls(Arena *a, int32_t argc, char **argv);
int32_t cmd_get(Arena *a, int32_t argc, char **argv);
int32_t cmd_collections(Arena *a, int32_t argc, char **argv);
int32_t cmd_status(Arena *a, int32_t argc, char **argv);
int32_t cmd_search(Arena *a, int32_t argc, char **argv);
int32_t cmd_chunk(Arena *a, int32_t argc, char **argv);
int32_t cmd_rebuild(Arena *a, int32_t argc, char **argv);
int32_t cmd_stale(Arena *a, int32_t argc, char **argv);
int32_t cmd_refresh(Arena *a, int32_t argc, char **argv);
int32_t cmd_links(Arena *a, int32_t argc, char **argv);
int32_t cmd_stats(Arena *a, int32_t argc, char **argv);
int32_t cmd_reindex(Arena *a, int32_t argc, char **argv);
int32_t cmd_compact(Arena *a, int32_t argc, char **argv);
int32_t cmd_forget(Arena *a, int32_t argc, char **argv);

/* Files a folder into a collection, incrementally (cmd_add.c); `kb refresh`
 * of a `dir` source runs it again. */
int32_t add_dir(Arena *a, bool json, const char *dir, const char *collection,
                bool forget);
int32_t cmd_sources(Arena *a, int32_t argc, char **argv);

/* ---- staleness (§5) ----------------------------------------------------
 *
 * ONE DEFINITION OF STALE, AND EVERY ROUTE READS IT FROM HERE. §5 puts the
 * flag on a search hit and the threshold on `GET /stale`; if the two were
 * computed in two places they could disagree, and a reader would be told a
 * passage is fresh by one route and stale by another.
 *
 * `now` is sampled ONCE per command. Two calls to the clock inside one
 * response can straddle a second, and then two hits of the same age would
 * not agree about whether they are stale.
 */
typedef struct {
    int64_t seconds; /* the threshold */
    int64_t now;     /* epoch seconds, sampled once */
    int64_t cutoff;  /* now - seconds; fetched before this is stale */
    const char *spec;     /* "90d", as given or defaulted */
    char cutoff_iso[32];  /* the cutoff as a timestamp, for the response */
} Staleness;

/* `older_than` is NULL for KB_STALE_DEFAULT. Fails with a message naming the
 * units that exist. */
bool staleness_init(Staleness *st, const char *older_than, char *err,
                    size_t errsz);

/* THE definition. A document whose fetchedAt cannot be read is stale: §5
 * exists because "a passage that cannot say how old it is will eventually be
 * believed when it should not be", and one with no readable date cannot say
 * how old it is at all. */
bool doc_stale(const Staleness *st, const Document *d);

/* `<count><unit>`, units s/m/h/d/w. A bare number is refused rather than
 * guessed at: "90" is ninety of something, and the difference between
 * seconds and days is the difference between everything and nothing. */
bool duration_parse(const char *s, int64_t *seconds);

/* §5 spells it `olderThan` in a query string and a command line spells it
 * `--older-than`. Both are accepted, the same way `--min-score` accepts
 * `--minScore`; every command that takes a threshold reads it through here so
 * the two spellings cannot diverge one command at a time. */
const char *older_than_arg(int32_t argc, char **argv,
                           const char *const *value_flags);

/* ---- stored documents --------------------------------------------------
 *
 * THE SPLITTER IS A FUNCTION OF THE RECORD AND NOTHING ELSE. Ingest knows
 * more than it keeps — the file it read the text out of, the extension on
 * the URL it was told about — and if it chose the splitter from that, a
 * reader holding only the log would split the same blob a different way.
 * The chunk range in the record would then stop describing the chunks on
 * disk, and `C-(chunkBase+i)` would name different text depending on who
 * was asking. So ingest, `get`, `search`, `chunk` and `rebuild` all derive
 * it here, from the two fields the log actually carries.
 */
Lang doc_lang(const char *mime, const char *path);

/* Reads a document's blob and splits it with the parameters the store's
 * index/model.json records — which is what the store was built with, not
 * what this build would choose today. Returns false when the blob is gone. */
bool doc_chunks(Arena *a, Store *s, const Document *d, char **text,
                size_t *len, Chunks *out);

/* The document whose reserved range holds chunk id C-n, and the ordinal
 * within it. NULL when no document claims it. */
const Document *doc_by_chunk(const DocList *l, int64_t chunk_num,
                             uint32_t *ordinal);

/* ---- which documents (§2's GET /documents, §4's filters) ---------------
 *
 * ONE FILTER, READ ONE WAY, FOR `ls` AND `search`. A collection list that
 * `search` accepted and `ls` did not, or a `since` one of them validated,
 * would make the same scope mean two things.
 *
 * - `--collection a,b`   any of the named collections
 * - `--source S-n`, `--mime M`
 * - `--since T`          fetched at or after T: a kb timestamp, or a bare
 *                        date meaning its midnight UTC. Anything else is
 *                        refused rather than compared as a string, where
 *                        "2026-9-1" would sort after every September date.
 * - `--meta '{...}'`     every key must match: the same JSON value, or, where
 *                        the document's value is an array, one of its
 *                        elements. The same shape `kb add --meta` takes.
 * - `--q text`           (ls) a case-insensitive substring of the title or
 *                        the locator
 */
typedef struct {
    const char *collection;
    const char *source;
    const char *mime;
    const char *q;
    bool has_since;
    int64_t since;
    const JVal *meta;
} DocQuery;

/* A case-insensitive (ASCII) substring test; an empty needle is in anything. */
bool icase_contains(const char *hay, const char *needle);

bool docquery_parse(Arena *a, int32_t argc, char **argv,
                    const char *const *value_flags, DocQuery *q, char *err,
                    size_t errsz);
bool docquery_keep(Arena *a, const DocQuery *q, const Document *d,
                   const Source *src);

/* A document that already exists, handed its text again, in a store already
 * open for write. Unchanged text is a touch: §2's "re-indexes nothing and
 * updates fetchedAt" — unless it comes with a new title or new meta, which
 * are recorded over the same chunks. `meta` NULL keeps the document's own;
 * `etag` NULL keeps it for the same text and clears it for new text, since an
 * ETag names one version of a page;
 * a new mime changes the split, so it counts as changed text. Changed text is a new version under the same id with a
 * fresh chunk range — the old chunk ids go with the text they named rather
 * than being handed to different passages (§1.1). `out` is the record now in
 * force. When *reindexed the caller rebuilds the keyword index; a touch
 * changes no term, so it needs none. The one path `kb add` and
 * `kb refresh S-n` share. */
bool refile_document(Arena *a, Store *s, const Document *existing,
                     const char *content, size_t len, const char *hash,
                     const char *title, const char *mime, const char *meta,
                     const char *etag, const char *now, Document *out,
                     bool *reindexed, bool *blob_written, char *err,
                     size_t errsz);

/* Records a source's status (§1.2) when it differs from the one in force:
 * "fetch_failed" when reading it again failed, "ok" when something was filed
 * from it since. Appends nothing when the status already holds. */
bool source_set_status(Arena *a, Store *s, const Source *src,
                       const char *status, char *err, size_t errsz);

/* Forgets documents, then sources, in a store already open for write: one
 * forget record each, documents first so no document is ever left naming a
 * source the fold no longer has, then the keyword index rebuilt so a search
 * stops finding them the moment this returns. Blobs stay until `kb compact`.
 * The one path every forgetting command goes through. */
bool forget_records(Arena *a, Store *s, const char *const *docs, size_t ndocs,
                    const char *const *srcs, size_t nsrcs, char *err,
                    size_t errsz);

/* Rebuilds index/fts.db for a store already open for write. Its caller
 * holds the lock, so the logs it reads cannot move underneath it. */
bool index_rebuild(Arena *a, Store *s, uint32_t *docs, uint32_t *missing_blobs,
                   FtsBuildStats *stats, char *err, size_t errsz);

/* ---- argument scanning (cmd_common.c) ----
 *
 * Flags listed in value_flags (NULL-terminated, may be NULL) consume the
 * next argument as their value, so a title that looks like a flag is never
 * read as one; "--flag=value" works for the same flags; and a literal "--"
 * ends flag parsing so a positional may start with '-'.
 */
bool has_flag(int32_t argc, char **argv, const char *const *value_flags,
              const char *flag);
const char *flag_value(int32_t argc, char **argv,
                       const char *const *value_flags, const char *flag);
const char *positional_arg(int32_t argc, char **argv,
                           const char *const *value_flags, int32_t index);
/* The first argument that is neither a known flag nor a known flag's value.
 * Catching it turns a typo into an error instead of a silent no-op. */
const char *unknown_flag(int32_t argc, char **argv,
                         const char *const *value_flags,
                         const char *const *bool_flags);

void err_out(bool json_mode, const char *code, const char *fmt, ...);

/* Appends text that came from an ingested document, with control bytes
 * flattened to spaces. Titles and headings are third-party content by
 * definition here, and a terminal that renders an escape sequence out of
 * one is being driven by whoever wrote the page. */
void sb_puts_safe(StrBuf *sb, const char *s);

/* Reads a whole file, or stdin for "-". */
bool read_text_arg(Arena *a, const char *path, char **out, size_t *out_len);

/* ---- the store ----
 *
 * §1.4's store is `.kb/` found by walking up from the cwd. Fills dir and
 * returns true, or fills err with the "run kb init" message and returns
 * false; every command reports that as `not_found`.
 */
bool store_resolve(char *dir, size_t dirsz, char *err, size_t errsz);

/* ---- shared JSON shapes ----
 *
 * One definition per entity, so `ls`, `get`, `collections` and `status`
 * cannot drift into describing the same thing three ways. Each emits the
 * field list WITHOUT enclosing braces.
 */
void json_document(StrBuf *sb, const Document *d, const Source *src,
                   const Staleness *st);
void json_source(StrBuf *sb, const Store *s, const Source *src);

/* §6's rows, resolved. `outgoing` selects which side of the adjacency; the
 * row names the document at the far end and carries `resolved:false` when
 * that document is not in the log — a link whose target was forgotten must
 * not break a read, and silently dropping the row would hide the dangling
 * edge instead of showing it. */
void json_links(StrBuf *sb, const Store *s, const char *id, bool outgoing,
                const Staleness *st);

/* Derived Source facts (§1.2 lists them on the entity; they are computed
 * from the source's documents rather than stored, so the two can never
 * disagree). fetched_at is the newest of them. */
typedef struct {
    uint32_t doc_count;
    uint64_t bytes;
    const char *fetched_at; /* NULL when the source has no documents */
    const char *content_hash;
    const char *etag;       /* the newest document's, or NULL */
} SourceFacts;

SourceFacts source_facts(Arena *a, const Store *s, const char *source_id);

#endif /* KB_CMD_H */
