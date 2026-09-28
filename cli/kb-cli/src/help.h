/* kb's commands, their flags and their help: one table. kb --help, kb
 * <command> --help, the synopsis under a usage error and the flags each
 * command accepts are all read from it, so none of them can fall behind
 * another. */
#ifndef KB_HELP_H
#define KB_HELP_H

#include "cmd.h"

#include <stdio.h>

/* One flag: its name, a second spelling kb also accepts (NULL if none), the
 * value it takes (NULL for a switch) and one line on what it does. */
typedef struct {
    const char *name;
    const char *alias;
    const char *arg;
    const char *text;
} HelpFlag;

typedef int32_t (*CmdFn)(Arena *a, int32_t argc, char **argv);

/* One command ("search") or subcommand ("collections rename"). A group
 * ("collections", "sources", "links") runs on its own too, and runs its
 * subcommands; its flags are its own and theirs. The synopsis follows "kb "
 * and names every flag; '\n' in it breaks the line. `section` starts a new
 * heading in kb --help. */
typedef struct {
    const char *name;
    const char *synopsis;
    const char *summary;
    const HelpFlag *flags; /* ends with a {NULL} entry */
    CmdFn run;             /* NULL for a subcommand: its group runs it */
    bool group;
    const char *section;
} HelpCmd;

/* Every entry, a group before its subcommands; ends with a {NULL} entry. */
extern const HelpCmd HELP_CMDS[];

/* By name; a subcommand by its full name ("collections rename"). */
const HelpCmd *help_find(const char *name);
/* A group's subcommand by its word, or NULL. */
const HelpCmd *help_sub(const HelpCmd *group, const char *word);
/* The flags `name` accepts, both spellings, as the scanners in
 * cmd_common.c take them: NULL-ended lists of value flags and of switches.
 * An unknown name aborts (a bug, not input). */
const char *const *help_values(const char *name);
const char *const *help_bools(const char *name);
/* --help or -h among the arguments: before any "--" and not as a flag's
 * value. */
bool help_asked(int32_t argc, char **argv, const char *const *value_flags);

/* The command main is running, whose synopsis follows a usage error; NULL
 * before one is chosen. */
extern const HelpCmd *help_running;

void help_usage(FILE *f);
void help_command(FILE *f, const HelpCmd *c);
/* The synopsis on one line, "kb " included; a group's with its subcommands'
 * after " | ". */
void help_synopsis(FILE *f, const HelpCmd *c);

#endif
