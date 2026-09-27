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

    t_begin("args: a value that looks like a flag is still the value");
    char *msg[] = {"-i", "--json", "f.txt"};
    ASSERT_EQ_S(flag_value(3, msg, values, "-i"), "--json");
    ASSERT_TRUE(!has_flag(3, msg, values, "--json"));
    ASSERT_EQ_S(positional_arg(3, msg, values, 0), "f.txt");
}
