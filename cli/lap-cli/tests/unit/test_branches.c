#include <stdio.h>
#include <stdlib.h>

#include "branches.h"
#include "cmd.h"
#include "platform.h"
#include "test.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#define T_REGDIR ".branches_unit_test"

/* A fixture folder under TMPDIR (else /tmp), not left in the source tree;
 * on Windows, in the current folder as before. */
static void unit_tmp(char *out, size_t sz, const char *name) {
#ifdef _WIN32
    snprintf(out, sz, ".%s", name);
#else
    const char *t = getenv("TMPDIR");
    snprintf(out, sz, "%s/lap_unit_%ld_%s", t && *t ? t : "/tmp",
             (long)getpid(), name);
#endif
}

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
                                      "/w/feat", "abc", "2026-09-27T00:00:00Z",
                                      NULL});
    branches_add(a, &b, (BranchEntry){"ba9876543210", "other", "/w/other",
                                      "def", "2026-09-27T00:00:01Z", NULL});
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
                       init.hash, "t1", NULL};
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
                         "b", "t", NULL};
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

#ifndef _WIN32
    t_begin("branch_check: a folder with no branches ignores LAP_BRANCH, "
            "but not --branch");
    setenv("LAP_BRANCH", "feat", 1);
    char *no_flag[] = {"commit", "f.txt"};
    char *flagged[] = {"commit", "--branch", "feat"};
    static const char *const bflags[] = {"--branch", NULL};
    ASSERT_TRUE(branch_check(a, &r, branch_given(2, no_flag, bflags), true));
    ASSERT_TRUE(!branch_check(a, &r, branch_given(3, flagged, bflags), true));
    unsetenv("LAP_BRANCH");
#endif

    t_begin("branch_check: a parent with branches needs main said");
    Branches one;
    memset(&one, 0, sizeof one);
    branches_add(a, &one, (BranchEntry){"0123456789ab", "feat", "/w/feat",
                                        "abc", "t", NULL});
    ASSERT_TRUE(branches_save(a, T_REGDIR, &one));
    ASSERT_TRUE(!branch_check(a, &r, NULL, true));
    ASSERT_TRUE(branch_check(a, &r, "main", true));
    ASSERT_TRUE(!branch_check(a, &r, "feat", true));
#ifndef _WIN32
    setenv("LAP_BRANCH", "feat", 1); /* with branches, it is checked */
    ASSERT_TRUE(!branch_check(a, &r, branch_given(2, no_flag, bflags), true));
    unsetenv("LAP_BRANCH");
#endif

    t_begin("branch_check: a branch folder takes its name or its id");
    snprintf(r.hist.parent, sizeof r.hist.parent, "main");
    snprintf(r.hist.name, sizeof r.hist.name, "feat");
    snprintf(r.hist.lineage, sizeof r.hist.lineage, "0123456789ab");
    remove(path); /* a branch needs it whether or not it has branches */
    ASSERT_TRUE(!branch_check(a, &r, NULL, true));
    ASSERT_TRUE(branch_check(a, &r, "feat", true));
    ASSERT_TRUE(branch_check(a, &r, "0123456789ab", true));
    ASSERT_TRUE(!branch_check(a, &r, "main", true));

    t_begin("session_ref: a branch's session is named with its branch, "
            "main's and unlabeled ones as they are");
    ASSERT_EQ_S(session_ref(a, "feat", "S4"), "feat/S4");
    ASSERT_EQ_S(session_ref(a, "main", "S4"), "S4");
    ASSERT_EQ_S(session_ref(a, NULL, "S4"), "S4");
    ASSERT_TRUE(session_ref(a, "feat", NULL) == NULL);

    t_begin("branches_load_deep: a branch's registry joins, its entries "
            "marked with the branch that listed them");
    remove(path);
    char b1path[LAP_PATH_MAX], b1lap[LAP_PATH_MAX];
    unit_tmp(b1path, sizeof b1path, "deep_unit_b1");
    snprintf(b1lap, sizeof b1lap, "%s/.lap", b1path);
    plat_mkdirs(b1lap);
    hist_write_lineage(b1lap, "0123456789ab");
    Branches top, sub, deep;
    memset(&top, 0, sizeof top);
    memset(&sub, 0, sizeof sub);
    branches_add(a, &top, (BranchEntry){"0123456789ab", "b1", b1path, "x",
                                        "t", NULL});
    branches_add(a, &sub, (BranchEntry){"ba9876543210", "b2", "/w/b2", "y",
                                        "t", NULL});
    branches_add(a, &sub, (BranchEntry){"0123456789ab", "b1", b1path, "x",
                                        "t", NULL}); /* already listed */
    ASSERT_TRUE(branches_save(a, T_REGDIR, &top));
    ASSERT_TRUE(branches_save(a, b1lap, &sub));
    branches_load_deep(a, T_REGDIR, &deep);
    ASSERT_EQ_I(deep.n, 2);
    ASSERT_TRUE(deep.v[0].via == NULL);
    ASSERT_EQ_S(deep.v[1].name, "b2");
    ASSERT_EQ_S(deep.v[1].via, "0123456789ab");
    branches_load(a, T_REGDIR, &deep);
    ASSERT_EQ_I(deep.n, 1);

    t_begin("branches_load_deep: a folder that is no longer that branch is "
            "not followed");
    hist_write_lineage(b1lap, "ba9876543210");
    branches_load_deep(a, T_REGDIR, &deep);
    ASSERT_EQ_I(deep.n, 1);
    char subpath[LAP_PATH_MAX];
    snprintf(subpath, sizeof subpath, "%s/%s", b1lap, LAP_BRANCHES_NAME);
    remove(subpath);
    snprintf(subpath, sizeof subpath, "%s/lineage", b1lap);
    remove(subpath);
    remove(b1lap);
    remove(b1path);
    remove(path);

    t_begin("repo_write_gitignore: git keeps only log/ and the file itself; "
            "a .gitignore already there is kept");
    plat_mkdirs(".gitignore_unit_test");
    char gip[128];
    snprintf(gip, sizeof gip, ".gitignore_unit_test/.gitignore");
    remove(gip);
    ASSERT_TRUE(repo_write_gitignore(".gitignore_unit_test"));
    char *gi;
    size_t gil;
    ASSERT_TRUE(plat_read_file(a, gip, &gi, &gil));
    ASSERT_TRUE(strstr(gi, "\n/*\n!/.gitignore\n!/log/\n") != NULL);
    plat_write_file_atomic(gip, "mine\n", 5);
    ASSERT_TRUE(repo_write_gitignore(".gitignore_unit_test"));
    ASSERT_TRUE(plat_read_file(a, gip, &gi, &gil));
    ASSERT_EQ_S(gi, "mine\n");
    remove(gip);

    t_begin("repo_convert_legacy: a conversion writes lap's .gitignore when "
            "there is none, keeps one that is there, and none is written "
            "without a conversion");
    bool conv = false;
    char cerr[256];
    const char *old_log = ".gitignore_unit_test/" LAP_LOG_NAME;
    const char *chunk1 = ".gitignore_unit_test/" LAP_LOG_DIR
                         "/main.000001.jsonl";
    ASSERT_TRUE(repo_convert_legacy(a, ".gitignore_unit_test", 1000, &conv,
                                    cerr, sizeof cerr));
    ASSERT_TRUE(!conv);
    ASSERT_TRUE(!plat_is_file(gip));
    plat_write_file_atomic(old_log, "aaaa\n", 5);
    ASSERT_TRUE(repo_convert_legacy(a, ".gitignore_unit_test", 1000, &conv,
                                    cerr, sizeof cerr));
    ASSERT_TRUE(conv);
    ASSERT_TRUE(plat_read_file(a, gip, &gi, &gil));
    ASSERT_TRUE(strstr(gi, "\n/*\n!/.gitignore\n!/log/\n") != NULL);
    remove(chunk1);
    plat_write_file_atomic(gip, "mine\n", 5);
    plat_write_file_atomic(old_log, "aaaa\n", 5);
    ASSERT_TRUE(repo_convert_legacy(a, ".gitignore_unit_test", 1000, &conv,
                                    cerr, sizeof cerr));
    ASSERT_TRUE(conv);
    ASSERT_TRUE(plat_read_file(a, gip, &gi, &gil));
    ASSERT_EQ_S(gi, "mine\n");
    remove(chunk1);
    remove(".gitignore_unit_test/" LAP_LOG_DIR);
    remove(gip);

    t_begin("own_part_start: a branch's own records start after its own "
            "branch record, not its parent branch's; main's at 0");
    Rec op[5];
    memset(op, 0, sizeof op);
    op[1].type = REC_BRANCH;
    op[1].id = "0123456789ab"; /* the branch it started from */
    op[3].type = REC_BRANCH;
    op[3].id = "ba9876543210"; /* its own */
    RecLog olog;
    memset(&olog, 0, sizeof olog);
    olog.v = op;
    olog.count = 5;
    ASSERT_EQ_I(own_part_start(&olog, "ba9876543210"), 4);
    ASSERT_EQ_I(own_part_start(&olog, "0123456789ab"), 2);
    ASSERT_EQ_I(own_part_start(&olog, NULL), 0);
    ASSERT_EQ_I(own_part_start(&olog, "ffffffffffff"), 0);

    t_begin("merge_redo_point: no earlier run, or one followed by other "
            "work, places against the latest record; one at the end is "
            "redone from just before it");
    Rec rl[4];
    memset(rl, 0, sizeof rl);
    rl[2].from = "h1"; /* an interrupted run's copies of h1 and h2 */
    rl[3].from = "h2";
    RecLog rlog;
    memset(&rlog, 0, sizeof rlog);
    rlog.v = rl;
    rlog.count = 4;
    StrSet nw;
    strset_init(&nw, a);
    ASSERT_EQ_I(merge_redo_point(&rlog, &nw, NULL), 3);
    strset_add(&nw, "h1");
    strset_add(&nw, "h2");
    ASSERT_EQ_I(merge_redo_point(&rlog, &nw, NULL), 1);
    rl[3].from = NULL; /* work recorded after the run's first copy */
    ASSERT_EQ_I(merge_redo_point(&rlog, &nw, NULL), 3);
    rl[3].from = "elsewhere";
    ASSERT_EQ_I(merge_redo_point(&rlog, &nw, NULL), 3);

    t_begin("merge_redo_point: the run's own merge records, written between "
            "its branches, are part of the run");
    rl[3].from = NULL;
    rl[3].type = REC_MERGE;
    rl[3].branch = "0123456789ab";
    StrSet runb;
    strset_init(&runb, a);
    ASSERT_EQ_I(merge_redo_point(&rlog, &nw, &runb), 3);
    strset_add(&runb, "0123456789ab");
    ASSERT_EQ_I(merge_redo_point(&rlog, &nw, &runb), 1);
    rl[3].branch = "ffffffffffff"; /* another branch's merge: other work */
    ASSERT_EQ_I(merge_redo_point(&rlog, &nw, &runb), 3);

    t_begin("merge_cut_start: trailing merge records of the chain's outer "
            "branches at the heads it writes are the cut run's own");
    Rec cr[5];
    memset(cr, 0, sizeof cr);
    cr[0].type = REC_COMMIT; /* the run's copies, then its outer records */
    cr[1].type = REC_COMMIT;
    cr[2].type = REC_MERGE;
    cr[2].branch = "0123456789ab";
    cr[2].head = "h1";
    cr[3].type = REC_MERGE;
    cr[3].branch = "ba9876543210";
    cr[3].head = "h2";
    RecLog clog;
    memset(&clog, 0, sizeof clog);
    clog.v = cr;
    clog.count = 4;
    const char *oids[] = {"0123456789ab", "ba9876543210"};
    const char *oheads[] = {"h1", "h2"};
    ASSERT_EQ_I(merge_cut_start(&clog, oids, oheads, 2), 2);
    ASSERT_EQ_I(merge_cut_start(&clog, oids, oheads, 1), 4);
    ASSERT_EQ_I(merge_cut_start(&clog, oids, oheads, 0), 4);

    t_begin("merge_cut_start: a merge record at another head, of another "
            "branch, or followed by other work is not the run's");
    oheads[1] = "h9"; /* a merge of that branch alone, further on */
    ASSERT_EQ_I(merge_cut_start(&clog, oids, oheads, 2), 4);
    oheads[1] = "h2";
    cr[3].branch = "ffffffffffff";
    ASSERT_EQ_I(merge_cut_start(&clog, oids, oheads, 2), 4);
    cr[3].branch = "ba9876543210";
    cr[4].type = REC_SESSION_START; /* work recorded after the cut */
    clog.count = 5;
    ASSERT_EQ_I(merge_cut_start(&clog, oids, oheads, 2), 5);
    clog.count = 0;
    ASSERT_EQ_I(merge_cut_start(&clog, oids, oheads, 2), 0);

    t_begin("ref_find_record: a hash or S<n> names any record, which "
            "ref_find, for commits only, does not");
    Rec rr[3];
    memset(rr, 0, sizeof rr);
    rr[0].type = REC_SESSION_START;
    rr[0].id = "S1";
    snprintf(rr[0].hash, sizeof rr[0].hash, "%s", "aaaa1111aaaa1111");
    rr[1].type = REC_COMMIT;
    rr[1].id = "L1";
    snprintf(rr[1].hash, sizeof rr[1].hash, "%s", "bbbb2222bbbb2222");
    rr[2].type = REC_AMEND;
    snprintf(rr[2].hash, sizeof rr[2].hash, "%s", "cccc3333cccc3333");
    RecLog rrlog;
    memset(&rrlog, 0, sizeof rrlog);
    rrlog.v = rr;
    rrlog.count = 3;
    const char *rcode;
    char rerr[256];
    ASSERT_EQ_I(ref_find_record(&rrlog, "aaaa1111", &rcode, rerr, sizeof rerr), 0);
    ASSERT_EQ_I(ref_find_record(&rrlog, "#CCCC3333", &rcode, rerr, sizeof rerr), 2);
    ASSERT_EQ_I(ref_find_record(&rrlog, "S1", &rcode, rerr, sizeof rerr), 0);
    ASSERT_EQ_I(ref_find_record(&rrlog, "L1", &rcode, rerr, sizeof rerr), 1);
    ASSERT_EQ_I(ref_find_record(&rrlog, "S9", &rcode, rerr, sizeof rerr), -1);
    ASSERT_EQ_S(rcode, "unknown_ref");
    ASSERT_EQ_I(ref_find(&rrlog, "aaaa1111", &rcode, rerr, sizeof rerr), -1);
    ASSERT_EQ_I(ref_find(&rrlog, "bbbb2222", &rcode, rerr, sizeof rerr), 1);

    t_begin("merge_nothing_yet: a merge finding nothing records itself only "
            "for a branch never merged, marking each unmerged in its chain");
    const char *mh[2] = {NULL, "hash-of-b1"};
    bool mu[2];
    ASSERT_TRUE(merge_nothing_yet(5, 5, mh, 2, mu));
    ASSERT_TRUE(mu[0] && !mu[1]);
    ASSERT_TRUE(!merge_nothing_yet(4, 5, mh, 2, mu)); /* something new */
    mh[0] = "hash-of-b2"; /* merged before: nothing new since */
    ASSERT_TRUE(!merge_nothing_yet(5, 5, mh, 2, mu));
    ASSERT_TRUE(!mu[0] && !mu[1]);

    t_begin("merge_unknown_records: a newer lap's records count in the part "
            "a merge would adopt, not in this folder's own");
    Rec ul[4];
    memset(ul, 0, sizeof ul);
    for (int32_t i = 0; i < 4; i++)
        ul[i].type = REC_COMMIT;
    RecLog ulog;
    memset(&ulog, 0, sizeof ulog);
    ulog.v = ul;
    ulog.count = 4;
    int32_t ulof[4] = {-1, -1, 0, 0};
    const char *utype = "x";
    ASSERT_EQ_I(merge_unknown_records(&ulog, ulof, &utype), 0);
    ASSERT_TRUE(utype == NULL);
    ul[1].type = REC_UNKNOWN; /* this folder's own: its writers refuse it */
    ul[1].name = "older";
    ASSERT_EQ_I(merge_unknown_records(&ulog, ulof, &utype), 0);
    ul[2].type = REC_UNKNOWN;
    ul[2].name = "annotate";
    ul[3].type = REC_UNKNOWN;
    ul[3].name = "other";
    ASSERT_EQ_I(merge_unknown_records(&ulog, ulof, &utype), 2);
    ASSERT_EQ_S(utype, "annotate");

    t_begin("branches_tree_order: each nested branch right after the branch "
            "it started from, depth first, roots in the registry's order");
    Branches tr;
    memset(&tr, 0, sizeof tr);
    branches_add(a, &tr, (BranchEntry){"aaaaaaaaaaaa", "b1", "/w/b1", "h", "t", NULL});
    branches_add(a, &tr, (BranchEntry){"bbbbbbbbbbbb", "x2", "/w/x2", "h", "t", NULL});
    branches_add(a, &tr, (BranchEntry){"cccccccccccc", "n1", "/w/n1", "h", "t", "aaaaaaaaaaaa"});
    branches_add(a, &tr, (BranchEntry){"dddddddddddd", "m1", "/w/m1", "h", "t", "cccccccccccc"});
    branches_add(a, &tr, (BranchEntry){"eeeeeeeeeeee", "lost", "/w/l", "h", "t", "ffffffffffff"});
    int32_t tord[5];
    ASSERT_EQ_I(branches_tree_order(a, &tr, tord), 5);
    ASSERT_EQ_I(tord[0], 0); /* b1 */
    ASSERT_EQ_I(tord[1], 2); /* n1, from b1 */
    ASSERT_EQ_I(tord[2], 3); /* m1, from n1 */
    ASSERT_EQ_I(tord[3], 1); /* x2 */
    ASSERT_EQ_I(tord[4], 4); /* its parent unlisted: a root, in order */

    t_begin("branches_status_nearer: a nested branch counts as merged where "
            "its head was adopted, here or in the branch it started from");
    BranchStatus sv, sh;
    memset(&sv, 0, sizeof sv);
    memset(&sh, 0, sizeof sh);
    sv.state = "active";
    sh.state = "active";
    ASSERT_TRUE(branches_status_nearer(&sv, &sh) == &sv); /* level: via's */
    sh.state = "merged";
    sh.merged = "h";
    ASSERT_TRUE(branches_status_nearer(&sv, &sh) == &sh); /* merged here */
    sv.state = "partly merged";
    sv.merged = "g";
    ASSERT_TRUE(branches_status_nearer(&sv, &sh) == &sh);
    sh.state = "missing"; /* here: merged, but not up to its head */
    ASSERT_TRUE(branches_status_nearer(&sv, &sh) == &sv); /* level again */
    sv.state = "merged";
    sh.state = "active";
    sh.merged = NULL;
    ASSERT_TRUE(branches_status_nearer(&sv, &sh) == &sv); /* merged there */

    t_begin("log_tracked_files: a file is tracked while its last commit is "
            "not a delete, whatever came before; other records are not files");
    Rec tl[6];
    memset(tl, 0, sizeof tl);
    const char *tfile[] = {"a.txt", "b.txt", NULL, "b.txt", "c.txt", "a.txt"};
    const char *tops[] = {"create", "create", NULL, "delete", "create", "edit"};
    for (int32_t i = 0; i < 6; i++) {
        tl[i].type = tfile[i] ? REC_COMMIT : REC_SESSION_START;
        tl[i].file = tfile[i];
        tl[i].op = tops[i];
    }
    RecLog tlog;
    memset(&tlog, 0, sizeof tlog);
    tlog.v = tl;
    tlog.count = 6;
    const char **tpaths;
    ASSERT_EQ_I((int32_t)log_tracked_files(a, &tlog, &tpaths), 2);
    ASSERT_EQ_S(tpaths[0], "a.txt");
    ASSERT_EQ_S(tpaths[1], "c.txt");
    tl[4].op = "delete"; /* c.txt gone too */
    tl[3].op = "edit";   /* b.txt kept after all */
    ASSERT_EQ_I((int32_t)log_tracked_files(a, &tlog, &tpaths), 2);
    ASSERT_EQ_S(tpaths[0], "a.txt");
    ASSERT_EQ_S(tpaths[1], "b.txt");

    t_begin("own_chunks: this folder's copy wins, and the branch folder is "
            "not read unless it may fill in");
    const char *oid = "0123456789ab";
    char here[LAP_PATH_MAX], there[LAP_PATH_MAX], here_lap[LAP_PATH_MAX];
    char here_log[LAP_PATH_MAX], there_log[LAP_PATH_MAX];
    char here_c1[LAP_PATH_MAX], there_c1[LAP_PATH_MAX], there_c2[LAP_PATH_MAX];
    unit_tmp(here, sizeof here, "own_unit_here");
    unit_tmp(there, sizeof there, "own_unit_there");
    snprintf(here_lap, sizeof here_lap, "%s/.lap", here);
    snprintf(here_log, sizeof here_log, "%s/log", here_lap);
    snprintf(there_log, sizeof there_log, "%s/.lap/log", there);
    plat_mkdirs(here_log);
    plat_mkdirs(there_log);
    snprintf(here_c1, sizeof here_c1, "%s/%s.000001.jsonl", here_log, oid);
    snprintf(there_c1, sizeof there_c1, "%s/%s.000001.jsonl", there_log, oid);
    snprintf(there_c2, sizeof there_c2, "%s/%s.000002.jsonl", there_log, oid);
    plat_write_file_atomic(here_c1, "a\n", 2);
    plat_write_file_atomic(there_c1, "a\nb\n", 4);
    plat_write_file_atomic(there_c2, "c\nd", 3);
    OwnChunk *own;
    int32_t nown;
    bool obehind;
    char oerr[256];
    ASSERT_TRUE(own_chunks(a, here_lap, there, oid,
                           false, false, &own, &nown, &obehind, oerr,
                           sizeof oerr));
    ASSERT_EQ_I(nown, 1);
    ASSERT_EQ_I((int32_t)own[0].len, 2);
    ASSERT_TRUE(!own[0].write);

    t_begin("own_chunks: filling in, a shorter copy here is extended and a "
            "missing chunk taken, cut to its complete lines");
    ASSERT_TRUE(own_chunks(a, here_lap, there, oid,
                           true, true, &own, &nown, &obehind, oerr,
                           sizeof oerr));
    ASSERT_EQ_I(nown, 2);
    ASSERT_TRUE(own[0].write && own[0].len == 4 &&
                memcmp(own[0].data, "a\nb\n", 4) == 0);
    ASSERT_TRUE(own[1].write && own[1].len == 2 &&
                memcmp(own[1].data, "c\n", 2) == 0);
    ASSERT_EQ_S(own[1].name, "0123456789ab.000002.jsonl");

    t_begin("own_chunks: filling in without extending (git brought the copy "
            "here), a shorter copy is kept as the last chunk and said to be "
            "behind");
    ASSERT_TRUE(own_chunks(a, here_lap, there, oid,
                           true, false, &own, &nown, &obehind, oerr,
                           sizeof oerr));
    ASSERT_EQ_I(nown, 1);
    ASSERT_TRUE(!own[0].write && own[0].len == 2);
    ASSERT_TRUE(obehind);
    ASSERT_TRUE(own_chunks(a, here_lap, there, oid,
                           true, true, &own, &nown, &obehind, oerr,
                           sizeof oerr));
    ASSERT_TRUE(!obehind);

    t_begin("own_chunks: a copy here that is not a prefix of the folder's is "
            "kept as it is");
    plat_write_file_atomic(here_c1, "x\n", 2);
    ASSERT_TRUE(own_chunks(a, here_lap, there, oid,
                           true, true, &own, &nown, &obehind, oerr,
                           sizeof oerr));
    ASSERT_TRUE(!own[0].write && memcmp(own[0].data, "x\n", 2) == 0);

    t_begin("own_chunks: a chunk holding only a line still being written "
            "ends them; nothing anywhere is none");
    plat_write_file_atomic(there_c2, "zz", 2);
    ASSERT_TRUE(own_chunks(a, here_lap, there, oid,
                           true, true, &own, &nown, &obehind, oerr,
                           sizeof oerr));
    ASSERT_EQ_I(nown, 1);
    remove(here_c1);
    ASSERT_TRUE(own_chunks(a, here_lap, NULL, oid, true, true,
                           &own, &nown, &obehind, oerr, sizeof oerr));
    ASSERT_EQ_I(nown, 0);
    remove(there_c1);
    remove(there_c2);
    remove(here_log);
    remove(here_lap);
    remove(here);
    remove(there_log);
    snprintf(there_log, sizeof there_log, "%s/.lap", there);
    remove(there_log);
    remove(there);

    t_begin("branches_live_of: an entry whose folder is that branch and "
            "names this folder its parent is live");
    plat_mkdirs(".live_unit_child/.lap");
    plat_mkdirs(".live_unit_other");
    plat_write_file_atomic(".live_unit_child/.lap/lineage", "0123456789ab\n",
                           13);
    plat_write_file_atomic(".live_unit_child/.lap/parent",
                           ".live_unit_parent\n", 18);
    Branches lr;
    memset(&lr, 0, sizeof lr);
    branches_add(a, &lr, (BranchEntry){"0123456789ab", "feat",
                                       ".live_unit_child", "b", "t", NULL});
    const BranchEntry *le = branches_live_of(a, &lr, ".live_unit_parent");
    ASSERT_TRUE(le != NULL);
    ASSERT_EQ_S(le->name, "feat");

    t_begin("branches_live_of: a registry copied from another folder, or "
            "naming a folder that is not that branch, is not live");
    ASSERT_TRUE(branches_live_of(a, &lr, ".live_unit_other") == NULL);
    plat_write_file_atomic(".live_unit_child/.lap/lineage", "ba9876543210\n",
                           13);
    ASSERT_TRUE(branches_live_of(a, &lr, ".live_unit_parent") == NULL);
    plat_write_file_atomic(".live_unit_child/.lap/lineage", "0123456789ab\n",
                           13);
    remove(".live_unit_child/.lap/parent");
    ASSERT_TRUE(branches_live_of(a, &lr, ".live_unit_parent") == NULL);
    remove(".live_unit_child/.lap/lineage");
    plat_rmdir(".live_unit_child/.lap");
    plat_rmdir(".live_unit_child");
    plat_rmdir(".live_unit_other");

    t_begin("branch_find_all: an id names one branch; a name every branch "
            "given it, which branch_find lists instead of picking one");
    Repo fr;
    memset(&fr, 0, sizeof fr);
    snprintf(fr.lapdir, sizeof fr.lapdir, "%s", T_REGDIR);
    Branches dup;
    memset(&dup, 0, sizeof dup);
    branches_add(a, &dup, (BranchEntry){"0123456789ab", "x", "/w/x", "h",
                                        "t", NULL});
    branches_add(a, &dup, (BranchEntry){"ba9876543210", "x", "/w/xn", "h",
                                        "t", "0123456789ab"});
    const char **fids;
    ASSERT_EQ_I(branch_find_all(a, &fr, &dup, "x", &fids), 2);
    ASSERT_EQ_I(branch_find_all(a, &fr, &dup, "ba9876543210", &fids), 1);
    ASSERT_EQ_S(fids[0], "ba9876543210");
    ASSERT_EQ_I(branch_find_all(a, &fr, &dup, "y", &fids), 0);
    const char *several;
    ASSERT_TRUE(branch_find(a, &fr, &dup, "x", &several) == NULL);
    ASSERT_EQ_S(several, "0123456789ab, ba9876543210");
    ASSERT_EQ_S(branch_find(a, &fr, &dup, "0123456789ab", &several),
                "0123456789ab");
    ASSERT_TRUE(several == NULL);

    t_begin("branch_name_used: from a nested branch's folder, the walk goes "
            "up through .lap/parent and sees every branch main sees");
    char topdir[LAP_PATH_MAX], b1dir[LAP_PATH_MAX];
    ASSERT_TRUE(plat_getcwd(topdir, sizeof topdir));
    snprintf(b1dir, sizeof b1dir, "%s/.name_unit_b1", topdir);
    snprintf(topdir + strlen(topdir), sizeof topdir - strlen(topdir),
             "/.name_unit_top");
    plat_mkdirs(".name_unit_top/.lap/log");
    plat_mkdirs(".name_unit_b1/.lap");
    Rec irec;
    memset(&irec, 0, sizeof irec);
    irec.type = REC_INIT;
    irec.version = 1;
    irec.ts = "t0";
    irec.prev = LAP_HASH_ZERO;
    size_t initn;
    char *initl = rec_encode(a, &irec, &initn);
    char *initline = arena_printf(a, "%s\n", initl);
    plat_write_file_atomic(".name_unit_top/.lap/log/main.000001.jsonl",
                           initline, strlen(initline));
    Branches treg, breg;
    memset(&treg, 0, sizeof treg);
    memset(&breg, 0, sizeof breg);
    branches_add(a, &treg, (BranchEntry){"0123456789ab", "b1", b1dir, "h",
                                         "t", NULL});
    branches_add(a, &breg, (BranchEntry){"ba9876543210", "nx", "/w/nx", "h",
                                         "t", NULL});
    ASSERT_TRUE(branches_save(a, ".name_unit_top/.lap", &treg));
    ASSERT_TRUE(branches_save(a, ".name_unit_b1/.lap", &breg));
    hist_write_lineage(".name_unit_b1/.lap", "0123456789ab");
    plat_write_file_atomic(".name_unit_b1/.lap/parent", topdir,
                           strlen(topdir));
    ASSERT_TRUE(branch_name_used(a, b1dir, "b1"));
    ASSERT_TRUE(branch_name_used(a, b1dir, "nx"));
    ASSERT_TRUE(branch_name_used(a, topdir, "nx")); /* nested, from main */
    ASSERT_TRUE(!branch_name_used(a, b1dir, "fresh"));
    ASSERT_TRUE(!branch_name_used(a, topdir, "fresh"));
    /* a folder that is no longer a branch is not climbed out of */
    remove(".name_unit_b1/.lap/lineage");
    ASSERT_TRUE(!branch_name_used(a, b1dir, "b1"));
    remove(".name_unit_b1/.lap/parent");
    remove(".name_unit_b1/.lap/" LAP_BRANCHES_NAME);
    remove(".name_unit_top/.lap/" LAP_BRANCHES_NAME);
    remove(".name_unit_top/.lap/log/main.000001.jsonl");
    plat_rmdir(".name_unit_top/.lap/log");
    plat_rmdir(".name_unit_top/.lap");
    plat_rmdir(".name_unit_top");
    plat_rmdir(".name_unit_b1/.lap");
    plat_rmdir(".name_unit_b1");

    t_begin("branches_copy_of: a folder naming a branch its parent places "
            "in another folder, which is still it, is a copy; the branch "
            "itself, or one moved away, is not");
    char cwd[LAP_PATH_MAX], cpar[LAP_PATH_MAX], corig[LAP_PATH_MAX];
    char ccopy[LAP_PATH_MAX];
    ASSERT_TRUE(plat_getcwd(cwd, sizeof cwd));
    snprintf(cpar, sizeof cpar, "%s/.copy_unit_parent", cwd);
    snprintf(corig, sizeof corig, "%s/.copy_unit_b1", cwd);
    snprintf(ccopy, sizeof ccopy, "%s/.copy_unit_x2", cwd);
    plat_mkdirs(".copy_unit_parent/.lap");
    plat_mkdirs(".copy_unit_b1/.lap");
    plat_mkdirs(".copy_unit_x2/.lap");
    Branches creg;
    memset(&creg, 0, sizeof creg);
    branches_add(a, &creg, (BranchEntry){"0123456789ab", "b1", corig, "h",
                                         "t", NULL});
    ASSERT_TRUE(branches_save(a, ".copy_unit_parent/.lap", &creg));
    hist_write_lineage(".copy_unit_b1/.lap", "0123456789ab");
    hist_write_lineage(".copy_unit_x2/.lap", "0123456789ab");
    plat_write_file_atomic(".copy_unit_b1/.lap/parent", cpar, strlen(cpar));
    plat_write_file_atomic(".copy_unit_x2/.lap/parent", cpar, strlen(cpar));
    const char *orig = NULL;
    ASSERT_TRUE(branches_copy_of(a, ".copy_unit_x2/.lap", ccopy,
                                 "0123456789ab", &orig));
    ASSERT_EQ_S(orig, corig);
    ASSERT_TRUE(!branches_copy_of(a, ".copy_unit_b1/.lap", corig,
                                  "0123456789ab", &orig));
    ASSERT_TRUE(!branches_copy_of(a, ".copy_unit_x2/.lap", ccopy,
                                  "ba9876543210", &orig)); /* unregistered */
    remove(".copy_unit_b1/.lap/lineage"); /* b1 moved away from there */
    ASSERT_TRUE(!branches_copy_of(a, ".copy_unit_x2/.lap", ccopy,
                                  "0123456789ab", &orig));
    hist_write_lineage(".copy_unit_b1/.lap", "0123456789ab");
    remove(".copy_unit_x2/.lap/parent"); /* no parent named: no registry */
    ASSERT_TRUE(!branches_copy_of(a, ".copy_unit_x2/.lap", ccopy,
                                  "0123456789ab", &orig));
    remove(".copy_unit_parent/.lap/" LAP_BRANCHES_NAME);
    remove(".copy_unit_b1/.lap/lineage");
    remove(".copy_unit_b1/.lap/parent");
    remove(".copy_unit_x2/.lap/lineage");
    plat_rmdir(".copy_unit_parent/.lap");
    plat_rmdir(".copy_unit_parent");
    plat_rmdir(".copy_unit_b1/.lap");
    plat_rmdir(".copy_unit_b1");
    plat_rmdir(".copy_unit_x2/.lap");
    plat_rmdir(".copy_unit_x2");

    t_begin("hist_lineage_leak: a lineage file is the folder's branch unless "
            "it plainly came through git");
    const char *lid = "0123456789ab";
    char lp[LAP_PATH_MAX], lb[LAP_PATH_MAX], lo[LAP_PATH_MAX];
    snprintf(lp, sizeof lp, "%s/.leak_unit_p", cwd);
    snprintf(lb, sizeof lb, "%s/.leak_unit_b", cwd);
    snprintf(lo, sizeof lo, "%s/.leak_unit_o", cwd);
    plat_mkdirs(".leak_unit_p/.lap");
    plat_mkdirs(".leak_unit_b/.lap/log");
    plat_mkdirs(".leak_unit_o/.lap");
    hist_write_lineage(".leak_unit_b/.lap", lid);
    const char *blap = ".leak_unit_b/.lap";
    /* no parent named: the branch */
    ASSERT_TRUE(hist_lineage_leak(a, blap, lid) == NULL);
    /* its parent is itself */
    plat_write_file_atomic(".leak_unit_b/.lap/parent", lb, strlen(lb));
    ASSERT_TRUE(hist_lineage_leak(a, blap, lid) != NULL);
    /* a parent without the same lineage file: the branch */
    plat_write_file_atomic(".leak_unit_b/.lap/parent", lp, strlen(lp));
    ASSERT_TRUE(hist_lineage_leak(a, blap, lid) == NULL);
    /* the parent carries it too, and lists the branch nowhere */
    hist_write_lineage(".leak_unit_p/.lap", lid);
    ASSERT_TRUE(hist_lineage_leak(a, blap, lid) != NULL);
    /* ... lists it in another folder that still is it: a checkout */
    Branches lreg;
    memset(&lreg, 0, sizeof lreg);
    branches_add(a, &lreg, (BranchEntry){lid, "b", lo, "h", "t", NULL});
    ASSERT_TRUE(branches_save(a, ".leak_unit_p/.lap", &lreg));
    hist_write_lineage(".leak_unit_o/.lap", lid);
    ASSERT_TRUE(hist_lineage_leak(a, blap, lid) != NULL);
    /* ... lists it in a folder no longer it: moved here, the branch */
    remove(".leak_unit_o/.lap/lineage");
    ASSERT_TRUE(hist_lineage_leak(a, blap, lid) == NULL);
    /* ... lists it here: the branch */
    lreg.v[0].path = lb;
    ASSERT_TRUE(branches_save(a, ".leak_unit_p/.lap", &lreg));
    ASSERT_TRUE(hist_lineage_leak(a, blap, lid) == NULL);
    /* the parent gone: the branch, unless main's chunks here run past its
     * base */
    char gone[LAP_PATH_MAX];
    snprintf(gone, sizeof gone, "%s/.leak_unit_gone", cwd);
    plat_write_file_atomic(".leak_unit_b/.lap/parent", gone, strlen(gone));
    Rec brec;
    memset(&brec, 0, sizeof brec);
    brec.type = REC_BRANCH;
    brec.id = lid;
    brec.name = "b";
    brec.parent = "main";
    brec.base = LAP_HASH_ZERO;
    brec.base_chunk = 1;
    brec.ts = "t0";
    brec.prev = LAP_HASH_ZERO;
    size_t bn;
    char *bline = arena_printf(a, "%s\n", rec_encode(a, &brec, &bn));
    plat_write_file_atomic(".leak_unit_b/.lap/log/0123456789ab.000001.jsonl",
                           bline, strlen(bline));
    plat_write_file_atomic(".leak_unit_b/.lap/log/main.000001.jsonl", "", 0);
    ASSERT_TRUE(hist_lineage_leak(a, blap, lid) == NULL);
    plat_write_file_atomic(".leak_unit_b/.lap/log/main.000002.jsonl", "", 0);
    ASSERT_TRUE(hist_lineage_leak(a, blap, lid) != NULL);
    remove(".leak_unit_b/.lap/log/0123456789ab.000001.jsonl");
    remove(".leak_unit_b/.lap/log/main.000001.jsonl");
    remove(".leak_unit_b/.lap/log/main.000002.jsonl");
    remove(".leak_unit_b/.lap/lineage");
    remove(".leak_unit_b/.lap/parent");
    remove(".leak_unit_p/.lap/lineage");
    remove(".leak_unit_p/.lap/" LAP_BRANCHES_NAME);
    plat_rmdir(".leak_unit_b/.lap/log");
    plat_rmdir(".leak_unit_b/.lap");
    plat_rmdir(".leak_unit_b");
    plat_rmdir(".leak_unit_p/.lap");
    plat_rmdir(".leak_unit_p");
    plat_rmdir(".leak_unit_o/.lap");
    plat_rmdir(".leak_unit_o");

    t_begin("repo_nested: a folder holding its own .lap/ is another "
            "repository, and a path into it is none of this one's");
    char nroot[LAP_PATH_MAX], npath[LAP_PATH_MAX], nrel[LAP_PATH_MAX];
    char nerr[512];
    snprintf(nroot, sizeof nroot, "%s/.nested_unit", cwd);
    plat_mkdirs(".nested_unit/sub/.lap");
    plat_mkdirs(".nested_unit/plain/deeper");
    ASSERT_TRUE(repo_nested(nroot, "sub"));
    ASSERT_TRUE(!repo_nested(nroot, "plain"));
    ASSERT_TRUE(!repo_nested(nroot, "plain/deeper"));
    ASSERT_TRUE(!repo_nested(nroot, "nothing"));
    Repo nr;
    memset(&nr, 0, sizeof nr);
    snprintf(nr.root, sizeof nr.root, "%s", nroot);
    snprintf(npath, sizeof npath, "%s/sub/a.txt", nroot);
    ASSERT_TRUE(!repo_relpath(&nr, npath, nrel, sizeof nrel, nerr,
                              sizeof nerr));
    ASSERT_TRUE(strstr(nerr, "sub has its own .lap/") != NULL);
    snprintf(npath, sizeof npath, "%s/plain/deeper/b.txt", nroot);
    ASSERT_TRUE(repo_relpath(&nr, npath, nrel, sizeof nrel, nerr,
                             sizeof nerr));
    ASSERT_EQ_S(nrel, "plain/deeper/b.txt");
    plat_rmdir(".nested_unit/sub/.lap");
    plat_rmdir(".nested_unit/sub");
    plat_rmdir(".nested_unit/plain/deeper");
    plat_rmdir(".nested_unit/plain");
    plat_rmdir(".nested_unit");

    remove(path);
    arena_free(a);
}
