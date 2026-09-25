#include "token.h"

/* ---- character classes ------------------------------------------------- */

static bool is_upper(unsigned char c) {
    return c >= 'A' && c <= 'Z';
}
static bool is_lower(unsigned char c) {
    return c >= 'a' && c <= 'z';
}
static bool is_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

/* What may appear inside a token. '_' is here deliberately (see token.h);
 * so is every byte >= 0x80, so that a word in another script forms one term
 * instead of being shredded into individual UTF-8 bytes. */
static bool tok_byte(unsigned char c) {
    return is_lower(c) || is_upper(c) || is_digit(c) || c == '_' || c >= 0x80;
}

static char fold(unsigned char c) {
    return is_upper(c) ? (char)(c - 'A' + 'a') : (char)c;
}

/* ---- emission ---------------------------------------------------------- */

static void emit(TokenFn fn, void *ud, const char *p, size_t n, size_t off,
                 bool primary) {
    Token t;
    if (n > KB_TERM_MAX)
        n = KB_TERM_MAX;
    for (size_t i = 0; i < n; i++)
        t.text[i] = fold((unsigned char)p[i]);
    t.text[n] = '\0';
    t.len = (uint32_t)n;
    t.off = off;
    t.primary = primary;
    fn(&t, ud);
}

/* Splits a compound into its parts. The boundaries are the four a reader
 * would name: an underscore run, a rise into upper case (`prepRecv`), the
 * last capital of a run when a lower-case letter follows it (`IOCPHandle`
 * is IOCP + Handle, not IOCPH + andle), and any letter/digit transition
 * (`sha256` is sha + 256).
 *
 * A part is emitted only when it differs from the whole token, so `foo`
 * yields nothing extra while `__init__` still yields `init`. Parts shorter
 * than two bytes are dropped: `int32_t`'s `t` is not a word anyone searches
 * for and it would cost a posting in every C file in the store. A primary
 * token of one byte is kept, because that one is what the author wrote.
 */
static void split_parts(TokenFn fn, void *ud, const char *p, size_t n,
                        size_t off, const char *whole) {
    char seen[KB_SUBWORD_MAX][KB_TERM_MAX + 1];
    int32_t nseen = 0;
    size_t i = 0;
    while (i < n && nseen < KB_SUBWORD_MAX) {
        while (i < n && p[i] == '_')
            i++;
        if (i >= n)
            break;
        size_t s = i;
        i++;
        while (i < n && p[i] != '_') {
            unsigned char a = (unsigned char)p[i - 1], b = (unsigned char)p[i];
            bool rise = is_upper(b) && !is_upper(a);
            bool digit_edge = is_digit(a) != is_digit(b);
            bool tail_of_run = is_upper(a) && is_upper(b) && i + 1 < n &&
                               is_lower((unsigned char)p[i + 1]);
            if (rise || digit_edge || tail_of_run)
                break;
            i++;
        }
        size_t len = i - s;
        if (len < 2)
            continue;
        if (len > KB_TERM_MAX)
            len = KB_TERM_MAX;
        char part[KB_TERM_MAX + 1];
        for (size_t k = 0; k < len; k++)
            part[k] = fold((unsigned char)p[s + k]);
        part[len] = '\0';
        if (strcmp(part, whole) == 0)
            continue;
        bool dup = false;
        for (int32_t k = 0; k < nseen && !dup; k++)
            dup = strcmp(seen[k], part) == 0;
        if (dup)
            continue;
        memcpy(seen[nseen++], part, len + 1);
        emit(fn, ud, part, len, off, false);
    }
}

void token_scan(const char *text, size_t len, TokenFn fn, void *ud) {
    size_t i = 0;
    while (i < len) {
        if (!tok_byte((unsigned char)text[i])) {
            i++;
            continue;
        }
        size_t s = i;
        while (i < len && tok_byte((unsigned char)text[i]))
            i++;
        size_t n = i - s;
        size_t keep = n > KB_TERM_MAX ? (size_t)KB_TERM_MAX : n;
        char whole[KB_TERM_MAX + 1];
        for (size_t k = 0; k < keep; k++)
            whole[k] = fold((unsigned char)text[s + k]);
        whole[keep] = '\0';
        emit(fn, ud, whole, keep, s, true);
        split_parts(fn, ud, text + s, n, s, whole);
    }
}

/* ---- query terms ------------------------------------------------------- */

typedef struct {
    Arena *a;
    const char **v;
    size_t n, cap;
} Collect;

static void collect(const Token *t, void *ud) {
    Collect *c = (Collect *)ud;
    if (c->n >= KB_QUERY_TERMS_MAX)
        return;
    for (size_t i = 0; i < c->n; i++) {
        if (strcmp(c->v[i], t->text) == 0)
            return;
    }
    ARENA_GROW(c->a, c->v, c->n, c->cap, const char *);
    c->v[c->n++] = arena_strndup(c->a, t->text, t->len);
}

TermList token_terms(Arena *a, const char *text, size_t len) {
    Collect c;
    memset(&c, 0, sizeof c);
    c.a = a;
    token_scan(text, len, collect, &c);
    TermList l;
    l.v = c.v;
    l.n = c.n;
    return l;
}

int32_t termlist_find(const TermList *l, const char *term) {
    for (size_t i = 0; i < l->n; i++) {
        if (strcmp(l->v[i], term) == 0)
            return (int32_t)i;
    }
    return -1;
}
