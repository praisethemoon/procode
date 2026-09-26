/* Structural chunking (§3).
 *
 * Chunks follow the document's own structure, not a fixed window: Markdown
 * and HTML split on heading boundaries, source code on top-level
 * declarations, and everything else falls back to a sliding window with
 * overlap. A section that is too large for the window is itself split by
 * sliding window, keeping the heading it belongs to.
 *
 * Every chunk carries the heading it sits under and its byte span into the
 * document text exactly as stored, so a hit can be shown in place. The
 * spans of a document's chunks start at 0, end at its length, and never
 * leave a gap; only sliding-window chunks overlap.
 *
 * Sizing targets the model's context window, so the parameters live in
 * index/model.json (§8) and are passed in rather than assumed here.
 */
#ifndef KB_CHUNK_H
#define KB_CHUNK_H

#include "str.h"

typedef enum { LANG_MARKDOWN, LANG_HTML, LANG_CODE, LANG_TEXT } Lang;

typedef struct {
    const char *heading; /* NULL when the chunk sits under none */
    size_t start, end;   /* byte span into the document text */
    uint32_t tokens;
} Chunk;

typedef struct {
    Chunk *v;
    size_t n;
} Chunks;

/* Which splitter applies. The mime decides when it says anything useful;
 * otherwise the path's extension does. Unknown means plain text, which is
 * the fallback the spec names. */
Lang chunk_lang(const char *mime, const char *path);
const char *chunk_lang_name(Lang l);

/* Whether text of this type can be split at all: any text/ type, and the
 * application/ types that are text by another name. A PDF, an image or an
 * archive would be split as plain text and indexed as noise, so ingest refuses
 * them (§11's unsupported_mime) instead of falling back. */
bool chunk_mime_supported(const char *mime);

/* Tokens are estimated from bytes until a real tokenizer exists (§8). */
uint32_t chunk_tokens_of(size_t bytes);

Chunks chunk_split(Arena *a, const char *text, size_t len, Lang lang,
                   size_t target_bytes, size_t overlap_bytes);

#endif /* KB_CHUNK_H */
