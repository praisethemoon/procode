#include "help.h"
#include "test.h"

#include <string.h>

/* Whether `flag` is one of the entry's flags, by either spelling. */
static bool has(const HelpCmd *c, const char *flag) {
    for (const HelpFlag *f = c->flags; f->name; f++)
        if (strcmp(f->name, flag) == 0 || (f->alias && strcmp(f->alias, flag) == 0))
            return true;
    return false;
}

/* The flag appears in the synopsis as a word of its own ("-n N", "[-F"). */
static bool in_synopsis(const HelpCmd *c, const char *flag) {
    size_t n = strlen(flag);
    for (const char *p = strstr(c->synopsis, flag); p; p = strstr(p + 1, flag)) {
        bool starts = p == c->synopsis || strchr(" [(|\n", p[-1]);
        bool ends = strchr(" ]).|\n=", p[n]) != NULL;
        if (starts && ends)
            return true;
    }
    return false;
}

static bool listed(const char *const *list, const char *flag) {
    for (; *list; list++)
        if (strcmp(*list, flag) == 0)
            return true;
    return false;
}

static bool in_group(const HelpCmd *g, const HelpCmd *s) {
    size_t n = strlen(g->name);
    return strncmp(s->name, g->name, n) == 0 && s->name[n] == ' ';
}

/* The help table (help.c): lap --help, <command> --help and the flags each
 * command accepts are all read from it. */
void test_help(void) {
    t_begin("help: every entry has a synopsis, a summary and a line per flag");
    int32_t entries = 0;
    for (const HelpCmd *c = HELP_CMDS; c->name; c++, entries++) {
        ASSERT_TRUE(c->synopsis && c->synopsis[0]);
        ASSERT_TRUE(c->summary && c->summary[0]);
        ASSERT_TRUE(strncmp(c->synopsis, c->name, strlen(c->name)) == 0);
        for (const HelpFlag *f = c->flags; f->name; f++) {
            ASSERT_TRUE(f->name[0] == '-');
            ASSERT_TRUE(f->text && f->text[0]);
        }
    }
    ASSERT_TRUE(entries >= 21);

    t_begin("help: every flag a command takes is named in its synopsis, so "
            "lap --help lists it too");
    for (const HelpCmd *c = HELP_CMDS; c->name; c++) {
        if (c->group)
            continue;
        for (const HelpFlag *f = c->flags; f->name; f++) {
            bool named = in_synopsis(c, f->name) || (f->alias && in_synopsis(c, f->alias));
            if (!named)
                fprintf(stderr, "  %s: %s is not in the synopsis\n", c->name, f->name);
            ASSERT_TRUE(named);
        }
    }

    t_begin("help: a group takes exactly the flags of its subcommands");
    for (const HelpCmd *g = HELP_CMDS; g->name; g++) {
        if (!g->group)
            continue;
        ASSERT_TRUE(g->run != NULL);
        int32_t subs = 0;
        for (const HelpCmd *s = g + 1; s->name && in_group(g, s); s++, subs++) {
            ASSERT_TRUE(s->run == NULL);
            ASSERT_TRUE(help_sub(g, s->name + strlen(g->name) + 1) == s);
            for (const HelpFlag *f = s->flags; f->name; f++)
                ASSERT_TRUE(has(g, f->name));
        }
        ASSERT_TRUE(subs >= 2);
        for (const HelpFlag *f = g->flags; f->name; f++) {
            bool used = false;
            for (const HelpCmd *s = g + 1; s->name && in_group(g, s); s++)
                used = used || has(s, f->name);
            ASSERT_TRUE(used);
        }
    }

    t_begin("help: every command outside a group runs; a subcommand only "
            "through its group");
    for (const HelpCmd *c = HELP_CMDS; c->name; c++)
        ASSERT_TRUE((c->run != NULL) == (strchr(c->name, ' ') == NULL));
    ASSERT_TRUE(help_find("review") == help_find("rr"));
    ASSERT_TRUE(help_find("nope") == NULL);
    ASSERT_TRUE(help_sub(help_find("session"), "nope") == NULL);
    ASSERT_TRUE(help_sub(help_find("commit"), "start") == NULL);

    t_begin("help: a command's flag sets carry both spellings, values apart "
            "from switches");
    FlagSets fs;
    help_flag_sets("commit", &fs);
    const char *values[] = {"-i", "--intent", "-b", "--behavior", "-F", "--edit",
                            "--lines", "--branch"};
    const char *bools[] = {"--force-message", "--dry-run", "--no-session", "--json"};
    int32_t nv = 0, nb = 0;
    while (fs.values[nv])
        nv++;
    while (fs.bools[nb])
        nb++;
    ASSERT_EQ_I(nv, 8);
    ASSERT_EQ_I(nb, 4);
    for (int32_t i = 0; i < 8; i++)
        ASSERT_TRUE(listed(fs.values, values[i]));
    for (int32_t i = 0; i < 4; i++)
        ASSERT_TRUE(listed(fs.bools, bools[i]));

    t_begin("help: --help is asked for anywhere but as a flag's value or "
            "after --");
    char *asked[] = {"notes.txt", "-i", "why", "--help"};
    char *as_value[] = {"notes.txt", "-i", "-h", "-b", "--help"};
    char *after[] = {"--", "--help"};
    ASSERT_TRUE(help_asked(4, asked, fs.values));
    ASSERT_TRUE(!help_asked(5, as_value, fs.values));
    ASSERT_TRUE(!help_asked(2, after, fs.values));

    t_begin("help: a usage error's synopsis is one line; a group's joins its "
            "subcommands'");
    ASSERT_TRUE(strchr(help_synopsis("commit"), '\n') == NULL);
    ASSERT_TRUE(strstr(help_synopsis("commit"), "[--no-session] [--branch <name>]") != NULL);
    const char *branch = help_synopsis("branch");
    ASSERT_TRUE(strstr(branch, "branch start [name] --from <folder> [--json] | branch list") != NULL);
    ASSERT_TRUE(strstr(branch, "| branch move <branch> <path> [--json]") != NULL);
}
