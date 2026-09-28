#include "help.h"
#include "test.h"

#include <string.h>

static bool has(const HelpCmd *c, const char *flag) {
    for (const HelpFlag *f = c->flags; f->name; f++)
        if (strcmp(f->name, flag) == 0 || (f->alias && strcmp(f->alias, flag) == 0))
            return true;
    return false;
}

/* The flag appears in the synopsis as a word of its own ("--k 10", "[--rerank"). */
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

static bool in_group(const HelpCmd *g, const HelpCmd *s) {
    size_t n = strlen(g->name);
    return strncmp(s->name, g->name, n) == 0 && s->name[n] == ' ';
}

static bool listed(const char *const *list, const char *flag) {
    for (; *list; list++)
        if (strcmp(*list, flag) == 0)
            return true;
    return false;
}

/* The help table (help.c): kb --help, <command> --help, the synopsis under a
 * usage error and the flags each command accepts are all read from it. */
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
    ASSERT_TRUE(entries >= 23);

    t_begin("help: every flag is named in a synopsis kb --help prints: its "
            "own, or for a group, one of its subcommands'");
    for (const HelpCmd *c = HELP_CMDS; c->name; c++) {
        for (const HelpFlag *f = c->flags; f->name; f++) {
            bool named = in_synopsis(c, f->name) || (f->alias && in_synopsis(c, f->alias));
            for (const HelpCmd *s = c + 1; !named && c->group && s->name && in_group(c, s); s++)
                named = in_synopsis(s, f->name);
            if (!named)
                fprintf(stderr, "  %s: %s is not in a synopsis\n", c->name, f->name);
            ASSERT_TRUE(named);
        }
    }

    t_begin("help: a group accepts every flag of its subcommands, since it "
            "parses their lines");
    int32_t groups = 0;
    for (const HelpCmd *g = HELP_CMDS; g->name; g++) {
        if (!g->group)
            continue;
        groups++;
        int32_t subs = 0;
        for (const HelpCmd *s = g + 1; s->name && in_group(g, s); s++, subs++) {
            ASSERT_TRUE(s->run == NULL);
            ASSERT_TRUE(help_sub(g, s->name + strlen(g->name) + 1) == s);
            for (const HelpFlag *f = s->flags; f->name; f++)
                ASSERT_TRUE(has(g, f->name));
        }
        ASSERT_TRUE(subs >= 1);
    }
    ASSERT_EQ_I(groups, 3);

    t_begin("help: every command outside a group runs; a subcommand only "
            "through its group");
    for (const HelpCmd *c = HELP_CMDS; c->name; c++)
        ASSERT_TRUE((c->run != NULL) == (strchr(c->name, ' ') == NULL));
    ASSERT_TRUE(help_find("nope") == NULL);
    ASSERT_TRUE(help_sub(help_find("links"), "add") == help_find("links add"));
    ASSERT_TRUE(help_sub(help_find("search"), "add") == NULL);

    t_begin("help: search's flags are all there, with both spellings, "
            "values apart from switches");
    const char *const *v = help_values("search");
    const char *const *b = help_bools("search");
    const char *values[] = {"--collection", "--mode", "--k", "--expand", "--source",
                            "--mime", "--since", "--min-score", "--minScore",
                            "--older-than", "--olderThan", "--meta", "--fusion",
                            "--rerank-depth", "--rerank-tokens"};
    int32_t nv = 0, nb = 0;
    while (v[nv])
        nv++;
    while (b[nb])
        nb++;
    ASSERT_EQ_I(nv, 15);
    ASSERT_EQ_I(nb, 2);
    for (int32_t i = 0; i < 15; i++)
        ASSERT_TRUE(listed(v, values[i]));
    ASSERT_TRUE(listed(b, "--rerank") && listed(b, "--json"));
    ASSERT_TRUE(help_values("search") == v); /* built once, kept */

    t_begin("help: --help is asked for anywhere but as a flag's value or "
            "after --");
    char *asked[] = {"query", "--k", "5", "--help"};
    char *as_value[] = {"--mime", "-h", "query"};
    char *after[] = {"--", "-h"};
    ASSERT_TRUE(help_asked(4, asked, v));
    ASSERT_TRUE(!help_asked(3, as_value, v));
    ASSERT_TRUE(!help_asked(2, after, v));
}
