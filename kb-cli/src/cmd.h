/* Command entry points and shared CLI helpers. */
#ifndef KB_CMD_H
#define KB_CMD_H

#include "chunk.h"
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

/* ---- tier selection ----
 *
 * §1.4: the project store is `.kb/` found by walking up from the cwd, the
 * global store is ~/.kb or $KB_STORE. A write defaults to the project store
 * when one exists and to global otherwise, because the intent at the moment
 * of filing is almost always project-scoped. Reads default to both, because
 * a reader wants what is in the store, not a lecture about which tier it is
 * in — every row says which tier it came from.
 */
typedef enum { SEL_DEFAULT, SEL_PROJECT, SEL_GLOBAL, SEL_ALL } StoreSel;

typedef struct {
    char dir[2][KB_PATH_MAX];
    Tier tier[2];
    size_t n;
} TierSet;

/* Parses --store. Returns false on an unrecognized value. */
bool store_sel_parse(const char *v, StoreSel *out);

/* Fills out with the store directories to use, project first. for_write
 * resolves to exactly one and refuses "all"; a read takes every tier in
 * scope that exists. Returns false with a message when nothing qualifies.
 */
bool tiers_resolve(StoreSel sel, bool for_write, TierSet *out, char *err,
                   size_t errsz);

/* ---- shared JSON shapes ----
 *
 * One definition per entity, so `ls`, `get`, `collections` and `status`
 * cannot drift into describing the same thing three ways. Each emits the
 * field list WITHOUT enclosing braces.
 */
void json_document(StrBuf *sb, const Store *s, const Document *d,
                   const Source *src);
void json_source(StrBuf *sb, const Store *s, const Source *src);

/* Derived Source facts (§1.2 lists them on the entity; they are computed
 * from the source's documents rather than stored, so the two can never
 * disagree). fetched_at is the newest of them. */
typedef struct {
    uint32_t doc_count;
    uint64_t bytes;
    const char *fetched_at; /* NULL when the source has no documents */
    const char *content_hash;
} SourceFacts;

SourceFacts source_facts(Arena *a, const Store *s, const char *source_id);

#endif /* KB_CMD_H */
