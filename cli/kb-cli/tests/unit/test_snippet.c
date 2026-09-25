#include "snippet.h"
#include "test.h"

/* What a result list is allowed to contain, and where in the passage it
 * comes from. */

static char *snip(Arena *a, const char *text, const char *query) {
    TermList q = token_terms(a, query, strlen(query));
    return snippet_of(a, text, 0, strlen(text), &q);
}

static void test_placement(Arena *a) {
    t_begin("snippet: the window lands on the query term, not the top");
    /* 400 bytes of filler, then the word asked about. A window taken from
     * the head of the chunk would show none of it. */
    StrBuf sb;
    sb_init(&sb, a);
    for (int32_t i = 0; i < 40; i++)
        sb_puts(&sb, "filler words that say nothing. ");
    sb_puts(&sb, "IORING_SETUP_SQPOLL lets the kernel poll. ");
    for (int32_t i = 0; i < 40; i++)
        sb_puts(&sb, "more filler that says nothing. ");
    char *text = sb_finish(&sb);
    char *s = snip(a, text, "IORING_SETUP_SQPOLL");
    ASSERT_TRUE(strstr(s, "IORING_SETUP_SQPOLL") != NULL);

    t_begin("snippet: a cut end is marked");
    ASSERT_TRUE(strncmp(s, "\xe2\x80\xa6", 3) == 0);
    ASSERT_TRUE(strlen(s) >= 3 &&
                strcmp(s + strlen(s) - 3, "\xe2\x80\xa6") == 0);

    t_begin("snippet: a passage that fits whole is not marked as cut");
    s = snip(a, "io_uring_prep_recv queues a receive.", "recv");
    ASSERT_EQ_S(s, "io_uring_prep_recv queues a receive.");

    t_begin("snippet: the densest window wins, not the first match");
    /* `port` alone early, `port` and `handle` together late. The window
     * holding two distinct terms is the one a reader wants. */
    sb_init(&sb, a);
    sb_puts(&sb, "port appears here on its own. ");
    for (int32_t i = 0; i < 20; i++)
        sb_puts(&sb, "padding padding padding padding. ");
    sb_puts(&sb, "the port and the handle together. ");
    text = sb_finish(&sb);
    s = snip(a, text, "port handle");
    ASSERT_TRUE(strstr(s, "the port and the handle together") != NULL);

    t_begin("snippet: a query that matches nothing shows the head");
    s = snip(a, "nothing in here is asked about.", "zzz");
    ASSERT_EQ_S(s, "nothing in here is asked about.");

    t_begin("snippet: an empty query shows the head");
    TermList empty;
    empty.v = NULL;
    empty.n = 0;
    s = snippet_of(a, "some text", 0, 9, &empty);
    ASSERT_EQ_S(s, "some text");
}

static void test_shape(Arena *a) {
    t_begin("snippet: newlines and runs of blanks become single spaces");
    /* A result list whose entries can contain newlines is a result list
     * whose entries can be made to look like other entries. */
    char *s = snip(a, "# Heading\n\n  body\ttext\r\nhere\n", "body");
    ASSERT_EQ_S(s, "# Heading body text here");
    ASSERT_TRUE(strchr(s, '\n') == NULL);
    ASSERT_TRUE(strchr(s, '\t') == NULL);

    t_begin("snippet: the budget is KB_SNIPPET_BYTES plus its markers");
    StrBuf sb;
    sb_init(&sb, a);
    for (int32_t i = 0; i < 400; i++)
        sb_puts(&sb, "word ");
    char *text = sb_finish(&sb);
    s = snip(a, text, "word");
    /* 240 bytes of passage, plus at most two three-byte ellipses. */
    ASSERT_TRUE(strlen(s) <= KB_SNIPPET_BYTES + 6);

    t_begin("snippet: only a span of the passage is returned");
    ASSERT_TRUE(strlen(s) < strlen(text) / 2);
}

/* Every byte of s must belong to a complete UTF-8 sequence. */
static bool utf8_whole(const char *s) {
    size_t i = 0, n = strlen(s);
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t need = c < 0x80 ? 1
                               : (c < 0xC0 ? 0 : (c < 0xE0 ? 2
                                                           : (c < 0xF0 ? 3 : 4)));
        if (need == 0 || i + need > n)
            return false;
        for (size_t k = 1; k < need; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80)
                return false;
        }
        i += need;
    }
    return true;
}

static void test_utf8(Arena *a) {
    t_begin("snippet: a passage cut mid-character does not carry the "
            "fragment");
    /* This is the chunk-boundary case: chunk_split works in bytes, so the
     * END of a passage can fall inside a character. The window stops at the
     * passage's end, so nothing downstream would trim it. */
    const char *text = "abc\xe4\xb8\x80 def";
    TermList q = token_terms(a, "abc", 3);
    char *s = snippet_of(a, text, 0, 5, &q); /* 5 splits U+4E00 */
    /* The half character is dropped and the passage is marked as cut, which
     * it is — it continues past what is shown. */
    ASSERT_EQ_S(s, "abc\xe2\x80\xa6");
    ASSERT_TRUE(utf8_whole(s));

    t_begin("snippet: a complete character at the passage's end is kept");
    s = snippet_of(a, text, 0, 6, &q);
    ASSERT_EQ_S(s, "abc\xe4\xb8\x80");
    ASSERT_TRUE(utf8_whole(s));

    t_begin("snippet: a passage beginning mid-character drops the stray byte");
    /* And this is the same boundary at the other end. */
    q = token_terms(a, "tail", 4);
    const char *whole = "\xe4\xb8\x80 tail";
    char *cut = snippet_of(a, whole, 1, strlen(whole), &q);
    ASSERT_TRUE(((unsigned char)cut[0] & 0xC0) != 0x80);
    ASSERT_TRUE(strstr(cut, "tail") != NULL);
    ASSERT_TRUE(utf8_whole(cut));

    t_begin("snippet: the byte budget never splits a character either");
    StrBuf sb;
    sb_init(&sb, a);
    sb_puts(&sb, "x");
    for (int32_t i = 0; i < 300; i++)
        sb_puts(&sb, "\xe4\xb8\x80"); /* U+4E00 */
    char *many = sb_finish(&sb);
    ASSERT_TRUE(utf8_whole(snip(a, many, "zzz")));
    /* Offset the passage by one so the 240-byte window lands elsewhere in
     * the three-byte cycle. */
    ASSERT_TRUE(utf8_whole(snippet_of(a, many, 1, strlen(many), &q)));
    ASSERT_TRUE(utf8_whole(snippet_of(a, many, 2, strlen(many) - 1, &q)));
    ASSERT_TRUE(utf8_whole(snippet_of(a, many, 0, strlen(many) - 2, &q)));
}

void test_snippet(void) {
    Arena *a = arena_new(1 << 16);
    test_placement(a);
    test_shape(a);
    test_utf8(a);
    arena_free(a);
}
