#include <stdio.h>

#include "branches.h"
#include "platform.h"
#include "test.h"

#define T_REGDIR ".branches_unit_test"

void test_branches(void) {
    Arena *a = arena_new(0);
    char path[256];
    snprintf(path, sizeof path, "%s/%s", T_REGDIR, LAP_BRANCHES_NAME);
    remove(path);
    plat_mkdirs(T_REGDIR);
    Branches b;

    t_begin("branches: a missing registry is no branches");
    branches_load(a, T_REGDIR, &b);
    ASSERT_EQ_I(b.n, 0);
    ASSERT_TRUE(branches_find(&b, "x") == NULL);

    t_begin("branches: entries survive a save and a load");
    branches_add(a, &b, (BranchEntry){"0123456789ab", "feat \"one\"",
                                      "/w/feat", "abc", "2026-09-27T00:00:00Z"});
    branches_add(a, &b, (BranchEntry){"ba9876543210", "other", "/w/other",
                                      "def", "2026-09-27T00:00:01Z"});
    ASSERT_TRUE(branches_save(a, T_REGDIR, &b));
    Branches back;
    branches_load(a, T_REGDIR, &back);
    ASSERT_EQ_I(back.n, 2);
    ASSERT_EQ_S(back.v[0].name, "feat \"one\"");
    ASSERT_EQ_S(back.v[1].path, "/w/other");

    t_begin("branches: an entry is found by its id or its name");
    ASSERT_TRUE(branches_find(&back, "ba9876543210") == &back.v[1]);
    ASSERT_TRUE(branches_find(&back, "feat \"one\"") == &back.v[0]);
    ASSERT_TRUE(branches_find(&back, "feat") == NULL);

    t_begin("branches: a malformed registry reads as none, never an error");
    plat_write_file_atomic(path, "{not json", 9);
    branches_load(a, T_REGDIR, &back);
    ASSERT_EQ_I(back.n, 0);
    plat_write_file_atomic(path, "{\"id\":1}", 8);
    branches_load(a, T_REGDIR, &back);
    ASSERT_EQ_I(back.n, 0);

    t_begin("branches: an entry missing a field is skipped");
    const char *partial = "[{\"id\":\"0123456789ab\",\"name\":\"x\"},"
                          "{\"id\":\"ba9876543210\",\"name\":\"y\",\"path\":"
                          "\"/p\",\"base\":\"b\",\"started\":\"t\"}]";
    plat_write_file_atomic(path, partial, strlen(partial));
    branches_load(a, T_REGDIR, &back);
    ASSERT_EQ_I(back.n, 1);
    ASSERT_EQ_S(back.v[0].name, "y");

    remove(path);
    arena_free(a);
}
