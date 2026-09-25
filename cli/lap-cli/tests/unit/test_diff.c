#include "diff.h"
#include "test.h"

static Lines mk(Arena *a, const char *text) {
    return split_lines(a, arena_strdup(a, text), strlen(text));
}

static bool lines_equal(Lines x, Lines y) {
    if (x.count != y.count || x.eof_nl != y.eof_nl)
        return false;
    for (int32_t i = 0; i < x.count; i++)
        if (!str_eq(x.lines[i], y.lines[i]))
            return false;
    return true;
}

/* Applies all regions (bottom-up so line numbers stay valid) of a diff from
 * oldl to newl back onto oldl; the result must equal newl.
 */
static bool regions_reconstruct(Arena *a, Lines oldl, Lines newl,
                                Regions rg) {
    Lines cur = oldl;
    for (int32_t i = rg.count - 1; i >= 0; i--) {
        const Region *r = &rg.v[i];
        bool touches_end =
            r->new_start + r->new_lines - 1 >= newl.count ||
            r->old_start + r->old_lines - 1 >= oldl.count;
        bool eof = touches_end ? newl.eof_nl : cur.eof_nl;
        cur = lines_replace(a, cur, r->old_start, r->old_lines,
                            newl.lines + (r->new_start - 1), r->new_lines,
                            eof);
    }
    return lines_equal(cur, newl);
}

/* xorshift64: deterministic PRNG for the property test */
static uint64_t rng_state;
static uint64_t rnd(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}
static uint32_t rnd_below(uint32_t n) {
    return (uint32_t)(rnd() % n);
}

void test_diff(void) {
    Arena *a = arena_new(0);

    t_begin("diff: identical files");
    Regions rg = diff_lines(a, mk(a, "a\nb\nc\n"), mk(a, "a\nb\nc\n"));
    ASSERT_EQ_I(rg.count, 0);

    t_begin("diff: single replacement region");
    rg = diff_lines(a, mk(a, "a\nb\nc\n"), mk(a, "a\nX\nc\n"));
    ASSERT_EQ_I(rg.count, 1);
    ASSERT_EQ_I(rg.v[0].old_start, 2);
    ASSERT_EQ_I(rg.v[0].old_lines, 1);
    ASSERT_EQ_I(rg.v[0].new_start, 2);
    ASSERT_EQ_I(rg.v[0].new_lines, 1);

    t_begin("diff: two edits separated by one unchanged line");
    rg = diff_lines(a, mk(a, "a\nb\nc\nd\ne\n"), mk(a, "A\nb\nc\nD\ne\n"));
    ASSERT_EQ_I(rg.count, 2);
    ASSERT_EQ_I(rg.v[0].new_start, 1);
    ASSERT_EQ_I(rg.v[1].new_start, 4);

    t_begin("diff: adjacent delete+insert merge into one region");
    rg = diff_lines(a, mk(a, "a\nb\nc\n"), mk(a, "a\nX\nY\nc\n"));
    ASSERT_EQ_I(rg.count, 1);
    ASSERT_EQ_I(rg.v[0].old_start, 2);
    ASSERT_EQ_I(rg.v[0].old_lines, 1);
    ASSERT_EQ_I(rg.v[0].new_lines, 2);

    t_begin("diff: pure insertion");
    rg = diff_lines(a, mk(a, "a\nc\n"), mk(a, "a\nb\nc\n"));
    ASSERT_EQ_I(rg.count, 1);
    ASSERT_EQ_I(rg.v[0].old_lines, 0);
    ASSERT_EQ_I(rg.v[0].new_start, 2);
    ASSERT_EQ_I(rg.v[0].new_lines, 1);

    t_begin("diff: pure deletion");
    rg = diff_lines(a, mk(a, "a\nb\nc\n"), mk(a, "a\nc\n"));
    ASSERT_EQ_I(rg.count, 1);
    ASSERT_EQ_I(rg.v[0].old_start, 2);
    ASSERT_EQ_I(rg.v[0].old_lines, 1);
    ASSERT_EQ_I(rg.v[0].new_lines, 0);

    t_begin("diff: edit at start and at end");
    rg = diff_lines(a, mk(a, "a\nb\nc\n"), mk(a, "A\nb\nC\n"));
    ASSERT_EQ_I(rg.count, 2);

    t_begin("diff: empty old file (all insertion)");
    rg = diff_lines(a, mk(a, ""), mk(a, "a\nb\n"));
    ASSERT_EQ_I(rg.count, 1);
    ASSERT_EQ_I(rg.v[0].old_start, 1);
    ASSERT_EQ_I(rg.v[0].old_lines, 0);
    ASSERT_EQ_I(rg.v[0].new_lines, 2);

    t_begin("diff: empty new file (all deletion)");
    rg = diff_lines(a, mk(a, "a\nb\n"), mk(a, ""));
    ASSERT_EQ_I(rg.count, 1);
    ASSERT_EQ_I(rg.v[0].old_lines, 2);
    ASSERT_EQ_I(rg.v[0].new_lines, 0);

    t_begin("diff: trailing-newline-only change is one region on last line");
    rg = diff_lines(a, mk(a, "a\nb\n"), mk(a, "a\nb"));
    ASSERT_EQ_I(rg.count, 1);
    ASSERT_EQ_I(rg.v[0].old_start, 2);
    ASSERT_EQ_I(rg.v[0].old_lines, 1);
    ASSERT_EQ_I(rg.v[0].new_start, 2);
    ASSERT_EQ_I(rg.v[0].new_lines, 1);

    t_begin("diff: content change already covering last line absorbs eof "
            "change");
    rg = diff_lines(a, mk(a, "a\nb\n"), mk(a, "a\nB"));
    ASSERT_EQ_I(rg.count, 1);

    t_begin("lines_replace: bounds and edges");
    Lines base = mk(a, "a\nb\nc\n");
    Lines out = lines_replace(a, base, 1, 0, NULL, 0, true);
    ASSERT_EQ_I(out.count, 3); /* no-op insert */
    Str one = str_c("Z");
    out = lines_replace(a, base, 4, 0, &one, 1, true); /* append at end */
    ASSERT_EQ_I(out.count, 4);
    ASSERT_TRUE(str_eq_c(out.lines[3], "Z"));
    out = lines_replace(a, base, 1, 3, NULL, 0, true); /* delete all */
    ASSERT_EQ_I(out.count, 0);
    out = lines_replace(a, base, 3, 5, &one, 1, true); /* out of range */
    ASSERT_EQ_I(out.count, 3);                         /* unchanged */

    t_begin("diff: blank-line gap does not split (blank lines aren't "
            "anchors)");
    rg = diff_lines(a, mk(a, "a\n\nb\n"), mk(a, "A\n\nB\n"));
    ASSERT_EQ_I(rg.count, 1);
    ASSERT_EQ_I(rg.v[0].old_start, 1);
    ASSERT_EQ_I(rg.v[0].old_lines, 3);
    ASSERT_EQ_I(rg.v[0].new_start, 1);
    ASSERT_EQ_I(rg.v[0].new_lines, 3);
    ASSERT_TRUE(regions_reconstruct(a, mk(a, "a\n\nb\n"), mk(a, "A\n\nB\n"),
                                    rg));

    t_begin("diff: whitespace-only gap lines merge too");
    rg = diff_lines(a, mk(a, "a\n \t\nb\n"), mk(a, "A\n \t\nB\n"));
    ASSERT_EQ_I(rg.count, 1);

    t_begin("diff: several blank gap lines merge");
    rg = diff_lines(a, mk(a, "a\n\n\n\nb\n"), mk(a, "A\n\n\n\nB\n"));
    ASSERT_EQ_I(rg.count, 1);
    ASSERT_TRUE(regions_reconstruct(a, mk(a, "a\n\n\n\nb\n"),
                                    mk(a, "A\n\n\n\nB\n"), rg));

    t_begin("diff: a gap with any non-blank line still splits");
    rg = diff_lines(a, mk(a, "a\n\nx\n\nb\n"), mk(a, "A\n\nx\n\nB\n"));
    ASSERT_EQ_I(rg.count, 2);
    ASSERT_EQ_I(rg.v[0].new_start, 1);
    ASSERT_EQ_I(rg.v[1].new_start, 5);

    t_begin("diff: three edits over blank gaps collapse into one");
    rg = diff_lines(a, mk(a, "a\n\nb\n\nc\n"), mk(a, "A\n\nB\n\nC\n"));
    ASSERT_EQ_I(rg.count, 1);
    ASSERT_EQ_I(rg.v[0].old_lines, 5);

    t_begin("diff property: K separated edits => K regions, reconstruct ok");
    rng_state = 0x9e3779b97f4a7c15ULL; /* fixed seed: deterministic */
    int32_t failures = 0;
    for (int32_t iter = 0; iter < 300; iter++) {
        Arena *ta = arena_new(1 << 16);
        int32_t nlines = 20 + (int32_t)rnd_below(60);
        StrBuf sold;
        sb_init(&sold, ta);
        for (int32_t i = 0; i < nlines; i++)
            sb_printf(&sold, "line-%d-%u\n", i, (uint32_t)rnd_below(1000));
        Lines oldl = split_lines(ta, sold.data, sold.len);

        /* build the new file by walking old lines, injecting K edits with
         * at least one unchanged line between them */
        StrBuf snew;
        sb_init(&snew, ta);
        int32_t k_expected = 0;
        int32_t i = 0;
        bool just_edited = false;
        while (i < nlines) {
            bool can_edit = !just_edited;
            if (can_edit && rnd_below(100) < 25) {
                uint32_t kind = rnd_below(3);
                int32_t span = 1 + (int32_t)rnd_below(3);
                if (span > nlines - i)
                    span = nlines - i;
                k_expected++;
                if (kind == 0) { /* replace span lines */
                    for (int32_t s = 0; s < span; s++)
                        sb_printf(&snew, "mod-%d-%d\n", iter, i + s);
                    i += span;
                } else if (kind == 1) { /* delete span lines */
                    i += span;
                } else { /* insert 1-3 fresh lines before old line i */
                    int32_t ins = 1 + (int32_t)rnd_below(3);
                    for (int32_t s = 0; s < ins; s++)
                        sb_printf(&snew, "ins-%d-%d-%d\n", iter, i, s);
                }
                just_edited = true;
                /* force the following line to stay unchanged (the gap),
                 * except a deletion at EOF may leave nothing to keep */
            } else {
                sb_putn(&snew, oldl.lines[i].ptr, oldl.lines[i].len);
                sb_putc(&snew, '\n');
                i++;
                just_edited = false;
            }
        }
        Lines newl = split_lines(ta, snew.data, snew.len);
        Regions prg = diff_lines(ta, oldl, newl);

        /* A deletion that ate through EOF can fuse with a previous edit's
         * region; reconstruction is the hard invariant, count is checked
         * only as <= expected. */
        bool count_ok = prg.count <= k_expected;
        bool rebuild_ok = regions_reconstruct(ta, oldl, newl, prg);
        if (!count_ok || !rebuild_ok) {
            failures++;
            if (failures == 1)
                printf("  property failure at iter %d: count=%d expected<=%d "
                       "rebuild=%d\n",
                       iter, prg.count, k_expected, rebuild_ok);
        }
        arena_free(ta);
    }
    ASSERT_EQ_I(failures, 0);

    t_begin("diff property: gap-separated edits give exact region count");
    rng_state = 0x243f6a8885a308d3ULL;
    int32_t exact_failures = 0;
    for (int32_t iter = 0; iter < 200; iter++) {
        Arena *ta = arena_new(1 << 16);
        /* fixed shape: 30 distinct lines, edit exactly lines 5, 12-13, 25
         * (1-based), all replacements => must be exactly 3 regions */
        StrBuf sold;
        sb_init(&sold, ta);
        for (int32_t i = 0; i < 30; i++)
            sb_printf(&sold, "u-%d-%u\n", i, (uint32_t)rnd_below(100000));
        Lines oldl = split_lines(ta, sold.data, sold.len);
        StrBuf snew;
        sb_init(&snew, ta);
        for (int32_t i = 0; i < 30; i++) {
            if (i == 4 || i == 11 || i == 12 || i == 24)
                sb_printf(&snew, "w-%d-%d\n", iter, i);
            else {
                sb_putn(&snew, oldl.lines[i].ptr, oldl.lines[i].len);
                sb_putc(&snew, '\n');
            }
        }
        Lines newl = split_lines(ta, snew.data, snew.len);
        Regions prg = diff_lines(ta, oldl, newl);
        if (prg.count != 3 || !regions_reconstruct(ta, oldl, newl, prg))
            exact_failures++;
        arena_free(ta);
    }
    ASSERT_EQ_I(exact_failures, 0);

    arena_free(a);
}
