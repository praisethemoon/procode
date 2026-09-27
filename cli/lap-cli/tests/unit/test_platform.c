#include <stdio.h>

#include "platform.h"
#include "test.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#define T_PLATDIR ".platform_unit_test"

void test_platform(void) {
    plat_mkdirs(T_PLATDIR "/sub");
    plat_mkdirs(T_PLATDIR "/other");

    t_begin("plat_same_file: one directory, spelled the same or through ..");
    ASSERT_TRUE(plat_same_file(T_PLATDIR "/sub", T_PLATDIR "/sub"));
    ASSERT_TRUE(plat_same_file(T_PLATDIR "/sub", T_PLATDIR "/other/../sub"));

    t_begin("plat_same_file: two directories, or one missing, are not one");
    ASSERT_TRUE(!plat_same_file(T_PLATDIR "/sub", T_PLATDIR "/other"));
    ASSERT_TRUE(!plat_same_file(T_PLATDIR "/sub", T_PLATDIR "/nothing"));
    ASSERT_TRUE(!plat_same_file(T_PLATDIR "/nothing", T_PLATDIR "/nothing"));

    t_begin("plat_stat: a regular file's size; a directory or a missing path "
            "is not one");
    plat_write_file_atomic(T_PLATDIR "/sub/f.txt", "hello\n", 6);
    PlatStat ps;
    ASSERT_TRUE(plat_stat(T_PLATDIR "/sub/f.txt", &ps));
    ASSERT_EQ_I((int32_t)ps.size, 6);
    ASSERT_TRUE(ps.mtime_sec > 0);
    ASSERT_TRUE(!plat_stat(T_PLATDIR "/sub", &ps));
    ASSERT_TRUE(!plat_stat(T_PLATDIR "/nothing", &ps));
    remove(T_PLATDIR "/sub/f.txt");

#ifndef _WIN32
    t_begin("plat_same_file: a symlink reaches the directory it names");
    remove(T_PLATDIR "/link");
    ASSERT_TRUE(symlink("sub", T_PLATDIR "/link") == 0);
    ASSERT_TRUE(plat_same_file(T_PLATDIR "/link", T_PLATDIR "/sub"));

    t_begin("plat_mkdir: a symlink to a directory is a directory that is "
            "there; plat_mkdirs goes through it");
    ASSERT_TRUE(plat_mkdir(T_PLATDIR "/link"));
    ASSERT_TRUE(plat_mkdirs(T_PLATDIR "/link/deeper"));
    ASSERT_TRUE(plat_is_dir(T_PLATDIR "/sub/deeper"));

    t_begin("plat_realpath: a path through a symlink resolves to the "
            "directory's own path; a missing one is false");
    char r1[LAP_PATH_MAX], r2[LAP_PATH_MAX];
    ASSERT_TRUE(plat_realpath(T_PLATDIR "/link", r1, sizeof r1));
    ASSERT_TRUE(plat_realpath(T_PLATDIR "/other/../sub", r2, sizeof r2));
    ASSERT_EQ_S(r1, r2);
    ASSERT_TRUE(r1[0] == '/');
    ASSERT_TRUE(!plat_realpath(T_PLATDIR "/nothing", r1, sizeof r1));
    remove(T_PLATDIR "/sub/deeper");
    remove(T_PLATDIR "/link");
#endif

    t_begin("plat_same_file: on a case-insensitive disk, a case variant is "
            "the same directory");
    if (plat_is_dir(T_PLATDIR "/SUB")) /* only where the disk folds case */
        ASSERT_TRUE(plat_same_file(T_PLATDIR "/SUB", T_PLATDIR "/sub"));
    else
        ASSERT_TRUE(!plat_same_file(T_PLATDIR "/SUB", T_PLATDIR "/sub"));
}
