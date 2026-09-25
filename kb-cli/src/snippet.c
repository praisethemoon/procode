#include "snippet.h"

#include "kb.h"

/* Where in the passage each query term occurs. A compound and the parts
 * split out of it report the same offset (token.h), so a match on `recv`
 * places the window on `io_uring_prep_recv` rather than fourteen bytes into
 * it. */
typedef struct {
    Arena *a;
    const TermList *q;
    size_t *pos;
    int32_t *term;
    size_t n, cap;
} Matches;

static void match_visit(const Token *t, void *ud) {
    Matches *m = (Matches *)ud;
    int32_t idx = termlist_find(m->q, t->text);
    if (idx < 0)
        return;
    if (m->n == m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 16;
        m->pos = (size_t *)arena_realloc(m->a, m->pos, m->cap * sizeof(size_t),
                                         nc * sizeof(size_t));
        m->term = (int32_t *)arena_realloc(
            m->a, m->term, m->cap * sizeof(int32_t), nc * sizeof(int32_t));
        m->cap = nc;
    }
    m->pos[m->n] = t->off;
    m->term[m->n] = idx;
    m->n++;
}

static bool utf8_tail(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

char *snippet_of(Arena *a, const char *text, size_t start, size_t end,
                 const TermList *q) {
    const char *body = text + start;
    size_t len = end > start ? end - start : 0;
    Matches m;
    memset(&m, 0, sizeof m);
    m.a = a;
    m.q = q;
    if (q->n)
        token_scan(body, len, match_visit, &m);

    size_t from = 0;
    if (m.n) {
        /* The window holding the most distinct terms. Ties go to the
         * earliest, so the reader sees the first place the passage answers
         * the query rather than an arbitrary one. */
        size_t best = 0;
        int32_t best_score = -1;
        for (size_t i = 0; i < m.n; i++) {
            uint64_t mask = 0;
            for (size_t j = i;
                 j < m.n && m.pos[j] < m.pos[i] + KB_SNIPPET_BYTES; j++)
                mask |= (uint64_t)1 << (m.term[j] & 63);
            int32_t score = 0;
            for (uint64_t bits = mask; bits; bits &= bits - 1)
                score++;
            if (score > best_score) {
                best_score = score;
                best = i;
            }
        }
        /* A little run-up, so the match is not flush against the left edge
         * and the sentence it sits in is readable. A match already near the
         * top of the passage starts at the top instead: an ellipsis
         * standing in for a heading marker tells the reader nothing. */
        from = m.pos[best];
        if (from < 64) {
            from = 0;
        } else {
            size_t back = from - 48;
            while (back < from && body[back] != ' ' && body[back] != '\n')
                back++;
            from = back < from ? back + 1 : from;
        }
    }
    while (from < len && utf8_tail((unsigned char)body[from]))
        from++;
    size_t to = from + KB_SNIPPET_BYTES;
    if (to > len)
        to = len;
    while (to > from && to < len && utf8_tail((unsigned char)body[to]))
        to--;
    to -= utf8_dangling(body + from, to - from);

    StrBuf sb;
    sb_init(&sb, a);
    if (from > 0)
        sb_puts(&sb, "\xe2\x80\xa6"); /* U+2026 */
    bool space = false, any = false;
    for (size_t i = from; i < to; i++) {
        unsigned char c = (unsigned char)body[i];
        if (c <= ' ') {
            space = any;
            continue;
        }
        if (space)
            sb_putc(&sb, ' ');
        space = false;
        sb_putc(&sb, (char)c);
        any = true;
    }
    if (to < len)
        sb_puts(&sb, "\xe2\x80\xa6");
    return sb_finish(&sb);
}
