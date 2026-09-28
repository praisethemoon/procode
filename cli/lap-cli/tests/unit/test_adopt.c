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
    adopt_place(a, base, parent, true, c1, 1, &p);
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
    adopt_place(a, base, parent, true, c2, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_I(p.start[0], 10);
    ASSERT_TRUE(str_eq_c(p.result.lines[9], "L10"));
    ASSERT_TRUE(str_eq_c(p.result.lines[49], "L50"));

    t_begin("adopt: deletions on the parent above move the branch's edit up");
    parent = lines_replace(a, base, 2, 3, NULL, 0, true);
    const Rec *c3[] = {edit(a, base, 20, 1, "L20\n")};
    adopt_place(a, base, parent, true, c3, 1, &p);
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
    adopt_place(a, base, parent, true, c4, 2, &p);
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
    adopt_place(a, base, parent, true, c5, 1, &p);
    ASSERT_EQ_I(p.placed, 0);
    ASSERT_TRUE(p.why && strstr(p.why, "overlaps") != NULL);
    ASSERT_EQ_S(text(a, p.result), text(a, parent));

    t_begin("adopt: both sides inserting at one point is a conflict");
    parent = lines_replace(a, base, 10, 0, L(a, "p\n").lines, 1, true);
    const Rec *c6[] = {edit(a, base, 10, 0, "b\n")};
    adopt_place(a, base, parent, true, c6, 1, &p);
    ASSERT_EQ_I(p.placed, 0);
    ASSERT_TRUE(p.why && strstr(p.why, "same point") != NULL);

    t_begin("adopt: touching a parent change at its edge is a conflict; one "
            "line away is not");
    parent = lines_replace(a, base, 10, 1, L(a, "P10\n").lines, 1, true);
    const Rec *c7[] = {edit(a, base, 11, 1, "B11\n")};
    adopt_place(a, base, parent, true, c7, 1, &p);
    ASSERT_EQ_I(p.placed, 0);
    const Rec *c7b[] = {edit(a, base, 9, 1, "B9\n")};
    adopt_place(a, base, parent, true, c7b, 1, &p);
    ASSERT_EQ_I(p.placed, 0);
    const Rec *c7c[] = {edit(a, base, 12, 1, "B12\n")};
    adopt_place(a, base, parent, true, c7c, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_I(p.start[0], 12);

    t_begin("adopt: the first conflict stops the file");
    Rec *ok1 = edit(a, base, 30, 1, "B30\n");
    Rec *bad = edit(a, after(a, base, ok1), 10, 1, "B10\n");
    Rec *ok2 = edit(a, after(a, after(a, base, ok1), bad), 40, 1, "B40\n");
    const Rec *c8[] = {ok1, bad, ok2};
    adopt_place(a, base, parent, true, c8, 3, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_TRUE(str_eq_c(p.result.lines[29], "B30"));
    ASSERT_TRUE(str_eq_c(p.result.lines[39], "l40"));

    t_begin("adopt: pure insertions at the very top and the very bottom");
    parent = lines_replace(a, base, 30, 1, L(a, "P30\n").lines, 1, true);
    const Rec *c9[] = {edit(a, base, 1, 0, "top\n")};
    adopt_place(a, base, parent, true, c9, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_I(p.start[0], 1);
    ASSERT_TRUE(str_eq_c(p.result.lines[0], "top"));
    parent = lines_replace(a, base, 2, 0, L(a, "p2\n").lines, 1, true);
    const Rec *c10[] = {edit(a, base, 61, 0, "bottom\n")};
    adopt_place(a, base, parent, true, c10, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_I(p.start[0], 62);
    ASSERT_TRUE(str_eq_c(p.result.lines[61], "bottom"));
    ASSERT_EQ_I(p.result.count, 62);

    t_begin("adopt: comparison is blind to CRLF carriage returns");
    Lines crbase = L(a, "a\r\nb\r\nc\r\n");
    Lines crparent = L(a, "a\r\nb\r\nc\r\nd\r\n");
    Lines lfview = L(a, "a\nb\nc\n");
    const Rec *c11[] = {edit(a, lfview, 2, 1, "B\n")};
    adopt_place(a, crbase, crparent, true, c11, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_I(p.start[0], 2);
    ASSERT_TRUE(str_eq_c(p.result.lines[1], "B"));

    t_begin("adopt: a commit whose old text is not the parent's there is a "
            "conflict");
    Lines other = L(a, "x\ny\nz\n");
    const Rec *c12[] = {edit(a, other, 2, 1, "Y\n")};
    adopt_place(a, lfview, lfview, true, c12, 1, &p);
    ASSERT_EQ_I(p.placed, 0);
    ASSERT_TRUE(p.why && strstr(p.why, "not the parent's") != NULL);

    t_begin("adopt: the parent's missing final newline survives a branch "
            "edit that did not touch it");
    Lines noeol = L(a, "a\nb\nc");
    Rec *c13 = edit(a, lfview, 1, 1, "A\n");
    const Rec *c13v[] = {c13};
    adopt_place(a, lfview, noeol, true, c13v, 1, &p);
    ASSERT_EQ_I(p.placed, 1); /* the parent's change is line 3, not 1 */
    ASSERT_EQ_S(text(a, p.result), "A\nb\nc");
    Lines five = L(a, "a\nb\nc\nd\ne\n");
    Lines five_noeol = L(a, "a\nb\nc\nd\ne");
    Rec *c14 = edit(a, five, 1, 1, "A\n");
    const Rec *c14v[] = {c14};
    adopt_place(a, five, five_noeol, true, c14v, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_TRUE(!p.eof_nl[0]);
    ASSERT_EQ_S(text(a, p.result), "A\nb\nc\nd\ne");

    t_begin("adopt: a delete is placed on an unchanged file, refused on a "
            "changed one");
    Rec *del = edit(a, five, 1, 5, "");
    del->op = "delete";
    const Rec *c15[] = {del};
    adopt_place(a, five, five, true, c15, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_I(p.result.count, 0);
    Lines five_changed = L(a, "a\nb\nC\nd\ne\n");
    adopt_place(a, five, five_changed, true, c15, 1, &p);
    ASSERT_EQ_I(p.placed, 0);

    t_begin("adopt: a file created on the branch alone is placed; created on "
            "both, it conflicts");
    Lines none = {NULL, 0, true};
    Rec *cr = edit(a, none, 1, 0, "new\nfile\n");
    cr->op = "create";
    const Rec *c16[] = {cr};
    adopt_place(a, none, none, false, c16, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_EQ_S(text(a, p.result), "new\nfile\n");
    adopt_place(a, none, L(a, "theirs\n"), true, c16, 1, &p);
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
    adopt_place(a, big, rewritten, true, c17, 1, &p);
    ASSERT_EQ_I(p.placed, 0);

    t_begin("adopt: the same edit on both sides is already done, and the "
            "file's later commits carry on");
    Lines twenty = numbered(a, 20);
    Rec *same = edit(a, twenty, 5, 1, "X\n");
    Lines same_v = after(a, twenty, same);
    const Rec *c18[] = {same, edit(a, same_v, 12, 1, "Y\n")};
    adopt_place(a, twenty, same_v, true, c18, 2, &p);
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
    adopt_place(a, twenty, near, true, c18, 2, &p);
    ASSERT_EQ_I(p.placed, 0);
    ASSERT_TRUE(!p.already[0]);
    ASSERT_TRUE(p.why != NULL);

    t_begin("adopt: a file deleted on both sides is already done");
    adopt_place(a, five, none, false, c15, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_TRUE(p.already[0]);
    ASSERT_EQ_I(p.result.count, 0);

    t_begin("adopt: a file created on both sides with the same lines is "
            "already done");
    adopt_place(a, none, L(a, "new\nfile\n"), true, c16, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_TRUE(p.already[0]);
    ASSERT_EQ_S(text(a, p.result), "new\nfile\n");

    t_begin("adopt: an identical insertion becomes common ground: later "
            "commits are not moved by it twice");
    Rec *ins = edit(a, twenty, 3, 0, "I\n");
    Lines bi = after(a, twenty, ins);
    Lines pi = lines_replace(a, bi, 16, 1, L(a, "P\n").lines, 1, true);
    const Rec *c19[] = {ins, edit(a, bi, 10, 1, "Z\n")};
    adopt_place(a, twenty, pi, true, c19, 2, &p);
    ASSERT_EQ_I(p.placed, 2);
    ASSERT_TRUE(p.already[0] && !p.already[1]);
    ASSERT_EQ_I(p.start[1], 10);
    ASSERT_TRUE(str_eq_c(p.result.lines[9], "Z"));
    ASSERT_TRUE(str_eq_c(p.result.lines[15], "P"));

    t_begin("adopt: an edit to an empty file the parent deleted conflicts; "
            "deleting an empty file the parent wrote into conflicts");
    Lines empty = {NULL, 0, true}; /* e.txt exists, with no lines */
    Rec *write = edit(a, empty, 1, 0, "hello\n");
    const Rec *c20[] = {write};
    adopt_place(a, empty, none, false, c20, 1, &p);
    ASSERT_EQ_I(p.placed, 0);
    ASSERT_EQ_S(p.why, "the parent deleted the file");
    Rec *del_empty = edit(a, empty, 1, 0, "");
    del_empty->op = "delete";
    const Rec *c21[] = {del_empty};
    adopt_place(a, empty, L(a, "hello\n"), true, c21, 1, &p);
    ASSERT_EQ_I(p.placed, 0);

    t_begin("adopt: created on both sides, empty on one and not the other, "
            "conflicts either way; empty on both is already done");
    Rec *cr_full = edit(a, none, 1, 0, "content\n");
    cr_full->op = "create";
    const Rec *c22[] = {cr_full};
    adopt_place(a, none, empty, true, c22, 1, &p);
    ASSERT_EQ_I(p.placed, 0);
    ASSERT_EQ_S(p.why, "both sides create it");
    Rec *cr_empty = edit(a, none, 1, 0, "");
    cr_empty->op = "create";
    const Rec *c23[] = {cr_empty};
    adopt_place(a, none, L(a, "content\n"), true, c23, 1, &p);
    ASSERT_EQ_I(p.placed, 0);
    adopt_place(a, none, empty, true, c23, 1, &p);
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_TRUE(p.already[0]);
    adopt_place(a, none, none, false, c23, 1, &p); /* the branch's alone */
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_TRUE(!p.already[0]);

    t_begin("adopt: an empty file deleted on both sides is already done, and "
            "a later create carries on");
    Rec *recreate = edit(a, none, 1, 0, "back\n");
    recreate->op = "create";
    const Rec *c24[] = {del_empty, recreate};
    adopt_place(a, empty, none, false, c24, 2, &p);
    ASSERT_EQ_I(p.placed, 2);
    ASSERT_TRUE(p.already[0] && !p.already[1]);
    ASSERT_EQ_S(text(a, p.result), "back\n");

    t_begin("adopt: a change both sides made in several steps is already "
            "done, and the file's later commits carry on");
    Lines ten = numbered(a, 10);
    Rec *step1 = edit(a, ten, 5, 1, "five\n");
    Lines ten1 = after(a, ten, step1);
    Rec *step2 = edit(a, ten1, 5, 1, "FIVE\n");
    Lines ten2 = after(a, ten1, step2);
    Rec *later = edit(a, ten2, 9, 1, "NINE\n");
    const Rec *c25[] = {step1, step2, later};
    adopt_place(a, ten, ten2, true, c25, 3, &p); /* the parent: ten2 */
    ASSERT_EQ_I(p.placed, 3);
    ASSERT_TRUE(p.already[0] && p.already[1] && !p.already[2]);
    ASSERT_TRUE(p.why == NULL);
    ASSERT_TRUE(str_eq_c(p.result.lines[4], "FIVE"));
    ASSERT_TRUE(str_eq_c(p.result.lines[8], "NINE"));
    ASSERT_EQ_I(p.start[2], 9);

    t_begin("adopt: steps that only start like the parent's change, or end "
            "elsewhere, still conflict");
    Rec *other2 = edit(a, ten1, 5, 1, "fivex\n");
    const Rec *c26[] = {step1, other2};
    adopt_place(a, ten, ten2, true, c26, 2, &p);
    ASSERT_EQ_I(p.placed, 0);
    ASSERT_TRUE(!p.already[0] && p.why != NULL);
    const Rec *c27[] = {step1}; /* only the first step on the branch */
    adopt_place(a, ten, ten2, true, c27, 1, &p);
    ASSERT_EQ_I(p.placed, 0);
    ASSERT_TRUE(p.why != NULL);

    t_begin("adopt: a run of steps may grow the region: an insertion then an "
            "edit inside it reaching the parent's lines");
    Rec *grow = edit(a, ten, 5, 1, "5a\n5b\n");
    Lines g1 = after(a, ten, grow);
    Rec *fix = edit(a, g1, 6, 1, "5B\n");
    Lines g2 = after(a, g1, fix);
    const Rec *c28[] = {grow, fix};
    adopt_place(a, ten, g2, true, c28, 2, &p);
    ASSERT_EQ_I(p.placed, 2);
    ASSERT_TRUE(p.already[0] && p.already[1]);
    ASSERT_EQ_S(text(a, p.result), text(a, g2));

    t_begin("adopt: a change already done still counts as the parent's: the "
            "branch undoing its copy stops the file, the parent's kept");
    Rec *addx = edit(a, ten, 6, 0, "X\n");
    Lines tx = after(a, ten, addx);
    Rec *delx = edit(a, tx, 6, 1, "");
    const Rec *c29[] = {addx, delx};
    adopt_place(a, ten, tx, true, c29, 2, &p); /* the parent: ten + X */
    ASSERT_EQ_I(p.placed, 1);
    ASSERT_TRUE(p.already[0]);
    ASSERT_TRUE(p.why != NULL);
    ASSERT_EQ_S(text(a, p.result), text(a, tx));
    /* the same after a change made in steps */
    Rec *undo = edit(a, ten2, 5, 1, "5\n");
    const Rec *c30[] = {step1, step2, undo};
    adopt_place(a, ten, ten2, true, c30, 3, &p);
    ASSERT_EQ_I(p.placed, 2);
    ASSERT_TRUE(p.why != NULL);
    ASSERT_EQ_S(text(a, p.result), text(a, ten2));
    /* a commit clear of it, beyond one unchanged line, carries on */
    Rec *far = edit(a, tx, 8, 1, "EIGHT\n");
    const Rec *c31[] = {addx, far};
    adopt_place(a, ten, tx, true, c31, 2, &p);
    ASSERT_EQ_I(p.placed, 2);
    ASSERT_TRUE(p.why == NULL);
    ASSERT_TRUE(str_eq_c(p.result.lines[7], "EIGHT"));

    arena_free(a);
}
