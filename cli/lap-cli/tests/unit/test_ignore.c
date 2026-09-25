#include "ignore.h"
#include "test.h"

void test_ignore(void) {
    t_begin("ignore: basename patterns match at any depth");
    ASSERT_TRUE(ignore_match_pattern("*.o", "main.o", false));
    ASSERT_TRUE(ignore_match_pattern("*.o", "src/deep/main.o", false));
    ASSERT_TRUE(!ignore_match_pattern("*.o", "main.c", false));
    ASSERT_TRUE(ignore_match_pattern(".DS_Store", "a/b/.DS_Store", false));
    ASSERT_TRUE(ignore_match_pattern("?.txt", "a.txt", false));
    ASSERT_TRUE(!ignore_match_pattern("?.txt", "ab.txt", false));

    t_begin("ignore: dir-only patterns");
    ASSERT_TRUE(ignore_match_pattern("build/", "build", true));
    ASSERT_TRUE(!ignore_match_pattern("build/", "build", false));
    ASSERT_TRUE(ignore_match_pattern(".lap/", ".lap", true));

    t_begin("ignore: anchored patterns (containing '/')");
    ASSERT_TRUE(ignore_match_pattern("/top.txt", "top.txt", false));
    ASSERT_TRUE(!ignore_match_pattern("/top.txt", "sub/top.txt", false));
    ASSERT_TRUE(ignore_match_pattern("src/*.c", "src/a.c", false));
    ASSERT_TRUE(!ignore_match_pattern("src/*.c", "src/deep/a.c", false));
    ASSERT_TRUE(!ignore_match_pattern("src/*.c", "other/a.c", false));

    t_begin("ignore: ** matches whole segments");
    ASSERT_TRUE(ignore_match_pattern("**/gen", "gen", true));
    ASSERT_TRUE(ignore_match_pattern("**/gen", "a/b/gen", true));
    ASSERT_TRUE(ignore_match_pattern("docs/**", "docs/a", false));
    ASSERT_TRUE(ignore_match_pattern("docs/**", "docs/a/b/c", false));
    ASSERT_TRUE(!ignore_match_pattern("docs/**", "src/a", false));
    ASSERT_TRUE(ignore_match_pattern("a/**/z.txt", "a/z.txt", false));
    ASSERT_TRUE(ignore_match_pattern("a/**/z.txt", "a/b/c/z.txt", false));

    t_begin("ignore: loaded set includes defaults and nested-dir files");
    Arena *a = arena_new(0);
    /* no .lapignore on disk in this scratch dir: defaults only */
    Ignore *ig = ignore_load(a, "/nonexistent-lap-test-dir");
    ASSERT_TRUE(ignore_match(ig, ".lap", true));
    ASSERT_TRUE(ignore_match(ig, ".git", true));
    ASSERT_TRUE(ignore_match(ig, ".lap/log.jsonl", false));
    ASSERT_TRUE(ignore_match(ig, "sub/.git/config", false));
    ASSERT_TRUE(ignore_match(ig, "a/b/.DS_Store", false));
    ASSERT_TRUE(!ignore_match(ig, "src/main.c", false));
    arena_free(a);
}
