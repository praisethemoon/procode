#include "cmd.h"
#include "test.h"

#include <string.h>

/* The lines under a commit in a human listing (print_commit_human): lap log
 * shows the intent and the behavior, labelled; either alone on request;
 * search keeps the intent alone. Only the first line of each text. */
void test_log_text(void) {
    Arena *a = arena_new(1 << 14);
    Rec rec;
    memset(&rec, 0, sizeof rec);
    rec.type = REC_COMMIT;
    rec.id = "L7";
    memset(rec.hash, '0', 64);
    memcpy(rec.hash, "795ad3e", 7);
    rec.ts = "2026-09-28T11:19:44Z";
    rec.op = "edit";
    rec.file = "g.txt";
    rec.old_start = rec.new_start = 2;
    rec.new_lines = 1;
    rec.intent = "g edit for the test\nsecond line of the intent";
    rec.behavior = "the test file changes: g edit\nmore";

    const char *under[3];
    CommitText modes[3] = {SHOW_BOTH, SHOW_INTENT, SHOW_BEHAVIOR};
    for (int32_t m = 0; m < 3; m++) {
        StrBuf sb;
        sb_init(&sb, a);
        print_commit_human(&sb, &rec, true, NULL, modes[m]);
        const char *out = sb_finish(&sb);
        under[m] = strchr(out, '\n') + 1; /* past the commit line */
    }

    t_begin("log text: both texts by default, labelled, first lines only");
    ASSERT_EQ_S(under[0],
                "       intent:   g edit for the test\n"
                "       behavior: the test file changes: g edit\n");

    t_begin("log text: the intent alone looks as it always did");
    ASSERT_EQ_S(under[1], "       g edit for the test\n");

    t_begin("log text: the behavior alone, unlabelled");
    ASSERT_EQ_S(under[2], "       the test file changes: g edit\n");

    t_begin("log text: an amended commit's marker follows the first line shown");
    rec.amended = 2;
    StrBuf sb;
    sb_init(&sb, a);
    print_commit_human(&sb, &rec, true, NULL, SHOW_BOTH);
    const char *out = strchr(sb_finish(&sb), '\n') + 1;
    ASSERT_EQ_S(out,
                "       intent:   g edit for the test (amended 2 times)\n"
                "       behavior: the test file changes: g edit\n");
    sb_init(&sb, a);
    print_commit_human(&sb, &rec, true, NULL, SHOW_BEHAVIOR);
    out = strchr(sb_finish(&sb), '\n') + 1;
    ASSERT_EQ_S(out, "       the test file changes: g edit (amended 2 times)\n");
    arena_free(a);
}
