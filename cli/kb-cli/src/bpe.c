#include "bpe.h"

#include <stdlib.h>
#include <string.h>

#include "third_party/utf8proc/utf8proc.h"

/* Token types, as GGUF (and llama.cpp) number them. */
enum { TT_NORMAL = 1, TT_CONTROL = 3, TT_USER_DEFINED = 4 };

/* ---- lookups ------------------------------------------------------------ */

static uint32_t slot_of(const Str *toks, const uint32_t *slots, uint32_t cap,
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

static int32_t lookup(const Bpe *b, const char *s, size_t n) {
    uint32_t i = slot_of(b->tokens, b->slots, b->slot_cap, s, n);
    return b->slots[i] ? (int32_t)(b->slots[i] - 1) : -1;
}

static uint64_t pair_key(int32_t l, int32_t r) {
    return ((uint64_t)(uint32_t)(l + 1) << 32) | (uint32_t)(r + 1);
}

static uint32_t pair_slot(const Bpe *b, uint64_t key) {
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    uint32_t i = (uint32_t)(h >> 32) & (b->merge_cap - 1);
    while (b->merge_keys[i] && b->merge_keys[i] != key)
        i = (i + 1) & (b->merge_cap - 1);
    return i;
}

/* GPT-2's byte alphabet: printable Latin-1 bytes stand for themselves, and
 * every other byte is shifted to U+0100 onwards, in byte order. */
static uint32_t byte_char(uint32_t byte) {
    if ((byte >= '!' && byte <= '~') || (byte >= 0xA1 && byte <= 0xAC) || (byte >= 0xAE && byte <= 0xFF))
        return byte;
    uint32_t n = 0;
    for (uint32_t c = 0; c < byte; c++) {
        if (!((c >= '!' && c <= '~') || (c >= 0xA1 && c <= 0xAC) || (c >= 0xAE && c <= 0xFF)))
            n++;
    }
    return 256 + n;
}

bool bpe_init(Arena *a, const Gguf *g, Bpe *b, char *err, size_t errsz) {
    memset(b, 0, sizeof(*b));
    const char *model = gguf_str(g, "tokenizer.ggml.model", NULL);
    if (!model || strcmp(model, "gpt2") != 0) {
        snprintf(err, errsz, "tokenizer \"%s\"; byte-level BPE needs \"gpt2\"", model ? model : "(none)");
        return false;
    }
    int32_t *types = NULL;
    uint64_t ntypes = 0;
    if (!gguf_str_array(a, g, "tokenizer.ggml.tokens", &b->tokens, &b->n) || b->n == 0 ||
        !gguf_i32_array(a, g, "tokenizer.ggml.token_type", &types, &ntypes) || ntypes != b->n) {
        snprintf(err, errsz, "the model file carries no usable vocabulary");
        return false;
    }
    if (b->n > 0x7FFFFFFFu) {
        snprintf(err, errsz, "the vocabulary is implausibly large");
        return false;
    }

    /* Normal tokens only: an added token is matched as raw text before BPE,
     * never produced by a merge. */
    uint32_t cap = 1024;
    while ((uint64_t)cap < b->n * 2)
        cap *= 2;
    b->slot_cap = cap;
    b->slots = (uint32_t *)arena_alloc0(a, (size_t)cap * sizeof(uint32_t));
    uint32_t n_added = 0;
    for (uint64_t i = 0; i < b->n; i++) {
        if (types[i] == TT_CONTROL || types[i] == TT_USER_DEFINED) {
            if (b->tokens[i].len > 0)
                n_added++;
            continue;
        }
        if (types[i] != TT_NORMAL)
            continue;
        uint32_t s = slot_of(b->tokens, b->slots, cap, b->tokens[i].ptr, b->tokens[i].len);
        if (!b->slots[s])
            b->slots[s] = (uint32_t)i + 1;
        if (b->tokens[i].len > b->max_token_bytes)
            b->max_token_bytes = (uint32_t)b->tokens[i].len;
    }

    b->added = (Str *)arena_alloc(a, (size_t)(n_added ? n_added : 1) * sizeof(Str));
    b->added_ids = (int32_t *)arena_alloc(a, (size_t)(n_added ? n_added : 1) * sizeof(int32_t));
    for (uint64_t i = 0; i < b->n; i++) {
        if ((types[i] == TT_CONTROL || types[i] == TT_USER_DEFINED) && b->tokens[i].len > 0) {
            b->added[b->n_added] = b->tokens[i];
            b->added_ids[b->n_added++] = (int32_t)i;
            b->added_first[(unsigned char)b->tokens[i].ptr[0]] = true;
        }
    }

    for (uint32_t byte = 0; byte < 256; byte++) {
        uint8_t buf[4];
        utf8proc_ssize_t len = utf8proc_encode_char((utf8proc_int32_t)byte_char(byte), buf);
        /* The bytes that never occur in UTF-8 (0xC0, 0xC1, 0xF5 and up) may
         * be missing, and are: text reaching BPE is valid UTF-8. Any other
         * missing byte means the file is not a byte-level vocabulary. */
        b->byte_id[byte] = lookup(b, (const char *)buf, (size_t)len);
        bool never_in_utf8 = byte == 0xC0 || byte == 0xC1 || byte >= 0xF5;
        if (b->byte_id[byte] < 0 && !never_in_utf8) {
            snprintf(err, errsz, "the vocabulary lacks the token for byte 0x%02x", byte);
            return false;
        }
    }

    Str *merges = NULL;
    uint64_t nm = 0;
    if (!gguf_str_array(a, g, "tokenizer.ggml.merges", &merges, &nm) || nm == 0) {
        snprintf(err, errsz, "the model file carries no BPE merges");
        return false;
    }
    uint32_t mcap = 1024;
    while ((uint64_t)mcap < nm * 2)
        mcap *= 2;
    b->merge_cap = mcap;
    b->merge_keys = (uint64_t *)arena_alloc0(a, (size_t)mcap * sizeof(uint64_t));
    b->merge_into = (int32_t *)arena_alloc(a, (size_t)mcap * sizeof(int32_t));
    b->merge_rank = (int32_t *)arena_alloc(a, (size_t)mcap * sizeof(int32_t));
    char *joined = (char *)arena_alloc(a, 1024);
    for (uint64_t r = 0; r < nm; r++) {
        const char *m = merges[r].ptr;
        size_t len = merges[r].len;
        const char *sp = memchr(m, ' ', len);
        if (!sp || (size_t)(sp - m) + 1 >= len || len > 1023)
            continue;
        size_t ll = (size_t)(sp - m), rl = len - ll - 1;
        int32_t l = lookup(b, m, ll), rr = lookup(b, sp + 1, rl);
        memcpy(joined, m, ll);
        memcpy(joined + ll, sp + 1, rl);
        int32_t into = lookup(b, joined, ll + rl);
        if (l < 0 || rr < 0 || into < 0)
            continue;
        uint64_t key = pair_key(l, rr);
        uint32_t s = pair_slot(b, key);
        if (!b->merge_keys[s]) {
            b->merge_keys[s] = key;
            b->merge_into[s] = into;
            b->merge_rank[s] = (int32_t)r;
        }
    }

    b->cls = (int32_t)gguf_u64(g, "tokenizer.ggml.cls_token_id", UINT64_MAX);
    b->sep = (int32_t)gguf_u64(g, "tokenizer.ggml.seperator_token_id", UINT64_MAX);
    b->pad = (int32_t)gguf_u64(g, "tokenizer.ggml.padding_token_id", UINT64_MAX);
    b->unk = (int32_t)gguf_u64(g, "tokenizer.ggml.unknown_token_id", UINT64_MAX);
    if (b->cls < 0 || (uint64_t)b->cls >= b->n || b->sep < 0 || (uint64_t)b->sep >= b->n) {
        snprintf(err, errsz, "the model file names no [CLS] and [SEP]");
        return false;
    }
    return true;
}

/* ---- normalisation ------------------------------------------------------ */

/* Invalid UTF-8 becomes U+FFFD, one byte at a time, then NFC. The result is
 * arena-owned. */
static char *normalise(Arena *a, const char *text, size_t len, size_t *out_len) {
    char *clean = (char *)arena_alloc(a, len * 3 + 1);
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t n = utf8proc_iterate((const utf8proc_uint8_t *)text + i, (utf8proc_ssize_t)(len - i), &cp);
        if (n <= 0) {
            memcpy(clean + o, "\xEF\xBF\xBD", 3);
            o += 3;
            i += 1;
        } else {
            memcpy(clean + o, text + i, (size_t)n);
            o += (size_t)n;
            i += (size_t)n;
        }
    }
    utf8proc_uint8_t *nfc = NULL;
    utf8proc_ssize_t m = utf8proc_map((const utf8proc_uint8_t *)clean, (utf8proc_ssize_t)o, &nfc,
                                      (utf8proc_option_t)(UTF8PROC_STABLE | UTF8PROC_COMPOSE));
    if (m < 0 || !nfc) {
        free(nfc);
        *out_len = o;
        return clean;
    }
    char *out = (char *)arena_alloc(a, (size_t)m + 1);
    memcpy(out, nfc, (size_t)m);
    out[m] = '\0';
    free(nfc);
    *out_len = (size_t)m;
    return out;
}

/* ---- the output buffer -------------------------------------------------- */

typedef struct {
    Arena *a;
    int32_t *ids;
    size_t n, cap;
    size_t limit; /* stop collecting past this many; 0 = no limit */
} Ids;

static void push(Ids *o, int32_t id) {
    if (o->limit && o->n >= o->limit)
        return;
    if (o->n == o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 256;
        int32_t *ids = (int32_t *)arena_alloc(o->a, cap * sizeof(int32_t));
        if (o->n)
            memcpy(ids, o->ids, o->n * sizeof(int32_t));
        o->ids = ids;
        o->cap = cap;
    }
    o->ids[o->n++] = id;
}

static bool full(const Ids *o) {
    return o->limit && o->n >= o->limit;
}

/* ---- BPE on one piece --------------------------------------------------- */

static void bpe_piece(const Bpe *b, Arena *a, const char *p, size_t n, Ids *out) {
    int32_t *sym = (int32_t *)arena_alloc(a, (n ? n : 1) * sizeof(int32_t));
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (b->byte_id[(uint8_t)p[i]] >= 0)
            sym[k++] = b->byte_id[(uint8_t)p[i]];
    }
    for (;;) {
        int32_t best = -1, best_rank = 0x7FFFFFFF;
        int32_t best_l = 0, best_r = 0;
        for (size_t i = 0; i + 1 < k; i++) {
            uint32_t s = pair_slot(b, pair_key(sym[i], sym[i + 1]));
            if (b->merge_keys[s] && b->merge_rank[s] < best_rank) {
                best_rank = b->merge_rank[s];
                best = b->merge_into[s];
                best_l = sym[i];
                best_r = sym[i + 1];
            }
        }
        if (best < 0)
            break;
        /* Every occurrence of that pair, left to right, in one pass. */
        size_t w = 0;
        for (size_t i = 0; i < k;) {
            if (i + 1 < k && sym[i] == best_l && sym[i + 1] == best_r) {
                sym[w++] = best;
                i += 2;
            } else {
                sym[w++] = sym[i++];
            }
        }
        k = w;
    }
    for (size_t i = 0; i < k; i++)
        push(out, sym[i]);
}

/* ---- the GPT-2 split ---------------------------------------------------- */

static bool is_letter(utf8proc_int32_t c) {
    utf8proc_category_t k = utf8proc_category(c);
    return k >= UTF8PROC_CATEGORY_LU && k <= UTF8PROC_CATEGORY_LO;
}

static bool is_number(utf8proc_int32_t c) {
    utf8proc_category_t k = utf8proc_category(c);
    return k == UTF8PROC_CATEGORY_ND || k == UTF8PROC_CATEGORY_NL || k == UTF8PROC_CATEGORY_NO;
}

/* Oniguruma's `\s` under Unicode: the White_Space property. */
static bool is_space(utf8proc_int32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000;
}

void bpe_strip(const char **s, size_t *n) {
    const utf8proc_uint8_t *p = (const utf8proc_uint8_t *)*s;
    size_t lo = 0, hi = *n;
    while (lo < hi) {
        utf8proc_int32_t c;
        utf8proc_ssize_t k = utf8proc_iterate(p + lo, (utf8proc_ssize_t)(hi - lo), &c);
        if (k <= 0 || !(is_space(c) || (c >= 0x1C && c <= 0x1F)))
            break;
        lo += (size_t)k;
    }
    while (hi > lo) {
        /* Back up to the start of the last codepoint. */
        size_t b = hi - 1;
        while (b > lo && (p[b] & 0xC0) == 0x80 && hi - b < 4)
            b--;
        utf8proc_int32_t c;
        utf8proc_ssize_t k = utf8proc_iterate(p + b, (utf8proc_ssize_t)(hi - b), &c);
        if (k != (utf8proc_ssize_t)(hi - b) || !(is_space(c) || (c >= 0x1C && c <= 0x1F)))
            break;
        hi = b;
    }
    *s += lo;
    *n = hi - lo;
}

static bool is_other(utf8proc_int32_t c) {
    return !is_space(c) && !is_letter(c) && !is_number(c);
}

/* The codepoint at byte offset i (already valid UTF-8), and its length. */
static utf8proc_int32_t cp_at(const char *s, size_t n, size_t i, size_t *len) {
    utf8proc_int32_t c = -1;
    utf8proc_ssize_t k = utf8proc_iterate((const utf8proc_uint8_t *)s + i, (utf8proc_ssize_t)(n - i), &c);
    *len = k > 0 ? (size_t)k : 1;
    return k > 0 ? c : 0xFFFD;
}

/* The end of a run of codepoints of one class starting at i. */
static size_t run_end(const char *s, size_t n, size_t i, bool (*cls)(utf8proc_int32_t)) {
    while (i < n) {
        size_t l;
        utf8proc_int32_t c = cp_at(s, n, i, &l);
        if (!cls(c))
            break;
        i += l;
    }
    return i;
}

static void split_and_merge(const Bpe *b, Arena *a, const char *s, size_t n, Ids *out) {
    static const char *const contractions[] = {"s", "t", "re", "ve", "m", "ll", "d"};
    size_t i = 0;
    while (i < n && !full(out)) {
        size_t l;
        utf8proc_int32_t c = cp_at(s, n, i, &l);
        size_t end = i;

        if (c == '\'') {
            for (size_t k = 0; k < 7; k++) {
                size_t cl = strlen(contractions[k]);
                if (i + 1 + cl <= n && memcmp(s + i + 1, contractions[k], cl) == 0) {
                    end = i + 1 + cl;
                    break;
                }
            }
        }
        if (end == i) {
            /* ` ?\p{L}+`, ` ?\p{N}+`, ` ?[^\s\p{L}\p{N}]+`: an optional
             * literal space, then a run of one class. */
            size_t start = i;
            utf8proc_int32_t first = c;
            if (c == ' ' && i + 1 < n) {
                size_t l2;
                first = cp_at(s, n, i + 1, &l2);
                start = i + 1;
            }
            if (is_letter(first))
                end = run_end(s, n, start, is_letter);
            else if (is_number(first))
                end = run_end(s, n, start, is_number);
            else if (is_other(first))
                end = run_end(s, n, start, is_other);
            if (end == start)
                end = i; /* the space was not followed by any of them */
        }
        if (end == i && is_space(c)) {
            /* `\s+(?!\S)`, else `\s+`: a whitespace run, minus its last
             * character when a non-space follows and the run has more than
             * one, so that character can lead the next piece. */
            size_t run = run_end(s, n, i, is_space);
            if (run < n) {
                size_t last = i, k = i;
                while (k < run) {
                    last = k;
                    size_t l3;
                    cp_at(s, n, k, &l3);
                    k += l3;
                }
                end = last > i ? last : run;
            } else {
                end = run;
            }
        }
        if (end == i)
            end = i + l; /* unreachable for valid input; never loop */
        /* WORK IS BOUNDED BY WHAT CAN BE KEPT. When the output will be cut
         * at `limit` ids, no more than (room + 1) × the longest token's
         * bytes of this piece can reach it, so only that much is merged —
         * a megabyte of minified text is not a quadratic merge over a
         * megabyte. Nothing that is kept changes unless the output was going
         * to be truncated anyway. */
        size_t piece = end - i;
        if (out->limit) {
            size_t room = out->limit > out->n ? out->limit - out->n : 0;
            size_t most = (room + 1) * (size_t)(b->max_token_bytes ? b->max_token_bytes : 1);
            if (piece > most) {
                size_t cut = i + most;
                while (cut > i && ((unsigned char)s[cut] & 0xC0) == 0x80)
                    cut--; /* a codepoint boundary */
                piece = cut - i;
            }
        }
        bpe_piece(b, a, s + i, piece, out);
        i = end;
    }
}

/* ---- added tokens, then the rest ---------------------------------------- */

static void encode_into(Arena *a, const Bpe *b, const char *text, size_t len, Ids *out) {
    size_t n;
    const char *s = normalise(a, text, len, &n);
    size_t seg = 0;
    for (size_t i = 0; i < n && !full(out);) {
        int32_t best = -1;
        size_t best_len = 0;
        unsigned char c0 = (unsigned char)s[i];
        if (b->added_first[c0]) {
            for (uint32_t k = 0; k < b->n_added; k++) {
                const Str *t = &b->added[k];
                if (t->len > best_len && t->len <= n - i && (unsigned char)t->ptr[0] == c0 &&
                    memcmp(s + i, t->ptr, t->len) == 0) {
                    best = b->added_ids[k];
                    best_len = t->len;
                }
            }
        }
        if (best < 0) {
            i++;
            continue;
        }
        if (i > seg)
            split_and_merge(b, a, s + seg, i - seg, out);
        push(out, best);
        i += best_len;
        seg = i;
    }
    if (seg < n)
        split_and_merge(b, a, s + seg, n - seg, out);
}

size_t bpe_encode(Arena *a, const Bpe *b, const char *text, size_t len,
                  bool add_special, int32_t *out, size_t cap, bool *truncated) {
    *truncated = false;
    if (cap == 0)
        return 0;
    size_t room = add_special ? (cap >= 2 ? cap - 2 : 0) : cap;
    Ids ids = {a, NULL, 0, 0, room + 1};
    encode_into(a, b, text, len, &ids);
    if (ids.n > room) {
        *truncated = true;
        ids.n = room;
    }
    size_t o = 0;
    if (add_special && cap >= 2)
        out[o++] = b->cls;
    for (size_t i = 0; i < ids.n; i++)
        out[o++] = ids.ids[i];
    if (add_special && cap >= 2)
        out[o++] = b->sep;
    return o;
}

size_t bpe_encode_pair(Arena *a, const Bpe *b, const char *x, size_t xlen,
                       const char *y, size_t ylen, int32_t *out, size_t cap,
                       bool *truncated) {
    *truncated = false;
    if (cap < 3)
        return 0;
    size_t room = cap - 3;
    Ids q = {a, NULL, 0, 0, room + 1};
    encode_into(a, b, x, xlen, &q);
    Ids p = {a, NULL, 0, 0, room + 1};
    encode_into(a, b, y, ylen, &p);
    size_t qn = q.n, pn = p.n;
    if (qn + pn > room) {
        *truncated = true;
        pn = qn >= room ? 0 : room - qn;
        if (qn > room)
            qn = room;
    }
    size_t o = 0;
    out[o++] = b->cls;
    for (size_t i = 0; i < qn; i++)
        out[o++] = q.ids[i];
    out[o++] = b->sep;
    for (size_t i = 0; i < pn; i++)
        out[o++] = p.ids[i];
    out[o++] = b->sep;
    return o;
}
