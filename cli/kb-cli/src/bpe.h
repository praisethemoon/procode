/* Byte-level BPE, the tokenizer ModernBERT's model files name
 * (`tokenizer.ggml.model` = "gpt2"): GPT-2's scheme, as the Hugging Face
 * tokenizer for gte-modernbert-base runs it.
 *
 * THE VOCABULARY AND THE MERGES ARE IN THE MODEL FILE, for the same reason
 * WordPiece's are (wpm.h): the ids mean these embedding rows and no others.
 *
 * FOUR STAGES, IN THE ORDER HUGGING FACE RUNS THEM, and each is exact:
 *
 *   1. NFC normalisation (utf8proc). Invalid UTF-8 becomes U+FFFD first.
 *   2. ADDED TOKENS ARE SPLIT OUT BEFORE ANYTHING ELSE, leftmost and longest
 *      first: the specials, the placeholders, the [unused] slots, and above
 *      all the runs of 2 to 24 spaces, which is what code indentation
 *      becomes. Each match is one id; only the text between matches goes on.
 *   3. The GPT-2 split, `'s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+|
 *      ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+`, as a scanner rather than a regex
 *      engine: the alternatives in that order, first match wins, `\s` is
 *      Unicode White_Space (Oniguruma's), and categories come from utf8proc.
 *   4. Each piece's bytes map to GPT-2's printable byte alphabet and are
 *      merged pairwise, lowest merge rank first, until no pair in the merge
 *      table is left.
 *
 * VERSIONED like WordPiece: KB_BPE_VERSION is part of what makes a stored
 * vector stale, so any change that could tokenise a text differently bumps it.
 */
#ifndef KB_BPE_H
#define KB_BPE_H

#include "gguf.h"

#define KB_BPE_VERSION 1u

typedef struct {
    Str *tokens;
    uint64_t n;
    /* token text -> id, open addressing; 0 empty, else id + 1 */
    uint32_t *slots;
    uint32_t slot_cap;
    /* The longest normal token's text, in bytes: an upper bound on the raw
     * bytes one token can stand for. */
    uint32_t max_token_bytes;
    /* (left id, right id) -> merged id and rank, open addressing */
    uint64_t *merge_keys; /* 0 empty, else ((left+1) << 32) | (right+1) */
    int32_t *merge_into;
    int32_t *merge_rank;
    uint32_t merge_cap;
    /* The id of each byte's one-character token in GPT-2's alphabet. */
    int32_t byte_id[256];
    /* Added tokens, matched before anything else. */
    Str *added;
    int32_t *added_ids;
    uint32_t n_added;
    bool added_first[256]; /* can an added token start with this byte? */
    int32_t cls, sep, pad, unk;
} Bpe;

/* Reads the vocabulary, the merges, the added tokens and the special ids out
 * of the GGUF. Fails when the file's tokenizer is not "gpt2" or anything the
 * scheme needs is missing. */
bool bpe_init(Arena *a, const Gguf *g, Bpe *b, char *err, size_t errsz);

/* Tokenises one text. With add_special, [CLS] leads and [SEP] follows. At
 * most `cap` ids are written; a longer text is TRUNCATED and `*truncated` is
 * set, keeping the final slot for [SEP] so the sequence stays well-formed. */
size_t bpe_encode(Arena *a, const Bpe *b, const char *text, size_t len,
                  bool add_special, int32_t *out, size_t cap, bool *truncated);

/* A pair, as a cross-encoder reads it: [CLS] a [SEP] b [SEP]. When the pair
 * does not fit, `b` (the passage) is cut first, then `a`. */
size_t bpe_encode_pair(Arena *a, const Bpe *b, const char *x, size_t xlen,
                       const char *y, size_t ylen, int32_t *out, size_t cap,
                       bool *truncated);

#endif /* KB_BPE_H */
