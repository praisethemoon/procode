/* Tokenisation for keyword retrieval (§4).
 *
 * WHY THIS CORPUS NEEDS ITS OWN TOKENISER. §4 is explicit about what is
 * being indexed: text dense with exact identifiers — `CreateIoCompletionPort`,
 * `IORING_SETUP_SQPOLL`, `EVFILT_READ`, `io_uring_prep_recv`. The whole
 * argument for hybrid retrieval is that embeddings put `io_uring_prep_recv`
 * and `io_uring_prep_send` on top of each other, and that keyword retrieval
 * has to separate them exactly. A tokeniser that splits on '_' throws that
 * away: both become {io, uring, prep, ...} and the one distinguishing word
 * is one term out of four. So:
 *
 *   '_' IS PART OF A TOKEN. `io_uring_prep_recv` is one term. So is
 *   `IORING_SETUP_SQPOLL`. This is the property the spec's design rests on
 *   and it is the one thing here that is not negotiable.
 *
 *   DIGITS ARE PART OF A TOKEN. `sha256`, `int32_t`, `bge-small-en-v1.5`'s
 *   `v1` — a digit is as much a part of an identifier as a letter, and a
 *   tokeniser that cut at digits would shred version numbers and type names.
 *
 *   CASE IS FOLDED (ASCII). A reader types `ioring_setup_sqpoll` or
 *   `IORING_SETUP_SQPOLL` depending on whether they are quoting the header
 *   or remembering it. Case-sensitive matching would make one of those two
 *   queries silently fail, which is the worst kind of failure for a store
 *   whose value is that the answer is already in it. Bytes >= 0x80 are
 *   passed through unchanged: folding UTF-8 correctly needs Unicode tables,
 *   which are a dependency this binary does not have, and folding it
 *   incorrectly is worse than not folding it.
 *
 *   A COMPOUND ALSO YIELDS ITS PARTS. `CreateIoCompletionPort` emits the
 *   whole term AND `create`, `io`, `completion`, `port`; `io_uring_prep_recv`
 *   emits the whole term AND `io`, `uring`, `prep`, `recv`. This costs
 *   nothing in exactness — the whole term is rare, so its idf dwarfs the
 *   parts', and a document holding the real identifier always outranks one
 *   that merely shares three of its four words — and it buys the reader who
 *   half-remembers "completion port" a hit on the page that spells it
 *   `CreateIoCompletionPort`. Parts are marked non-primary so they do not
 *   inflate the document length that BM25 normalises against: length is
 *   meant to say how much text there is, and the parts are kb's invention,
 *   not the author's words.
 *
 * NO STEMMING, NO STOP WORDS. Stemming `IORING_SETUP_SQPOLL` is meaningless
 * and stemming English prose would blur exactly the distinctions this corpus
 * is made of. A stop list would make `kb search "the C ABI"` unable to find
 * `C`. BM25's idf already charges almost nothing for a term that is in every
 * document, which is the same job a stop list does, done from the data.
 */
#ifndef KB_TOKEN_H
#define KB_TOKEN_H

#include "str.h"

/* Longest term kept, in bytes. Anything longer is truncated rather than
 * dropped: tokens past this length are base64 payloads, hex digests and
 * minified code, and colliding two of those costs nothing, while dropping
 * them would silently remove text from the index. The query is truncated by
 * the same rule, so every identifier a person can actually type still
 * matches exactly. */
#define KB_TERM_MAX 64
/* Most parts one compound is split into. A 16-part identifier is already
 * pathological; stopping there bounds the work per token. */
#define KB_SUBWORD_MAX 16

typedef struct {
    char text[KB_TERM_MAX + 1]; /* case-folded, NUL-terminated */
    uint32_t len;
    /* Byte offset of the whole token in the input — a part reports the
     * offset of the compound it came from, because a snippet that matched
     * `recv` should show `io_uring_prep_recv`, not a fragment of it. */
    size_t off;
    /* False for a part split out of a compound. Only primary tokens count
     * toward a chunk's length. */
    bool primary;
} Token;

typedef void (*TokenFn)(const Token *t, void *ud);

/* Walks text, calling fn for every term. Each primary token is emitted
 * first, immediately followed by its parts when it is a compound. */
void token_scan(const char *text, size_t len, TokenFn fn, void *ud);

/* A query's distinct terms, in first-seen order. Queries are short by
 * nature and duplicates carry no extra information in this scoring model,
 * so `alpha alpha` and `alpha` ask the same question. */
typedef struct {
    const char **v; /* NUL-terminated, case-folded */
    size_t n;
} TermList;

/* Terms past this many are ignored. A query with more than this is not a
 * query, and the snippet picker tracks which terms it has seen in a 64-bit
 * mask. */
#define KB_QUERY_TERMS_MAX 64

TermList token_terms(Arena *a, const char *text, size_t len);
/* Index of term in the list, or -1. Lists are short; a scan beats a map. */
int32_t termlist_find(const TermList *l, const char *term);

#endif /* KB_TOKEN_H */
