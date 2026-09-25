#include "wpm.h"

/* ---- UTF-8 -------------------------------------------------------------
 *
 * A decoder that refuses overlong forms, surrogates and out-of-range
 * codepoints, because this text came out of an ingested document and a
 * lenient decoder would turn a malformed byte into a codepoint the rest of
 * the file then reasons about. Anything invalid decodes as U+FFFD over one
 * byte, which the word splitter drops — the same answer llama.cpp reaches by
 * the same route. */
static uint32_t u8_next(const char *s, size_t len, size_t *i) {
    const uint8_t *p = (const uint8_t *)s;
    uint8_t c = p[*i];
    if (c < 0x80) {
        (*i)++;
        return c;
    }
    uint32_t cp;
    size_t need;
    if ((c & 0xE0) == 0xC0) {
        cp = c & 0x1Fu;
        need = 1;
    } else if ((c & 0xF0) == 0xE0) {
        cp = c & 0x0Fu;
        need = 2;
    } else if ((c & 0xF8) == 0xF0) {
        cp = c & 0x07u;
        need = 3;
    } else {
        (*i)++;
        return 0xFFFDu;
    }
    /* The continuation bytes are at *i+1 .. *i+need, so the last one has to
     * be inside the buffer. Written as an addition on the left because
     * `len - need` underflows for a short buffer. */
    if (*i + need >= len) {
        (*i)++;
        return 0xFFFDu;
    }
    for (size_t k = 1; k <= need; k++) {
        uint8_t cc = p[*i + k];
        if ((cc & 0xC0) != 0x80) {
            (*i)++;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
        (need == 3 && cp < 0x10000) || cp > 0x10FFFFu ||
        (cp >= 0xD800u && cp <= 0xDFFFu)) {
        (*i)++;
        return 0xFFFDu;
    }
    *i += need + 1;
    return cp;
}

static size_t u8_put(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* ---- character classes -------------------------------------------------
 *
 * ASCII is exact. Above it these are explicit ranges rather than a Unicode
 * database: the database is a dependency of real size, the corpus is English
 * technical documentation, and a wrong answer here costs one token in a
 * chunk of four hundred. What it must never be is UNSTABLE — see
 * KB_WPM_VERSION.
 */

static bool is_space(uint32_t c) {
    return c == 0x09u || c == 0x0Au || c == 0x0Bu || c == 0x0Cu ||
           c == 0x0Du || c == 0x20u || c == 0x85u || c == 0xA0u ||
           c == 0x1680u || (c >= 0x2000u && c <= 0x200Au) || c == 0x2028u ||
           c == 0x2029u || c == 0x202Fu || c == 0x205Fu || c == 0x3000u;
}

/* Control and format characters, which vanish rather than split. U+00AD
 * (soft hyphen) and the bidi and zero-width marks are here because they are
 * invisible in the source text and must be invisible in the tokens too. */
static bool is_control(uint32_t c) {
    return c < 0x20u || c == 0x7Fu || (c >= 0x80u && c <= 0x9Fu) ||
           c == 0xADu || (c >= 0x200Bu && c <= 0x200Fu) ||
           (c >= 0x202Au && c <= 0x202Eu) || (c >= 0x2060u && c <= 0x2064u) ||
           c == 0xFEFFu;
}

/* Every printable ASCII byte that is not a letter or a digit is punctuation
 * or a symbol, and BERT splits on both — which is why `io_uring_prep_recv`
 * is seven words and not one. Above ASCII these are the punctuation blocks
 * documentation actually uses: Latin-1's ¡ § ¶ · ¿ « », the General
 * Punctuation block (dashes, curly quotes, ellipsis, bullets), CJK
 * punctuation, and the fullwidth forms. */
static bool is_break_char(uint32_t c) {
    if (c < 0x80u) {
        bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                     (c >= 'a' && c <= 'z');
        return c > 0x20u && c != 0x7Fu && !alnum;
    }
    if (c == 0xA1u || c == 0xA7u || c == 0xABu || c == 0xB6u || c == 0xB7u ||
        c == 0xBBu || c == 0xBFu)
        return true;
    if (c >= 0x2010u && c <= 0x2027u)
        return true; /* dashes, quotes, ellipsis, bullets */
    if (c >= 0x2030u && c <= 0x205Eu)
        return true; /* per-mille through vertical line */
    if (c >= 0x3001u && c <= 0x303Fu)
        return true; /* CJK punctuation */
    if (c >= 0xFF01u && c <= 0xFF0Fu)
        return true;
    if (c >= 0xFF1Au && c <= 0xFF20u)
        return true;
    if (c >= 0xFF3Bu && c <= 0xFF40u)
        return true;
    if (c >= 0xFF5Bu && c <= 0xFF65u)
        return true;
    return false;
}

/* CJK ideographs are one word each — they are written without spaces, so
 * treating a run of them as a word would ask the vocabulary for a string it
 * has never seen. The ranges are BERT's own list. Kana are deliberately NOT
 * here: BERT leaves them to WordPiece, and so does this. */
static bool is_cjk(uint32_t c) {
    return (c >= 0x4E00u && c <= 0x9FFFu) || (c >= 0x3400u && c <= 0x4DBFu) ||
           (c >= 0xF900u && c <= 0xFAFFu) ||
           (c >= 0x20000u && c <= 0x2A6DFu) ||
           (c >= 0x2A700u && c <= 0x2B73Fu) ||
           (c >= 0x2B740u && c <= 0x2B81Fu) ||
           (c >= 0x2B820u && c <= 0x2CEAFu) ||
           (c >= 0x2F800u && c <= 0x2FA1Fu);
}

/* Combining marks, dropped outright. This is BERT's accent stripping: the
 * vocabulary was built from text that had already been decomposed and had
 * its marks removed, so `café` has to reach it as `cafe` or it matches
 * nothing at all. */
static bool is_mark(uint32_t c) {
    return (c >= 0x0300u && c <= 0x036Fu) || (c >= 0x1AB0u && c <= 0x1AFFu) ||
           (c >= 0x1DC0u && c <= 0x1DFFu) || (c >= 0x20D0u && c <= 0x20FFu) ||
           (c >= 0xFE20u && c <= 0xFE2Fu);
}

/* Canonical base letter for the precomposed Latin letters, which is the only
 * part of NFD that changes an answer here: decomposing turns À into A plus a
 * mark, and the mark is then dropped. A character with no Latin base — Æ, Ø,
 * Þ, ß, Đ — decomposes to itself and is kept, which is why `Straße` keeps
 * its ß. '.' means "no decomposition".
 *
 * Latin-1 Supplement, U+00C0 through U+00FF. */
static const char LATIN1_BASE[] =
    "AAAAAA.CEEEEIIII.NOOOOO..UUUUY.."
    "aaaaaa.ceeeeiiii.nooooo..uuuuy.y";
/* One entry per codepoint in the block, checked at compile time: a table
 * that is one character short silently shifts every letter after the gap. */
_Static_assert(sizeof LATIN1_BASE == 0x100u - 0xC0u + 1u,
               "LATIN1_BASE must cover U+00C0..U+00FF exactly");

/* Latin Extended-A, U+0100 through U+017F. Letters carrying a stroke, a bar
 * or a ligature rather than an accent — Đ, Ħ, ı, Ĳ, ĸ, Ŀ, Ł, ŉ, Ŋ, Œ, Ŧ, ſ —
 * have no canonical decomposition and are kept exactly as they are. */
static const char LATIN_A_BASE[] =
    "AaAaAaCcCcCcCcDd" /* 0100 */
    "..EeEeEeEeEeGgGg" /* 0110 */
    "GgGgHh..IiIiIiIi" /* 0120 */
    "I...JjKk.LlLlLl." /* 0130 */
    "...NnNnNn...OoOo" /* 0140 */
    "Oo..RrRrRrSsSsSs" /* 0150 */
    "SsTtTt..UuUuUuUu" /* 0160 */
    "UuUuWwYyYZzZzZz."; /* 0170 */
_Static_assert(sizeof LATIN_A_BASE == 0x180u - 0x100u + 1u,
               "LATIN_A_BASE must cover U+0100..U+017F exactly");

static uint32_t lower_cp(uint32_t c) {
    if (c < 0x80u)
        return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0u && c <= 0xDEu && c != 0xD7u)
        return c + 32; /* À..Þ, minus the multiplication sign */
    if (c >= 0x100u && c <= 0x17Fu) {
        /* The block is mostly upper/lower pairs, but not entirely, and the
         * three exceptions are the ones a pattern would get wrong:
         * U+0130 İ lower-cases to plain `i` and NOT to the dotless U+0131,
         * U+0138 ĸ has no upper-case partner at all, and U+0178 Ÿ
         * lower-cases back into Latin-1 as U+00FF. */
        if (c == 0x130u)
            return 'i';
        if (c == 0x138u || c == 0x149u || c == 0x17Fu)
            return c;
        if (c == 0x178u)
            return 0xFFu;
        /* Two runs are offset by one from the rest: U+0139..U+0148 and
         * U+0179..U+017E pair odd-then-even. */
        if ((c >= 0x139u && c <= 0x148u) || (c >= 0x179u && c <= 0x17Eu))
            return (c % 2 == 1) ? c + 1 : c;
        return (c % 2 == 0) ? c + 1 : c;
    }
    if (c >= 0x391u && c <= 0x3A9u && c != 0x3A2u)
        return c + 32; /* Greek */
    if (c >= 0x410u && c <= 0x42Fu)
        return c + 32; /* Cyrillic А..Я */
    if (c >= 0x400u && c <= 0x40Fu)
        return c + 80; /* Cyrillic Ѐ..Џ */
    return c;
}

/* Lower-case, then strip the accent if the result has a Latin base. The
 * order matters only for readability; both tables carry both cases. */
static uint32_t fold_cp(uint32_t c) {
    c = lower_cp(c);
    if (c >= 0xC0u && c <= 0xFFu) {
        char b = LATIN1_BASE[c - 0xC0u];
        if (b != '.')
            return (uint32_t)(unsigned char)b;
    } else if (c >= 0x100u && c <= 0x17Fu) {
        char b = LATIN_A_BASE[c - 0x100u];
        if (b != '.')
            return (uint32_t)(unsigned char)b;
    }
    return c;
}

/* ---- stage 1: words ----------------------------------------------------- */

typedef struct {
    Arena *a;
    Str *v;
    size_t n, cap;
    char *buf;
    size_t len, buf_cap;
} Words;

static void w_flush(Words *w) {
    if (!w->len)
        return;
    ARENA_GROW(w->a, w->v, w->n, w->cap, Str);
    w->v[w->n].ptr = arena_strndup(w->a, w->buf, w->len);
    w->v[w->n].len = w->len;
    w->n++;
    w->len = 0;
}

static void w_put(Words *w, uint32_t cp) {
    if (w->len + 4 > w->buf_cap) {
        size_t nc = w->buf_cap ? w->buf_cap * 2 : 64;
        w->buf = (char *)arena_realloc(w->a, w->buf, w->buf_cap, nc);
        w->buf_cap = nc;
    }
    w->len += u8_put(cp, w->buf + w->len);
}

size_t wpm_words(Arena *a, const char *text, size_t len, Str **out) {
    Words w;
    memset(&w, 0, sizeof w);
    w.a = a;
    for (size_t i = 0; i < len;) {
        uint32_t cp = u8_next(text, len, &i);
        if (is_space(cp)) {
            w_flush(&w);
            continue;
        }
        if (cp == 0 || cp == 0xFFFDu || is_control(cp) || is_mark(cp))
            continue;
        cp = fold_cp(cp);
        if (is_break_char(cp) || is_cjk(cp)) {
            w_flush(&w);
            w_put(&w, cp);
            w_flush(&w);
            continue;
        }
        w_put(&w, cp);
    }
    w_flush(&w);
    *out = w.v;
    return w.n;
}

/* ---- the vocabulary ----------------------------------------------------- */

static uint32_t vslot(const uint32_t *slots, uint32_t cap, const Str *toks,
                      const char *s, size_t n) {
    uint32_t i = (uint32_t)str_hash(str_n(s, n)) & (cap - 1);
    while (slots[i]) {
        const Str *t = &toks[slots[i] - 1];
        if (t->len == n && memcmp(t->ptr, s, n) == 0)
            break;
        i = (i + 1) & (cap - 1);
    }
    return i;
}

bool wpm_init(Arena *a, const Gguf *g, Wpm *w, char *err, size_t errsz) {
    memset(w, 0, sizeof(*w));
    const char *model = gguf_str(g, "tokenizer.ggml.model", NULL);
    if (!model) {
        snprintf(err, errsz, "the model file names no tokenizer");
        return false;
    }
    if (strcmp(model, "bert") != 0) {
        snprintf(err, errsz,
                 "tokenizer \"%s\"; this build implements \"bert\" (WordPiece)",
                 model);
        return false;
    }
    if (!gguf_str_array(a, g, "tokenizer.ggml.tokens", &w->tokens, &w->n) ||
        w->n == 0) {
        snprintf(err, errsz, "the model file carries no vocabulary");
        return false;
    }
    if (w->n > 0x7FFFFFFFu) {
        snprintf(err, errsz, "the vocabulary is implausibly large");
        return false;
    }

    uint32_t cap = 1024;
    while ((uint64_t)cap < w->n * 2)
        cap *= 2;
    w->slot_cap = cap;
    w->slots = (uint32_t *)arena_alloc0(a, (size_t)cap * sizeof(uint32_t));
    for (uint64_t i = 0; i < w->n; i++) {
        if (w->tokens[i].len > w->max_token_len)
            w->max_token_len = (uint32_t)w->tokens[i].len;
        uint32_t s = vslot(w->slots, cap, w->tokens, w->tokens[i].ptr,
                           w->tokens[i].len);
        /* First id wins. A vocabulary with a repeated string is malformed,
         * and the alternative — letting the last win — would silently change
         * which embedding row a word resolves to. */
        if (!w->slots[s])
            w->slots[s] = (uint32_t)i + 1;
    }

    w->cls = (int32_t)gguf_u64(g, "tokenizer.ggml.bos_token_id", 101);
    w->sep = (int32_t)gguf_u64(g, "tokenizer.ggml.eos_token_id", 102);
    w->unk = (int32_t)gguf_u64(g, "tokenizer.ggml.unknown_token_id", 100);
    w->pad = (int32_t)gguf_u64(g, "tokenizer.ggml.padding_token_id", 0);
    if (w->cls < 0 || (uint64_t)w->cls >= w->n || w->sep < 0 ||
        (uint64_t)w->sep >= w->n || w->unk < 0 || (uint64_t)w->unk >= w->n) {
        snprintf(err, errsz,
                 "the model file's special token ids fall outside its own "
                 "vocabulary");
        return false;
    }
    return true;
}

int32_t wpm_lookup(const Wpm *w, const char *s, size_t n) {
    if (n == 0 || n > w->max_token_len)
        return -1;
    uint32_t i = vslot(w->slots, w->slot_cap, w->tokens, s, n);
    return w->slots[i] ? (int32_t)(w->slots[i] - 1) : -1;
}

/* ---- stage 2: greedy longest match -------------------------------------- */

#define WPM_MARK "\xe2\x96\x81" /* U+2581 LOWER ONE EIGHTH BLOCK */
#define WPM_MARK_LEN 3u

size_t wpm_encode(Arena *a, const Wpm *w, const char *text, size_t len,
                  bool add_special, int32_t *out, size_t cap,
                  bool *truncated) {
    if (truncated)
        *truncated = false;
    size_t n = 0;
    /* [SEP] is reserved up front so that truncation cannot cost the model its
     * end-of-sequence marker; a sequence that ends mid-text is still a
     * sequence shaped the way the model was trained to see. */
    size_t room = cap;
    if (add_special) {
        if (cap < 2)
            return 0;
        out[n++] = w->cls;
        room = cap - 1;
    }

    Str *words;
    size_t nw = wpm_words(a, text, len, &words);
    char *buf = NULL;
    size_t buf_cap = 0;
    for (size_t i = 0; i < nw; i++) {
        if (n >= room) {
            if (truncated)
                *truncated = true;
            break;
        }
        size_t wl = WPM_MARK_LEN + words[i].len;
        if (wl > buf_cap) {
            buf_cap = wl * 2;
            buf = (char *)arena_alloc(a, buf_cap);
        }
        memcpy(buf, WPM_MARK, WPM_MARK_LEN);
        memcpy(buf + WPM_MARK_LEN, words[i].ptr, words[i].len);

        size_t before = n;
        bool matched = true, full = false;
        for (size_t p = 0; p < wl;) {
            size_t longest = wl - p;
            if (longest > w->max_token_len)
                longest = w->max_token_len;
            int32_t id = -1;
            size_t take = 0;
            for (size_t l = longest; l >= 1; l--) {
                id = wpm_lookup(w, buf + p, l);
                if (id >= 0) {
                    take = l;
                    break;
                }
            }
            /* ALL OR NOTHING PER WORD. A word with an unmatchable position
             * contributes [UNK] and none of the pieces that did match: those
             * pieces are evidence about a prefix, not about the word, and
             * emitting them would put the model's attention on a fragment
             * that is not in the text. */
            if (id < 0) {
                matched = false;
                break;
            }
            if (n >= room) {
                full = true;
                break;
            }
            out[n++] = id;
            p += take;
        }
        if (full) {
            /* The budget ran out inside this word. Its pieces go back —
             * half a word is not the text — and the sequence ends here. */
            n = before;
            if (truncated)
                *truncated = true;
            break;
        }
        if (!matched) {
            n = before;
            out[n++] = w->unk;
        }
    }
    if (add_special)
        out[n++] = w->sep;
    return n;
}
