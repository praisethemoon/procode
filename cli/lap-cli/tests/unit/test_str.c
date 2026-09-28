#include "str.h"
#include "test.h"

void test_str(void) {
    Arena *a = arena_new(0);

    t_begin("str: eq and find");
    ASSERT_TRUE(str_eq(str_c("abc"), str_c("abc")));
    ASSERT_TRUE(!str_eq(str_c("abc"), str_c("abd")));
    ASSERT_TRUE(str_eq(str_c(""), str_c("")));
    ASSERT_EQ_I(str_find(str_c("hello world"), str_c("world")), 6);
    ASSERT_EQ_I(str_find(str_c("hello"), str_c("x")), -1);
    ASSERT_EQ_I(str_find(str_c("aaab"), str_c("aab")), 1);
    ASSERT_EQ_I(str_find(str_c("abc"), str_c("")), 0);
    ASSERT_EQ_I(str_find(str_c("ab"), str_c("abc")), -1);

    t_begin("str: builder");
    StrBuf sb;
    sb_init(&sb, a);
    sb_puts(&sb, "a");
    sb_putc(&sb, 'b');
    sb_printf(&sb, "%d%s", 1, "c");
    ASSERT_EQ_S(sb_finish(&sb), "ab1c");
    ASSERT_EQ_I(sb.len, 4);

    t_begin("str: split_lines basics");
    Lines l = split_lines(a, "a\nb\nc\n", 6);
    ASSERT_EQ_I(l.count, 3);
    ASSERT_TRUE(l.eof_nl);
    ASSERT_TRUE(str_eq_c(l.lines[0], "a"));
    ASSERT_TRUE(str_eq_c(l.lines[2], "c"));

    t_begin("str: split_lines without trailing newline");
    l = split_lines(a, "a\nb", 3);
    ASSERT_EQ_I(l.count, 2);
    ASSERT_TRUE(!l.eof_nl);
    ASSERT_TRUE(str_eq_c(l.lines[1], "b"));

    t_begin("str: split_lines empty and blank lines");
    l = split_lines(a, "", 0);
    ASSERT_EQ_I(l.count, 0);
    ASSERT_TRUE(l.eof_nl);
    l = split_lines(a, "\n\n", 2);
    ASSERT_EQ_I(l.count, 2);
    ASSERT_TRUE(str_eq_c(l.lines[0], ""));

    t_begin("str: CRLF is preserved byte-faithfully");
    l = split_lines(a, "a\r\nb\r\n", 6);
    ASSERT_EQ_I(l.count, 2);
    ASSERT_TRUE(str_eq_c(l.lines[0], "a\r"));

    t_begin("str: join_lines round-trips");
    const char *cases[] = {"a\nb\nc\n", "a\nb", "", "\n", "x\r\ny\r\n"};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        size_t n = strlen(cases[i]);
        Lines split = split_lines(a, cases[i], n);
        size_t out_len;
        char *joined = join_lines(a, split, &out_len);
        ASSERT_EQ_I(out_len, n);
        ASSERT_EQ_S(joined, cases[i]);
    }

    t_begin("str: set add/has across growth");
    StrSet set;
    strset_init(&set, a);
    char key[32];
    for (int32_t i = 0; i < 500; i++) {
        snprintf(key, sizeof key, "path/to/file-%d.c", i);
        ASSERT_TRUE(strset_add(&set, key));
    }
    for (int32_t i = 0; i < 500; i++) {
        snprintf(key, sizeof key, "path/to/file-%d.c", i);
        ASSERT_TRUE(strset_has(&set, key));
        ASSERT_TRUE(!strset_add(&set, key));
    }
    ASSERT_TRUE(!strset_has(&set, "path/to/file-500.c"));

    t_begin("str: hash is conformant FNV-1a 64 (published vectors)");
    /* Dispersion alone cannot catch a wrong offset basis — a mistyped one
     * mixes just as well. Only published vectors pin the claim. */
    ASSERT_TRUE(str_hash(str_c("")) == 0xcbf29ce484222325ULL);
    ASSERT_TRUE(str_hash(str_c("a")) == 0xaf63dc4c8601ec8cULL);
    ASSERT_TRUE(str_hash(str_c("foobar")) == 0x85944171f73967e8ULL);

    t_begin("str: hash disperses");
    ASSERT_TRUE(str_hash(str_c("a")) != str_hash(str_c("b")));
    ASSERT_TRUE(str_hash(str_c("")) != str_hash(str_c(" ")));

    t_begin("str: StrMap puts and gets, the last put winning, and grows");
    StrMap m;
    memset(&m, 0, sizeof m);
    ASSERT_TRUE(strmap_get(&m, "x") == NULL);
    strmap_put(a, &m, "x", "1");
    strmap_put(a, &m, "x", "2");
    ASSERT_EQ_S(strmap_get(&m, "x"), "2");
    char *keys[200];
    for (int32_t i = 0; i < 200; i++) {
        keys[i] = arena_printf(a, "k%d", i);
        strmap_put(a, &m, keys[i], keys[i]);
    }
    ASSERT_EQ_I((int32_t)m.n, 201);
    ASSERT_EQ_S(strmap_get(&m, "k0"), "k0");
    ASSERT_EQ_S(strmap_get(&m, "k199"), "k199");
    ASSERT_TRUE(strmap_get(&m, "k200") == NULL);

    t_begin("str: a chain of copies followed through a StrMap reaches the "
            "original");
    StrMap from;
    memset(&from, 0, sizeof from);
    strmap_put(a, &from, "main-copy", "b1-copy"); /* copied from b1's copy */
    strmap_put(a, &from, "b1-copy", "b2-orig");   /* which came from b2 */
    const char *f = "main-copy", *last = NULL;
    int32_t hops = 0;
    for (; f; f = strmap_get(&from, f), hops++)
        last = f;
    ASSERT_EQ_S(last, "b2-orig");
    ASSERT_EQ_I(hops, 3);

    t_begin("str: lines_bytes is join_lines' length, without joining");
    const char *shapes[] = {"", "a\n", "a\nbb", "a\nbb\n", "\n\n", NULL};
    for (int32_t i = 0; shapes[i]; i++) {
        Lines l = split_lines(a, shapes[i], strlen(shapes[i]));
        size_t joined;
        join_lines(a, l, &joined);
        ASSERT_EQ_I((int32_t)lines_bytes(l), (int32_t)joined);
    }

    t_begin("str: lines_copy keeps the lines and owns their text");
    Arena *b = arena_new(0);
    char src[] = "one\ntwo";
    Lines orig = split_lines(a, src, strlen(src));
    Lines copy = lines_copy(b, orig);
    src[0] = 'X'; /* the copy does not see its source change */
    ASSERT_EQ_I(copy.count, 2);
    ASSERT_TRUE(!copy.eof_nl);
    size_t cl;
    ASSERT_EQ_S(join_lines(b, copy, &cl), "one\ntwo");
    arena_free(b);

    arena_free(a);
}
