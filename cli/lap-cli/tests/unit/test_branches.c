#include <stdio.h>

#include "branches.h"
#include "cmd.h"
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

    t_begin("branches_status: a gone, unmerged branch is missing, with its "
            "commits counted from the chunks here");
    remove(path);
    const char *id = "0123456789ab";
    Rec init, br, c1, c2;
    memset(&init, 0, sizeof init);
    init.type = REC_INIT;
    init.version = 1;
    init.ts = "t0";
    init.prev = LAP_HASH_ZERO;
    size_t n0, n1, n2, n3;
    char *l0 = rec_encode(a, &init, &n0);
    memset(&br, 0, sizeof br);
    br.type = REC_BRANCH;
    br.id = id;
    br.name = "feat";
    br.parent = "main";
    br.base = init.hash;
    br.base_chunk = 1;
    br.ts = "t1";
    br.prev = init.hash;
    char *l1 = rec_encode(a, &br, &n1);
    Str none = {"x", 1};
    memset(&c1, 0, sizeof c1);
    c1.type = REC_COMMIT;
    c1.id = "L1";
    c1.file = "f.txt";
    c1.op = "create";
    c1.new_text = &none;
    c1.new_n = c1.new_lines = 1;
    c1.old_start = c1.new_start = 1;
    c1.intent = "make the file";
    c1.behavior = "creates it";
    c1.ts = "t2";
    c1.prev = br.hash;
    char *l2 = rec_encode(a, &c1, &n2);
    c2 = c1;
    c2.id = "L2";
    c2.op = "edit";
    c2.old_text = &none;
    c2.old_n = c2.old_lines = 1;
    c2.prev = c1.hash;
    char *l3 = rec_encode(a, &c2, &n3);
    char p1[256], p2[256];
    snprintf(p1, sizeof p1, "%s/log", T_REGDIR);
    plat_mkdirs(p1);
    snprintf(p1, sizeof p1, "%s/log/main.000001.jsonl", T_REGDIR);
    plat_write_file_atomic(p1, arena_printf(a, "%s\n", l0), n0 + 1);
    snprintf(p2, sizeof p2, "%s/log/%s.000001.jsonl", T_REGDIR, id);
    char *bl = arena_printf(a, "%s\n%s\n%s\n", l1, l2, l3);
    plat_write_file_atomic(p2, bl, strlen(bl));
    BranchEntry ent = {id, "feat", "/nonexistent-lap-branch-folder",
                       init.hash, "t1"};
    RecLog plog;
    memset(&plog, 0, sizeof plog);
    BranchStatus st;
    branches_status(a, T_REGDIR, &plog, &ent, &st);
    ASSERT_TRUE(!st.present);
    ASSERT_TRUE(st.readable);
    ASSERT_EQ_S(st.state, "missing");
    ASSERT_EQ_I(st.since_base, 2);
    ASSERT_EQ_I(st.since_merge, 2);
    ASSERT_EQ_S(st.head, c2.hash);

    t_begin("branches_status: merged up to its head is merged; one commit "
            "short, or with a stopped file, is not");
    Rec mr;
    memset(&mr, 0, sizeof mr);
    mr.type = REC_MERGE;
    mr.branch = id;
    mr.head = c2.hash;
    Rec logv[1] = {mr};
    plog.v = logv;
    plog.count = 1;
    branches_status(a, T_REGDIR, &plog, &ent, &st);
    ASSERT_EQ_S(st.state, "merged");
    ASSERT_EQ_I(st.since_merge, 0);
    logv[0].head = c1.hash;
    branches_status(a, T_REGDIR, &plog, &ent, &st);
    ASSERT_EQ_S(st.state, "missing");
    ASSERT_EQ_I(st.since_merge, 1);
    logv[0].head = c2.hash;
    const char *sf[] = {"f.txt"};
    const char *sa[] = {"h"};
    logv[0].stopped_file = sf;
    logv[0].stopped_at = sa;
    logv[0].stopped_n = 1;
    branches_status(a, T_REGDIR, &plog, &ent, &st);
    ASSERT_EQ_S(st.state, "missing");
    ASSERT_EQ_I(st.nstopped, 1);
    ASSERT_EQ_S(st.stopped[0], "f.txt");

    t_begin("branches_status: a branch with no chunk anywhere is unreadable "
            "and missing");
    BranchEntry ghost = {"ba9876543210", "ghost", "/nonexistent-lap-ghost",
                         "b", "t"};
    branches_status(a, T_REGDIR, &plog, &ghost, &st);
    ASSERT_TRUE(!st.readable);
    ASSERT_EQ_I(st.since_base, -1);
    ASSERT_EQ_S(st.state, "missing");
    remove(p1);
    remove(p2);

    t_begin("branch_check: a folder with no branches needs no name, and "
            "takes only main");
    remove(path);
    Repo r;
    memset(&r, 0, sizeof r);
    snprintf(r.root, sizeof r.root, "/w/plain");
    snprintf(r.lapdir, sizeof r.lapdir, "%s", T_REGDIR);
    ASSERT_TRUE(branch_check(a, &r, NULL, true));
    ASSERT_TRUE(branch_check(a, &r, "main", true));
    ASSERT_TRUE(!branch_check(a, &r, "feat", true));

    t_begin("branch_check: a parent with branches needs main said");
    Branches one;
    memset(&one, 0, sizeof one);
    branches_add(a, &one, (BranchEntry){"0123456789ab", "feat", "/w/feat",
                                        "abc", "t"});
    ASSERT_TRUE(branches_save(a, T_REGDIR, &one));
    ASSERT_TRUE(!branch_check(a, &r, NULL, true));
    ASSERT_TRUE(branch_check(a, &r, "main", true));
    ASSERT_TRUE(!branch_check(a, &r, "feat", true));

    t_begin("branch_check: a branch folder takes its name or its id");
    snprintf(r.hist.parent, sizeof r.hist.parent, "main");
    snprintf(r.hist.name, sizeof r.hist.name, "feat");
    snprintf(r.hist.lineage, sizeof r.hist.lineage, "0123456789ab");
    remove(path); /* a branch needs it whether or not it has branches */
    ASSERT_TRUE(!branch_check(a, &r, NULL, true));
    ASSERT_TRUE(branch_check(a, &r, "feat", true));
    ASSERT_TRUE(branch_check(a, &r, "0123456789ab", true));
    ASSERT_TRUE(!branch_check(a, &r, "main", true));

    remove(path);
    arena_free(a);
}
