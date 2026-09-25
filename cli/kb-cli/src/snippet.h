/* The passage a hit shows (§4).
 *
 * "Search returns snippets only… a list must not be able to flood a
 * caller's context." That is a hard limit on the size of a result, and it
 * makes choosing WHICH bytes to show part of the retrieval: a window taken
 * from the top of a chunk would, for a long passage, frequently contain
 * none of the words the query asked about, and a hit whose text does not
 * show why it is a hit is not worth the bytes it costs.
 *
 * So the window is placed where the query terms are densest — the run of
 * KB_SNIPPET_BYTES holding the most DISTINCT terms, not the most
 * occurrences, because a passage that mentions two of the query's words is
 * more likely the one wanted than a passage repeating one of them.
 *
 * Whitespace is flattened to single spaces. A snippet is one line: a result
 * list whose entries can contain newlines is a result list whose entries
 * can be made to look like other entries, and the text in it was written by
 * whoever wrote the page.
 *
 * Both edges are pulled back off an incomplete UTF-8 character. A chunk
 * boundary is a byte offset and so is a window, so either can land inside a
 * character; a JSON string is defined over text rather than bytes, and half
 * a character can cost a caller the whole response.
 */
#ifndef KB_SNIPPET_H
#define KB_SNIPPET_H

#include "token.h"

/* The snippet for text[start,end), NUL-terminated, with "…" marking each
 * end that was cut. q may be empty, in which case the window is the head of
 * the passage. */
char *snippet_of(Arena *a, const char *text, size_t start, size_t end,
                 const TermList *q);

#endif /* KB_SNIPPET_H */
