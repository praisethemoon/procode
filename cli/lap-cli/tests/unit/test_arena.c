#include "arena.h"
#include "test.h"

void test_arena(void) {
    t_begin("arena: basic allocation");
    Arena *a = arena_new(0);
    void *p1 = arena_alloc(a, 10);
    void *p2 = arena_alloc(a, 10);
    ASSERT_TRUE(p1 != NULL);
    ASSERT_TRUE(p2 != NULL);
    ASSERT_TRUE(p1 != p2);
    ASSERT_TRUE(((uintptr_t)p1 % 16) == 0);
    ASSERT_TRUE(((uintptr_t)p2 % 16) == 0);

    t_begin("arena: zeroed allocation");
    uint8_t *z = (uint8_t *)arena_alloc0(a, 64);
    int32_t all_zero = 1;
    for (int32_t i = 0; i < 64; i++)
        if (z[i] != 0)
            all_zero = 0;
    ASSERT_TRUE(all_zero);

    t_begin("arena: growth past the initial chunk");
    size_t before = arena_used(a);
    for (int32_t i = 0; i < 1000; i++) {
        char *p = (char *)arena_alloc(a, 100);
        p[0] = (char)i; /* touch it */
        p[99] = (char)i;
    }
    ASSERT_TRUE(arena_used(a) >= before + 100 * 1000);

    t_begin("arena: oversized single allocation");
    char *big = (char *)arena_alloc(a, 1 << 20);
    big[0] = 'x';
    big[(1 << 20) - 1] = 'y';
    ASSERT_TRUE(big[0] == 'x' && big[(1 << 20) - 1] == 'y');

    t_begin("arena: strdup / strndup / printf");
    char *s = arena_strdup(a, "hello");
    ASSERT_EQ_S(s, "hello");
    char *sn = arena_strndup(a, "hello world", 5);
    ASSERT_EQ_S(sn, "hello");
    char *f = arena_printf(a, "%s-%d", "id", 42);
    ASSERT_EQ_S(f, "id-42");

    t_begin("arena: realloc grows and preserves");
    char *r = (char *)arena_realloc(a, NULL, 0, 4);
    memcpy(r, "abc", 4);
    r = (char *)arena_realloc(a, r, 4, 400);
    ASSERT_EQ_S(r, "abc");

    arena_free(a);
}
