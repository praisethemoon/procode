#include "cmd.h"
#include "test.h"

/* The command-line scanners every command shares (cmd_common.c). */
void test_args(void) {
    static const char *const values[] = {"-i", "--branch", "--session", NULL};
    static const char *const bools[] = {"--json", NULL};

    t_begin("args: a long value flag takes its value apart or joined");
    char *apart[] = {"log", "--branch", "feat"};
    char *joined[] = {"log", "--branch=feat"};
    char *empty[] = {"log", "--branch="};
    ASSERT_EQ_S(flag_value(3, apart, values, "--branch"), "feat");
    ASSERT_EQ_S(flag_value(2, joined, values, "--branch"), "feat");
    ASSERT_EQ_S(flag_value(2, empty, values, "--branch"), "");
    ASSERT_TRUE(flags_known(2, joined, values, bools));

    t_begin("args: a joined value is not a positional argument, and a "
            "prefix of another flag is not that flag");
    char *mixed[] = {"--session=S1", "f.txt", "--branchy=x"};
    ASSERT_EQ_S(positional_arg(3, mixed, values, 0), "f.txt");
    ASSERT_TRUE(positional_arg(3, mixed, values, 1) == NULL);
    ASSERT_TRUE(flag_value(3, mixed, values, "--branch") == NULL);
    ASSERT_TRUE(!flags_known(3, mixed, values, bools));

    t_begin("args: short flags take their value apart only");
    char *short_eq[] = {"-i=why", "f.txt"};
    ASSERT_TRUE(flag_value(2, short_eq, values, "-i") == NULL);
    ASSERT_TRUE(!flags_known(2, short_eq, values, bools));

    t_begin("args: a value flag at the end of the line is refused");
    char *trailing[] = {"log", "--json", "--branch"};
    ASSERT_TRUE(!flags_known(3, trailing, values, bools));
    char *after_dashes[] = {"--", "--branch"};
    ASSERT_TRUE(flags_known(2, after_dashes, values, bools));

    t_begin("args: an empty value is refused as a missing one, joined or "
            "apart, long or short");
    ASSERT_TRUE(!flags_known(2, empty, values, bools)); /* --branch= */
    char *apart_empty[] = {"log", "--branch", ""};
    ASSERT_TRUE(!flags_known(3, apart_empty, values, bools));
    char *short_empty[] = {"-i", "", "f.txt"};
    ASSERT_TRUE(!flags_known(3, short_empty, values, bools));
    char *empty_after_dashes[] = {"--", ""};
    ASSERT_TRUE(flags_known(2, empty_after_dashes, values, bools));

    t_begin("args: a value that looks like a flag is still the value");
    char *msg[] = {"-i", "--json", "f.txt"};
    ASSERT_EQ_S(flag_value(3, msg, values, "-i"), "--json");
    ASSERT_TRUE(!has_flag(3, msg, values, "--json"));
    ASSERT_EQ_S(positional_arg(3, msg, values, 0), "f.txt");

    t_begin("args: a new or deleted file refuses --edit, and a deleted one "
            "--lines, rather than ignoring them; --lines on a new file picks "
            "its part");
    ASSERT_EQ_S(whole_file_pick_error("create", "2", NULL), "bad_edit_index");
    ASSERT_TRUE(whole_file_pick_error("create", NULL, "5-7") == NULL);
    ASSERT_EQ_S(whole_file_pick_error("delete", NULL, "1-3"), "bad_lines");
    ASSERT_TRUE(whole_file_pick_error("create", NULL, NULL) == NULL);
    ASSERT_TRUE(whole_file_pick_error("delete", NULL, NULL) == NULL);
    ASSERT_TRUE(whole_file_pick_error("edit", "2", NULL) == NULL);
    ASSERT_TRUE(whole_file_pick_error("edit", NULL, "5-7") == NULL);

    t_begin("args: create_part takes a new file's lines a..b, ending with a "
            "newline unless it ends the file");
    Str text[5] = {{"f1", 2}, {"", 0}, {"f2", 2}, {"", 0}, {"f3", 2}};
    Lines file = {text, 5, false}; /* no newline after f3 */
    Lines part;
    ASSERT_TRUE(create_part(file, 1, 1, &part)); /* the top */
    ASSERT_TRUE(part.lines == text && part.count == 1 && part.eof_nl);
    ASSERT_TRUE(create_part(file, 3, 3, &part)); /* the middle */
    ASSERT_TRUE(part.lines == text + 2 && part.count == 1 && part.eof_nl);
    ASSERT_TRUE(create_part(file, 4, 5, &part)); /* the end, as the file ends */
    ASSERT_TRUE(part.lines == text + 3 && part.count == 2 && !part.eof_nl);
    file.eof_nl = true;
    ASSERT_TRUE(create_part(file, 5, 5, &part) && part.eof_nl);
    ASSERT_TRUE(create_part(file, 1, 5, &part) && part.count == 5);
    ASSERT_TRUE(!create_part(file, 0, 2, &part));
    ASSERT_TRUE(!create_part(file, 3, 2, &part));
    ASSERT_TRUE(!create_part(file, 4, 6, &part));
}
