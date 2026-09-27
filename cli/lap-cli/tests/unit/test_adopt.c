#include "adopt.h"
#include "diff.h"
#include "test.h"

/* "a\nb\n" as Lines. */
static Lines L(Arena *a, const char *text) {
    return split_lines(a, text, strlen(text));
}

/* Lines "l1" .. "ln", each ended. */
static Lines numbered(Arena *a, int32_t n) {
    StrBuf sb;
    sb_init(&sb, a);
    for (int32_t i = 1; i <= n; i++)
        sb_printf(&sb, "l%d\n", i);
    size_t len = sb.len;
    char *text = sb_finish(&sb);
    return split_lines(a, text, len);
}

/* A commit replacing lines [start, start+old.count) of `before` with the
 * lines of `repl` (text, "" for none). */
static Rec *edit(Arena *a, Lines before, int32_t start, int32_t old_n,
                 const char *repl) {
    Rec *c = (Rec *)arena_alloc0(a, sizeof(Rec));
    c->type = REC_COMMIT;
    c->op = "edit";
    c->old_start = c->new_start = start;
    c->old_lines = old_n;
    c->old_text = before.lines ? before.lines + (start - 1) : NULL;
    c->old_n = old_n;
    Lines r = L(a, repl);
    c->new_text = r.lines;
    c->new_n = c->new_lines = r.count;
    c->eof_nl = before.eof_nl;
    return c;
}

/* The version `before` becomes after c. */
static Lines after(Arena *a, Lines before, const Rec *c) {
    return lines_replace(a, before, c->old_start, c->old_lines, c->new_text,
                         c->new_n, c->eof_nl);
}

static const char *text(Arena *a, Lines l) {
    return join_lines(a, l, NULL);
}

void test_adopt(void) {
    Arena *a = arena_new(0);
    Placement p;

    t_begin("adopt: an import on the parent moves the branch's function "
            "down one line");
    Lines base = numbered(a, 60);
    Lines parent = lines_replace(a, base, 4, 0, L(a, "import x\n").lines, 1,
                                 true);
    const Rec *c1[] = {edit(a, base, 51, 0, "fn() {\n}\n")};
    adopt_place(a, base, parent, c1, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_I(p.start[0], 52);
    ASSERT_EQ_I(p.result.count, 63);
    ASSERT_TRUE(str_eq_c(p.result.lines[3], "import x"));
    ASSERT_TRUE(str_eq_c(p.result.lines[51], "fn() {"));
    ASSERT_TRUE(str_eq_c(p.result.lines[53], "l51"));
    ASSERT_TRUE(p.why == NULL);

    t_begin("adopt: a parent change below the branch's does not move it");
    parent = lines_replace(a, base, 50, 1, L(a, "L50\n").lines, 1, true);
    const Rec *c2[] = {edit(a, base, 10, 1, "L10\n")};
    adopt_place(a, base, parent, c2, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_I(p.start[0], 10);
    ASSERT_TRUE(str_eq_c(p.result.lines[9], "L10"));
    ASSERT_TRUE(str_eq_c(p.result.lines[49], "L50"));

    t_begin("adopt: deletions on the parent above move the branch's edit up");
    parent = lines_replace(a, base, 2, 3, NULL, 0, true);
    const Rec *c3[] = {edit(a, base, 20, 1, "L20\n")};
    adopt_place(a, base, parent, c3, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_I(p.start[0], 17);
    ASSERT_TRUE(str_eq_c(p.result.lines[16], "L20"));
    ASSERT_EQ_I(p.result.count, 57);

    t_begin("adopt: several branch commits shift each other and the parent's "
            "changes");
    /* the parent inserts before line 5 and before line 20; the branch
     * inserts two lines before line 10, then edits what is then line 40
     * (the base's line 38) */
    Lines p1 = lines_replace(a, base, 20, 0, L(a, "p20\n").lines, 1, true);
    parent = lines_replace(a, p1, 5, 0, L(a, "p5\n").lines, 1, true);
    Rec *b1 = edit(a, base, 10, 0, "b10a\nb10b\n");
    Lines bv = after(a, base, b1);
    Rec *b2 = edit(a, bv, 40, 1, "B40\n");
    ASSERT_TRUE(str_eq_c(b2->old_text[0], "l38"));
    const Rec *c4[] = {b1, b2};
    adopt_place(a, base, parent, c4, 2, &p);
    ASSERT_EQ_I(p.placed, 2);
    ASSERT_EQ_I(p.start[0], 11);
    ASSERT_EQ_I(p.start[1], 42);
    ASSERT_TRUE(str_eq_c(p.result.lines[10], "b10a"));
    ASSERT_TRUE(str_eq_c(p.result.lines[41], "B40"));
    ASSERT_TRUE(str_eq_c(p.result.lines[22], "p20"));
    ASSERT_EQ_I(p.result.count, 64);

    t_begin("adopt: a region overlapping a parent change is a conflict");
    parent = lines_replace(a, base, 10, 1, L(a, "P10\n").lines, 1, true);
    const Rec *c5[] = {edit(a, base, 9, 3, "x\n")};
    adopt_place(a, base, parent, c5, 1, &p);
    ASSERT_EQ_I(p.placed, 0);
    ASSERT_TRUE(p.why && strstr(p.why, "overlaps") != NULL);
    ASSERT_EQ_S(text(a, p.result), text(a, parent));

    t_begin("adopt: both sides inserting at one point is a conflict");
    parent = lines_replace(a, base, 10, 0, L(a, "p\n").lines, 1, true);
    const Rec *c6[] = {edit(a, base, 10, 0, "b\n")};
    adopt_place(a, base, parent, c6, 1, &p);
    ASSERT_EQ_I(p.placed, 0);
    ASSERT_TRUE(p.why && strstr(p.why, "same point") != NULL);

    t_begin("adopt: touching a parent change at its edge is a conflict; one "
            "line away is not");
    parent = lines_replace(a, base, 10, 1, L(a, "P10\n").lines, 1, true);
    const Rec *c7[] = {edit(a, base, 11, 1, "B11\n")};
    adopt_place(a, base, parent, c7, 1, &p);
    ASSERT_EQ_I(p.placed, 0);
    const Rec *c7b[] = {edit(a, base, 9, 1, "B9\n")};
    adopt_place(a, base, parent, c7b, 1, &p);
    ASSERT_EQ_I(p.placed, 0);
    const Rec *c7c[] = {edit(a, base, 12, 1, "B12\n")};
    adopt_place(a, base, parent, c7c, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_I(p.start[0], 12);

    t_begin("adopt: the first conflict stops the file");
    Rec *ok1 = edit(a, base, 30, 1, "B30\n");
    Rec *bad = edit(a, after(a, base, ok1), 10, 1, "B10\n");
    Rec *ok2 = edit(a, after(a, after(a, base, ok1), bad), 40, 1, "B40\n");
    const Rec *c8[] = {ok1, bad, ok2};
    adopt_place(a, base, parent, c8, 3, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_TRUE(str_eq_c(p.result.lines[29], "B30"));
    ASSERT_TRUE(str_eq_c(p.result.lines[39], "l40"));

    t_begin("adopt: pure insertions at the very top and the very bottom");
    parent = lines_replace(a, base, 30, 1, L(a, "P30\n").lines, 1, true);
    const Rec *c9[] = {edit(a, base, 1, 0, "top\n")};
    adopt_place(a, base, parent, c9, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_I(p.start[0], 1);
    ASSERT_TRUE(str_eq_c(p.result.lines[0], "top"));
    parent = lines_replace(a, base, 2, 0, L(a, "p2\n").lines, 1, true);
    const Rec *c10[] = {edit(a, base, 61, 0, "bottom\n")};
    adopt_place(a, base, parent, c10, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_I(p.start[0], 62);
    ASSERT_TRUE(str_eq_c(p.result.lines[61], "bottom"));
    ASSERT_EQ_I(p.result.count, 62);

    t_begin("adopt: comparison is blind to CRLF carriage returns");
    Lines crbase = L(a, "a\r\nb\r\nc\r\n");
    Lines crparent = L(a, "a\r\nb\r\nc\r\nd\r\n");
    Lines lfview = L(a, "a\nb\nc\n");
    const Rec *c11[] = {edit(a, lfview, 2, 1, "B\n")};
    adopt_place(a, crbase, crparent, c11, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_I(p.start[0], 2);
    ASSERT_TRUE(str_eq_c(p.result.lines[1], "B"));

    t_begin("adopt: a commit whose old text is not the parent's there is a "
            "conflict");
    Lines other = L(a, "x\ny\nz\n");
    const Rec *c12[] = {edit(a, other, 2, 1, "Y\n")};
    adopt_place(a, lfview, lfview, c12, 1, &p);
    ASSERT_EQ_I(p.placed, 0);
    ASSERT_TRUE(p.why && strstr(p.why, "not the parent's") != NULL);

    t_begin("adopt: the parent's missing final newline survives a branch "
            "edit that did not touch it");
    Lines noeol = L(a, "a\nb\nc");
    Rec *c13 = edit(a, lfview, 1, 1, "A\n");
    const Rec *c13v[] = {c13};
    adopt_place(a, lfview, noeol, c13v, 1, &p);
    ASSERT_EQ_I(p.placed, 1); /* the parent's change is line 3, not 1 */
    ASSERT_EQ_S(text(a, p.result), "A\nb\nc");
    Lines five = L(a, "a\nb\nc\nd\ne\n");
    Lines five_noeol = L(a, "a\nb\nc\nd\ne");
    Rec *c14 = edit(a, five, 1, 1, "A\n");
    const Rec *c14v[] = {c14};
    adopt_place(a, five, five_noeol, c14v, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_TRUE(!p.eof_nl[0]);
    ASSERT_EQ_S(text(a, p.result), "A\nb\nc\nd\ne");

    t_begin("adopt: a delete is placed on an unchanged file, refused on a "
            "changed one");
    Rec *del = edit(a, five, 1, 5, "");
    del->op = "delete";
    const Rec *c15[] = {del};
    adopt_place(a, five, five, c15, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_I(p.result.count, 0);
    Lines five_changed = L(a, "a\nb\nC\nd\ne\n");
    adopt_place(a, five, five_changed, c15, 1, &p);
    ASSERT_EQ_I(p.placed, 0);

    t_begin("adopt: a file created on the branch alone is placed; created on "
            "both, it conflicts");
    Lines none = {NULL, 0, true};
    Rec *cr = edit(a, none, 1, 0, "new\nfile\n");
    cr->op = "create";
    const Rec *c16[] = {cr};
    adopt_place(a, none, none, c16, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_S(text(a, p.result), "new\nfile\n");
    adopt_place(a, none, L(a, "theirs\n"), c16, 1, &p);
    ASSERT_EQ_I(p.placed, 0);

    t_begin("adopt: a parent file rewritten past the diff's effort cap is one "
            "change, so a branch edit anywhere in it conflicts");
    Lines big = numbered(a, 3000);
    Lines rewritten = big;
    for (int32_t k = 2; k <= 2998; k += 2) /* 1,499 separate changes */
        rewritten = lines_replace(a, rewritten, k, 1, L(a, "x\n").lines, 1,
                                  true);
    ASSERT_TRUE(diff_lines(a, big, rewritten).truncated);
    const Rec *c17[] = {edit(a, big, 1501, 0, "branch\n")};
    adopt_place(a, big, rewritten, c17, 1, &p);
    ASSERT_EQ_I(p.placed, 0);

    t_begin("adopt: the same edit on both sides is already done, and the "
            "file's later commits carry on");
    Lines twenty = numbered(a, 20);
    Rec *same = edit(a, twenty, 5, 1, "X\n");
    Lines same_v = after(a, twenty, same);
    const Rec *c18[] = {same, edit(a, same_v, 12, 1, "Y\n")};
    adopt_place(a, twenty, same_v, c18, 2, &p);
    ASSERT_EQ_I(p.placed, 2);
    ASSERT_TRUE(p.already[0] && !p.already[1]);
    ASSERT_EQ_I(p.start[1], 12);
    ASSERT_TRUE(str_eq_c(p.result.lines[4], "X"));
    ASSERT_TRUE(str_eq_c(p.result.lines[11], "Y"));
    ASSERT_EQ_I(p.result.count, 20);
    ASSERT_TRUE(p.why == NULL);

    t_begin("adopt: a near-identical edit, one line different, still "
            "conflicts");
    Lines near = lines_replace(a, twenty, 5, 1, L(a, "X2\n").lines, 1, true);
    adopt_place(a, twenty, near, c18, 2, &p);
    ASSERT_EQ_I(p.placed, 0);
    ASSERT_TRUE(!p.already[0]);
    ASSERT_TRUE(p.why != NULL);

    t_begin("adopt: a file deleted on both sides is already done");
    adopt_place(a, five, none, c15, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_TRUE(p.already[0]);
    ASSERT_EQ_I(p.result.count, 0);

    t_begin("adopt: a file created on both sides with the same lines is "
            "already done");
    adopt_place(a, none, L(a, "new\nfile\n"), c16, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_TRUE(p.already[0]);
    ASSERT_EQ_S(text(a, p.result), "new\nfile\n");

    t_begin("adopt: an identical insertion becomes common ground: later "
            "commits are not moved by it twice");
    Rec *ins = edit(a, twenty, 3, 0, "I\n");
    Lines bi = after(a, twenty, ins);
    Lines pi = lines_replace(a, bi, 16, 1, L(a, "P\n").lines, 1, true);
    const Rec *c19[] = {ins, edit(a, bi, 10, 1, "Z\n")};
    adopt_place(a, twenty, pi, c19, 2, &p);
    ASSERT_EQ_I(p.placed, 2);
    ASSERT_TRUE(p.already[0] && !p.already[1]);
    ASSERT_EQ_I(p.start[1], 10);
    ASSERT_TRUE(str_eq_c(p.result.lines[9], "Z"));
    ASSERT_TRUE(str_eq_c(p.result.lines[15], "P"));

    arena_free(a);
}
