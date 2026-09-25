#include "test.h"

#include "tty.h"

/* tty_init reads a real command line, so the tests hand it one. The mode
 * is process-wide state; every test sets it explicitly rather than
 * inheriting whatever the previous one left behind. */
static bool init(const char *a1, const char *a2, const char *a3) {
    char *argv[5];
    int32_t argc = 1;
    argv[0] = (char *)"lap";
    if (a1)
        argv[argc++] = (char *)a1;
    if (a2)
        argv[argc++] = (char *)a2;
    if (a3)
        argv[argc++] = (char *)a3;
    argv[argc] = NULL;
    return tty_init(argc, argv);
}

static const char *field(Arena *a, Style s, const char *text, int32_t width) {
    StrBuf sb;
    sb_init(&sb, a);
    sb_field(&sb, s, text, width);
    return sb_finish(&sb);
}

void test_tty(void) {
    Arena *a = arena_new(4096);

    t_begin("tty: never yields empty style strings");
    ASSERT_TRUE(init("--color=never", NULL, NULL));
    ASSERT_EQ_S(sgr(S_ID), "");
    ASSERT_EQ_S(sgr_off(), "");
    ASSERT_EQ_S(sgr_f(stderr, S_ERROR), "");
    ASSERT_EQ_S(sgr_off_f(stderr), "");

    t_begin("tty: always yields SGR sequences");
    ASSERT_TRUE(init("--color=always", NULL, NULL));
    ASSERT_EQ_S(sgr(S_ID), "\033[33m");
    ASSERT_EQ_S(sgr(S_ADDED), "\033[32m");
    ASSERT_EQ_S(sgr(S_REMOVED), "\033[31m");
    ASSERT_EQ_S(sgr_off(), "\033[0m");
    ASSERT_EQ_S(sgr_f(stderr, S_ERROR), "\033[1;31m");

    t_begin("tty: the last flag on the line wins");
    ASSERT_TRUE(init("--color=always", "--no-color", NULL));
    ASSERT_EQ_S(sgr(S_ID), "");
    ASSERT_TRUE(init("--no-color", "--color=always", NULL));
    ASSERT_EQ_S(sgr(S_ID), "\033[33m");

    t_begin("tty: flags end at --, so a path is never a flag");
    /* were the trailing flag read, last-wins would have turned colour on */
    ASSERT_TRUE(init("--color=never", "--", "--color=always"));
    ASSERT_EQ_S(sgr(S_ID), "");

    t_begin("tty: an unknown mode is rejected but still resolves a mode");
    ASSERT_TRUE(!init("--color=purple", NULL, NULL));
    ASSERT_TRUE(!init("--color=", NULL, NULL));
    /* the complaint is an error like any other, so it may be coloured */
    ASSERT_TRUE(!init("--color=always", "--color=purple", NULL));
    ASSERT_EQ_S(sgr_f(stderr, S_ERROR), "\033[1;31m");
    ASSERT_TRUE(init("--color=auto", NULL, NULL));

    t_begin("tty: a field pads to its visible width, not its byte length");
    ASSERT_TRUE(init("--color=never", NULL, NULL));
    ASSERT_EQ_S(field(a, S_ID, "L1", 8), "L1      ");
    ASSERT_TRUE(init("--color=always", NULL, NULL));
    ASSERT_EQ_S(field(a, S_ID, "L1", 8), "\033[33mL1\033[0m      ");

    t_begin("tty: an over-long field is not truncated and gains no padding");
    ASSERT_EQ_S(field(a, S_ID, "cfd4ac/L11", 8), "\033[33mcfd4ac/L11\033[0m");

    t_begin("tty: an empty field is spaces alone, never a bare escape pair");
    ASSERT_EQ_S(field(a, S_SESSION, "", 4), "    ");
    ASSERT_EQ_S(field(a, S_SESSION, "", 0), "");

    /* leave the process plain for whatever runs next */
    ASSERT_TRUE(init("--color=never", NULL, NULL));
    arena_free(a);
}
