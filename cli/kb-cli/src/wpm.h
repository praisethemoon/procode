/* WordPiece, the tokenizer this model file names in its own metadata
 * (`tokenizer.ggml.model` = "bert").
 *
 * THE VOCABULARY IS NOT IN THIS FILE. It is 30522 strings in the GGUF's
 * metadata and it is read from there. Hard-coding one would mean a second
 * copy that could drift from the weights it belongs to, and the weights are
 * the whole reason the vocabulary matters: token 7592 means something
 * specific to THESE embedding rows.
 *
 * TWO STAGES, AND THE FIRST IS THE SUBTLE ONE.
 *
 *   1. Text is cut into words. Whitespace separates; control characters and
 *      U+FFFD vanish; every punctuation or symbol character becomes a word
 *      of its own; a CJK ideograph becomes a word of its own; letters are
 *      lower-cased and accents are stripped. `io_uring_prep_recv` therefore
 *      becomes seven words — io, _, uring, _, prep, _, recv — because `_`
 *      is punctuation, and `café` becomes `cafe`.
 *
 *   2. Each word gets a "▁" (U+2581) glued to its front and is then matched
 *      greedily, longest prefix first, against the vocabulary. The marker is
 *      how this vocabulary spells "start of a word": the conversion that
 *      produced the GGUF rewrote BERT's `hello` as `▁hello` and its
 *      continuation piece `##ing` as `ing`, so one forward greedy match over
 *      `▁hello` does what BERT's two-case rule did. A word with any
 *      unmatchable position contributes [UNK] and nothing else — not a
 *      partial decomposition — because a prefix that matched is not evidence
 *      about a word that did not.
 *
 * WHAT IS EXACT AND WHAT IS BEST-EFFORT. Every rule above is exact for
 * ASCII, which is what this corpus is made of. Beyond ASCII the character
 * classes are expressed as explicit codepoint ranges (see wpm.c) covering
 * the Latin, punctuation and CJK blocks that documentation actually
 * contains; a codepoint outside them is treated as an ordinary letter. That
 * is a decision, not an accident, and it is versioned: KB_WPM_VERSION is
 * part of what makes an index stale, so a later, better table cannot quietly
 * change what a stored vector means.
 */
#ifndef KB_WPM_H
#define KB_WPM_H

#include "gguf.h"

/* Bump for ANY change that could make the same text tokenise differently. */
#define KB_WPM_VERSION 1u

typedef struct {
    Str *tokens;
    uint64_t n;
    uint32_t *slots; /* open addressing; 0 empty, else id + 1 */
    uint32_t slot_cap;
    uint32_t max_token_len;
    int32_t cls, sep, unk, pad;
} Wpm;

/* Reads the vocabulary and the special ids out of the GGUF. Fails when the
 * tokenizer named there is not one this build implements, or when the
 * vocabulary is absent or empty. */
bool wpm_init(Arena *a, const Gguf *g, Wpm *w, char *err, size_t errsz);

/* Exact vocabulary lookup over raw bytes. -1 when absent. */
int32_t wpm_lookup(const Wpm *w, const char *s, size_t n);

/* Tokenises. With add_special, [CLS] leads and [SEP] follows. Writes at most
 * `cap` ids and returns how many it wrote; when the text produces more than
 * `cap`, it is TRUNCATED and `*truncated` is set — a chunk longer than the
 * model's context is a chunk the model will only see the start of, and the
 * caller has to be able to say so. With add_special the final slot is kept
 * for [SEP], so a truncated sequence is still a well-formed one. */
size_t wpm_encode(Arena *a, const Wpm *w, const char *text, size_t len,
                  bool add_special, int32_t *out, size_t cap, bool *truncated);

/* The word-splitting stage on its own, for tests: the words that stage 2
 * will look up, each already lower-cased and accent-stripped, WITHOUT the
 * "▁" marker. */
size_t wpm_words(Arena *a, const char *text, size_t len, Str **out);

#endif /* KB_WPM_H */
