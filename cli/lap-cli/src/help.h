/* lap's commands, their flags and their help: one table. lap --help, lap
 * <command> --help, each command's usage error and the flags each command
 * accepts are all read from it, so none of them can fall behind another. */
#ifndef LAP_HELP_H
#define LAP_HELP_H

#include "cmd.h"

#include <stdio.h>

/* One flag: its name, a second spelling (the long form of a short flag),
 * the value it takes (NULL for a switch) and one line on what it does. */
typedef struct {
    const char *name;
    const char *alias;
    const char *arg;
    const char *text;
} HelpFlag;

typedef int32_t (*CmdFn)(Arena *a, int32_t argc, char **argv);

/* One command ("commit") or subcommand ("session start"). A command with
 * subcommands ("session", "branch") is a group: it runs them, and its
 * flags are every flag they take. The synopsis follows "lap " and names
 * every flag; '\n' in it breaks the line. */
typedef struct {
    const char *name;
    const char *alias; /* another name that runs it ("review"), or NULL */
    const char *synopsis;
    const char *summary;
    const HelpFlag *flags; /* ends with a {NULL} entry */
    CmdFn run;             /* NULL for a subcommand: its group runs it */
    bool group;
} HelpCmd;

/* Every entry, a group before its subcommands; ends with a {NULL} entry. */
extern const HelpCmd HELP_CMDS[];

#define HELP_MAX_FLAGS 32

/* A command's flags as the scanners in cmd_common.c take them: NULL-ended
 * lists of value flags and of switches, each spelling listed. */
typedef struct {
    const char *values[HELP_MAX_FLAGS];
    const char *bools[HELP_MAX_FLAGS];
} FlagSets;

/* By name or alias; a subcommand by its full name ("session start"). */
const HelpCmd *help_find(const char *name);
/* A group's subcommand by its word, or NULL. */
const HelpCmd *help_sub(const HelpCmd *group, const char *word);
/* The flags `name` accepts; an unknown name aborts (a bug, not input). */
void help_flag_sets(const char *name, FlagSets *out);
/* The synopsis, joined onto one line, for a usage error. */
const char *help_synopsis(const char *name);
/* --help or -h among the arguments: before any "--" and not as a flag's
 * value. */
bool help_asked(int32_t argc, char **argv, const char *const *value_flags);

void help_usage(FILE *f);
void help_command(FILE *f, const HelpCmd *c);

#endif
