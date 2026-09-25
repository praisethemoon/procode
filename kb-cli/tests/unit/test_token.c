#include "test.h"
#include "token.h"

/* The tokeniser is where §4's argument lives or dies. Every case below is
 * an identifier the spec names, or the rule that keeps one intact. */

typedef struct {
    Arena *a;
    const char **text;
    bool *primary;
    size_t n, cap;
} Seen;

static void see(const Token *t, void *ud) {
    Seen *s = (Seen *)ud;
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 16;
        s->text = (const char **)arena_realloc(
            s->a, s->text, s->cap * sizeof(char *), nc * sizeof(char *));
        s->primary = (bool *)arena_realloc(s->a, s->primary,
                                           s->cap * sizeof(bool),
                                           nc * sizeof(bool));
        s->cap = nc;
    }
    s->text[s->n] = arena_strdup(s->a, t->text);
    s->primary[s->n] = t->primary;
    s->n++;
}

static Seen scan(Arena *a, const char *text) {
    Seen s;
    memset(&s, 0, sizeof s);
    s.a = a;
    token_scan(text, strlen(text), see, &s);
    return s;
}

static bool has_term(const Seen *s, const char *term) {
    for (size_t i = 0; i < s->n; i++) {
        if (strcmp(s->text[i], term) == 0)
            return true;
    }
    return false;
}

static bool has_primary(const Seen *s, const char *term) {
    for (size_t i = 0; i < s->n; i++) {
        if (s->primary[i] && strcmp(s->text[i], term) == 0)
            return true;
    }
    return false;
}

static size_t primaries(const Seen *s) {
    size_t n = 0;
    for (size_t i = 0; i < s->n; i++)
        n += s->primary[i] ? 1u : 0u;
    return n;
}

static void test_identifiers(Arena *a) {
    t_begin("token: an underscored identifier stays one term");
    /* The property §4's whole design rests on: this must not become four
     * words, or io_uring_prep_recv and io_uring_prep_send stop being
     * distinguishable by keyword retrieval. */
    Seen s = scan(a, "io_uring_prep_recv queues a receive");
    ASSERT_TRUE(has_primary(&s, "io_uring_prep_recv"));
    ASSERT_TRUE(!has_primary(&s, "io"));
    ASSERT_TRUE(!has_primary(&s, "uring"));
    ASSERT_EQ_I(primaries(&s), 4); /* the identifier + queues a receive */

    t_begin("token: its parts are indexed too, as non-primary terms");
    ASSERT_TRUE(has_term(&s, "io"));
    ASSERT_TRUE(has_term(&s, "uring"));
    ASSERT_TRUE(has_term(&s, "prep"));
    ASSERT_TRUE(has_term(&s, "recv"));

    t_begin("token: a SHOUTING identifier folds and keeps its underscores");
    s = scan(a, "IORING_SETUP_SQPOLL");
    ASSERT_TRUE(has_primary(&s, "ioring_setup_sqpoll"));
    ASSERT_TRUE(has_term(&s, "ioring"));
    ASSERT_TRUE(has_term(&s, "setup"));
    ASSERT_TRUE(has_term(&s, "sqpoll"));

    s = scan(a, "EVFILT_READ");
    ASSERT_TRUE(has_primary(&s, "evfilt_read"));
    ASSERT_TRUE(has_term(&s, "evfilt"));
    ASSERT_TRUE(has_term(&s, "read"));

    t_begin("token: camelCase yields the whole name and its words");
    s = scan(a, "CreateIoCompletionPort");
    ASSERT_EQ_I(primaries(&s), 1);
    ASSERT_TRUE(has_primary(&s, "createiocompletionport"));
    ASSERT_TRUE(has_term(&s, "create"));
    ASSERT_TRUE(has_term(&s, "io"));
    ASSERT_TRUE(has_term(&s, "completion"));
    ASSERT_TRUE(has_term(&s, "port"));

    t_begin("token: a run of capitals keeps its last letter for the next word");
    /* IOCPHandle is IOCP + Handle. Splitting at every capital would make it
     * IOCPH + andle, which is not a word anyone will ever search for. */
    s = scan(a, "IOCPHandle");
    ASSERT_TRUE(has_term(&s, "iocp"));
    ASSERT_TRUE(has_term(&s, "handle"));
    ASSERT_TRUE(!has_term(&s, "iocph"));

    t_begin("token: digits belong to their identifier and also split out");
    s = scan(a, "int32_t sha256");
    ASSERT_TRUE(has_primary(&s, "int32_t"));
    ASSERT_TRUE(has_primary(&s, "sha256"));
    ASSERT_TRUE(has_term(&s, "int"));
    ASSERT_TRUE(has_term(&s, "32"));
    ASSERT_TRUE(has_term(&s, "sha"));
    ASSERT_TRUE(has_term(&s, "256"));
    /* One letter is not a word worth a posting in every C file. */
    ASSERT_TRUE(!has_term(&s, "t"));

    t_begin("token: a name whose parts are the name is not indexed twice");
    s = scan(a, "port");
    ASSERT_EQ_I(s.n, 1);
    s = scan(a, "Port");
    ASSERT_EQ_I(s.n, 1);
    ASSERT_TRUE(has_primary(&s, "port"));

    t_begin("token: a part that differs from the whole is still emitted");
    s = scan(a, "__init__");
    ASSERT_TRUE(has_primary(&s, "__init__"));
    ASSERT_TRUE(has_term(&s, "init"));
}

static void test_boundaries(Arena *a) {
    t_begin("token: case folds, so a query may be typed either way");
    Seen s = scan(a, "GetQueuedCompletionStatus");
    ASSERT_TRUE(has_primary(&s, "getqueuedcompletionstatus"));

    t_begin("token: punctuation separates, underscores do not");
    s = scan(a, "a-b.c/d e_f");
    ASSERT_TRUE(has_primary(&s, "a"));
    ASSERT_TRUE(has_primary(&s, "b"));
    ASSERT_TRUE(has_primary(&s, "c"));
    ASSERT_TRUE(has_primary(&s, "d"));
    ASSERT_TRUE(has_primary(&s, "e_f"));
    ASSERT_TRUE(!has_primary(&s, "a-b"));

    t_begin("token: a one-letter word the author wrote is kept");
    /* Dropping it would make `kb search "the C ABI"` unable to find C. */
    s = scan(a, "the C ABI");
    ASSERT_TRUE(has_primary(&s, "c"));

    t_begin("token: a compound contributes one primary and several parts");
    s = scan(a, "xx io_uring_prep_recv");
    ASSERT_EQ_I(primaries(&s), 2);
    ASSERT_EQ_I(s.n - primaries(&s), 4);

    t_begin("token: bytes above ASCII hold a word together");
    /* No Unicode tables in this binary, so a non-ASCII word is indexed by
     * its bytes rather than shredded one byte per term. */
    s = scan(a, "caf\xc3\xa9 au lait");
    ASSERT_EQ_I(primaries(&s), 3);
    ASSERT_TRUE(has_primary(&s, "caf\xc3\xa9"));
}

/* Offsets, checked on their own so the callback can be a plain function. */
typedef struct {
    size_t ident_off;
    size_t part_off;
    bool seen_ident, seen_part;
} OffSeen;

static void off_visit(const Token *t, void *ud) {
    OffSeen *o = (OffSeen *)ud;
    if (t->primary && strcmp(t->text, "io_uring_prep_recv") == 0) {
        o->ident_off = t->off;
        o->seen_ident = true;
    }
    if (!t->primary && strcmp(t->text, "recv") == 0) {
        o->part_off = t->off;
        o->seen_part = true;
    }
}

static void test_offsets(void) {
    t_begin("token: a part reports the offset of the compound it came from");
    /* A snippet that matched `recv` has to show `io_uring_prep_recv`, not a
     * fragment starting fourteen bytes into it. */
    const char *text = "xx io_uring_prep_recv";
    OffSeen o;
    memset(&o, 0, sizeof o);
    token_scan(text, strlen(text), off_visit, &o);
    ASSERT_TRUE(o.seen_ident);
    ASSERT_TRUE(o.seen_part);
    ASSERT_EQ_I(o.ident_off, 3);
    ASSERT_EQ_I(o.part_off, 3);
}

static void test_limits(Arena *a) {
    t_begin("token: an over-long token is truncated, not dropped");
    char big[200];
    memset(big, 'z', sizeof big);
    big[sizeof big - 1] = '\0';
    Seen s = scan(a, big);
    ASSERT_EQ_I(s.n, 1);
    ASSERT_EQ_I(strlen(s.text[0]), KB_TERM_MAX);

    t_begin("token: query terms are distinct and in first-seen order");
    TermList l = token_terms(a, "alpha beta alpha", 16);
    ASSERT_EQ_I(l.n, 2);
    ASSERT_EQ_S(l.v[0], "alpha");
    ASSERT_EQ_S(l.v[1], "beta");
    ASSERT_EQ_I(termlist_find(&l, "beta"), 1);
    ASSERT_EQ_I(termlist_find(&l, "gamma"), -1);

    t_begin("token: a query's terms include the compound and its parts");
    l = token_terms(a, "io_uring_prep_recv", 18);
    ASSERT_EQ_S(l.v[0], "io_uring_prep_recv");
    ASSERT_TRUE(termlist_find(&l, "recv") > 0);

    t_begin("token: a query with nothing in it has no terms");
    l = token_terms(a, "!!! ... ---", 11);
    ASSERT_EQ_I(l.n, 0);

    t_begin("token: the query term list is capped");
    StrBuf sb;
    sb_init(&sb, a);
    for (int32_t i = 0; i < 200; i++)
        sb_printf(&sb, "w%d ", i);
    char *many = sb_finish(&sb);
    l = token_terms(a, many, strlen(many));
    ASSERT_TRUE(l.n <= KB_QUERY_TERMS_MAX);
}

void test_token(void) {
    Arena *a = arena_new(1 << 16);
    test_identifiers(a);
    test_boundaries(a);
    test_offsets();
    test_limits(a);
    arena_free(a);
}
