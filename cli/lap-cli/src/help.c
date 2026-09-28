#include "help.h"

#include <stdlib.h>
#include <string.h>

#define JSON_FLAG {"--json", NULL, NULL, "answer in JSON"}
#define BRANCH_READ                                                          \
    {"--branch", NULL, "<name>",                                             \
     "read that branch's history instead of this folder's\n"                 \
     "(main: a branch folder's parent)"}
#define BRANCH_WRITE                                                         \
    {"--branch", NULL, "<name>",                                             \
     "the line of history this records to; required where\n"                 \
     "branches exist (LAP_BRANCH stands in for it)"}
#define END {NULL, NULL, NULL, NULL}

static const HelpFlag F_JSON_ONLY[] = {JSON_FLAG, END};

static const HelpFlag F_COMMIT[] = {
    {"-i", "--intent", "\"intent\"", "why the edit exists"},
    {"-b", "--behavior", "\"behavior\"", "what this edit makes the code do"},
    {"-F", NULL, "<file>",
     "read both from Intent:/Behavior: sections of a file\n(- = stdin)"},
    {"--edit", NULL, "<n>",
     "pick the n-th pending edit (lap status numbers them);\n"
     "refused on a new or deleted file, committed whole"},
    {"--lines", NULL, "<a>-<b>", "pick the edit covering these lines; refused likewise"},
    {"--force-message", NULL, NULL, "skip the repetition checks (not length)"},
    {"--dry-run", NULL, NULL, "show what would be recorded; write nothing"},
    {"--no-session", NULL, NULL, "commit outside any session"},
    BRANCH_WRITE,
    JSON_FLAG,
    END};

static const HelpFlag F_AMEND[] = {
    {"-i", "--intent", "\"intent\"", "the corrected intent"},
    {"-b", "--behavior", "\"behavior\"", "the corrected behavior"},
    {"-F", NULL, "<file>",
     "read both from Intent:/Behavior: sections of a file\n(- = stdin)"},
    {"--force-message", NULL, NULL, "skip the repetition checks (not length)"},
    BRANCH_WRITE,
    JSON_FLAG,
    END};

static const HelpFlag F_LOG[] = {
    {"--session", NULL, "<S>", "only that session's commits (S<n> or <branch>/S<n>)"},
    {"--file", NULL, "<F>", "only that file's commits"},
    {"-n", NULL, "<N>", "at most N commits"},
    BRANCH_READ,
    JSON_FLAG,
    END};

static const HelpFlag F_SHOW[] = {
    {"--full-file", NULL, NULL,
     "also the whole file as of that commit, replayed from its history"},
    BRANCH_READ,
    JSON_FLAG,
    END};

static const HelpFlag F_SEARCH[] = {
    {"--file", NULL, "<F>", "commits to that file"},
    {"--line", NULL, "<N>",
     "with --file: the commit that last touched current line N (blame)"},
    {"--text", NULL, "<STR>", "commits whose changed lines contain STR"},
    {"--added", NULL, NULL, "with --text: only in added lines"},
    {"--removed", NULL, NULL, "with --text: only in removed lines"},
    {"--msg", NULL, "<STR>", "intent or behavior contains STR"},
    {"--session", NULL, "<S>", "only that session's commits"},
    {"--since", NULL, "<TS>", "from this date or time (local unless it says)"},
    {"--until", NULL, "<TS>", "up to this date or time"},
    {"--limit", NULL, "<N>", "at most N results"},
    JSON_FLAG,
    END};

static const HelpFlag F_SESSION[] = {
    {"-F", NULL, "<file>", "on start: read the purpose from a file (- = stdin)"},
    {"--meta", NULL, "key=value",
     "on start: tag the session (repeatable), e.g. --meta\n"
     "ticket=T-12; on list: keep the sessions carrying it"},
    {"--branch", NULL, "<name>",
     "on start: as for commit; on list: that branch's sessions"},
    JSON_FLAG,
    END};

static const HelpFlag F_SESSION_START[] = {
    {"-F", NULL, "<file>", "read the purpose from a file (- = stdin)"},
    {"--meta", NULL, "key=value",
     "tag the session (repeatable), e.g. --meta ticket=T-12"},
    BRANCH_WRITE,
    JSON_FLAG,
    END};

static const HelpFlag F_SESSION_LIST[] = {
    {"--meta", NULL, "key=value",
     "only the sessions carrying every given pair (repeatable)"},
    {"--branch", NULL, "<name>", "that branch's sessions"},
    JSON_FLAG,
    END};

static const HelpFlag F_VERIFY[] = {
    {"--deep", NULL, NULL,
     "also replay every file's history and compare it with the caches"},
    JSON_FLAG,
    END};

static const HelpFlag F_REBUILD[] = {
    {"--verify", NULL, NULL, "also fail when the hash chain is broken"},
    JSON_FLAG,
    END};

static const HelpFlag F_RR[] = {
    {"--no-diff", NULL, NULL, "keep the per-file summary, drop the hunks"},
    BRANCH_READ,
    JSON_FLAG,
    END};

static const HelpFlag F_BRANCH[] = {
    {"--from", NULL, "<folder>", "on start: the folder this branch starts from"},
    JSON_FLAG,
    END};

static const HelpFlag F_BRANCH_START[] = {
    {"--from", NULL, "<folder>", "the folder this branch starts from"},
    JSON_FLAG,
    END};

static const HelpFlag F_MERGE[] = {
    {"--dry-run", NULL, NULL, "say what would be adopted; write nothing"},
    {"--copy-from-folder", NULL, NULL,
     "take a git branch's history from its folder before\n"
     "git merge brought it"},
    JSON_FLAG,
    END};

const HelpCmd HELP_CMDS[] = {
    {"init", NULL, "init [--json]", "create a lap repository here",
     F_JSON_ONLY, cmd_init, false},
    {"status", NULL, "status [--json]", "pending edits per file, numbered",
     F_JSON_ONLY, cmd_status, false},
    {"commit", NULL,
     "commit <file> (-i \"intent\" -b \"behavior\" | -F <file>)\n"
     "[--edit <n> | --lines <a>-<b>] [--force-message] [--dry-run]\n"
     "[--no-session] [--branch <name>] [--json]",
     "record ONE edit of one file: why it exists, what it makes the code\n"
     "do; prints its id and short hash",
     F_COMMIT, cmd_commit, false},
    {"amend", NULL,
     "amend <commit> (-i \"intent\" -b \"behavior\" | -F <file>)\n"
     "[--force-message] [--branch <name>] [--json]",
     "correct what a commit of this folder says; its code and every\n"
     "written record stay as they are",
     F_AMEND, cmd_amend, false},
    {"log", NULL, "log [--session S] [--file F] [-n N] [--branch <name>] [--json]",
     "commits newest first: id, short hash, time, session, file, intent",
     F_LOG, cmd_log, false},
    {"show", NULL, "show <commit> [--full-file] [--branch <name>] [--json]",
     "one commit in full: its message and its hunk; <commit>: an id, a\n"
     "hash or a hash prefix",
     F_SHOW, cmd_show, false},
    {"search", NULL,
     "search [--file F [--line N]] [--text STR [--added|--removed]]\n"
     "[--msg STR] [--session S] [--since TS] [--until TS] [--limit N]\n"
     "[--json]",
     "ask the history questions; the criteria AND together",
     F_SEARCH, cmd_search, false},
    {"session", NULL,
     "session (start | end | list | current) [flags]",
     "one active session at a time; every commit belongs to it",
     F_SESSION, cmd_session, true},
    {"session start", NULL,
     "session start (\"purpose\" | -F <file|->) [--meta key=value]...\n"
     "[--branch <name>] [--json]",
     "open a session for one piece of work",
     F_SESSION_START, NULL, false},
    {"session end", NULL, "session end [--json]", "close the active session",
     F_JSON_ONLY, NULL, false},
    {"session list", NULL,
     "session list [--meta key=value]... [--branch <name>] [--json]",
     "every session with its commit count",
     F_SESSION_LIST, NULL, false},
    {"session current", NULL, "session current [--json]",
     "the active session, if any", F_JSON_ONLY, NULL, false},
    {"verify", NULL, "verify [--deep] [--json]",
     "check the log hash chain (and caches)", F_VERIFY, cmd_verify, false},
    {"rebuild", NULL, "rebuild [--verify] [--json]",
     "reconstruct every cache from the log", F_REBUILD, cmd_rebuild, false},
    {"rr", "review",
     "rr [<session> | <from> <to>] [--no-diff] [--branch <name>] [--json]",
     "review request: the trajectory and the net change of a session (the\n"
     "last one by default) or of a commit range; <from>/<to> are commits",
     F_RR, cmd_rr, false},
    {"branch", NULL, "branch (start | list | forget | move) [flags]",
     "the branches of this folder: other folders with their own line of\n"
     "history, merged back with lap merge",
     F_BRANCH, cmd_branch, true},
    {"branch start", NULL, "branch start [name] --from <folder> [--json]",
     "make this folder a branch of another: its own line of history from\n"
     "that folder's head",
     F_BRANCH_START, NULL, false},
    {"branch list", NULL, "branch list [--json]",
     "the branches started from this folder: active, merged, partly\n"
     "merged, missing",
     F_JSON_ONLY, NULL, false},
    {"branch forget", NULL, "branch forget <branch> [--json]",
     "drop a branch from this folder's list", F_JSON_ONLY, NULL, false},
    {"branch move", NULL, "branch move <branch> <path> [--json]",
     "record that a branch's folder now lives at <path>", F_JSON_ONLY, NULL,
     false},
    {"merge", NULL,
     "merge <branch> [--dry-run] [--copy-from-folder] [--json]",
     "after git merged a branch's code here: adopt its commits into this\n"
     "history; what cannot be placed is left to commit by hand",
     F_MERGE, cmd_merge, false},
    {NULL, NULL, NULL, NULL, NULL, NULL, false},
};

static const char *FOOTER =
    "sessions: S<n> is this folder's; <branch>/S<n> is a branch's, taken\n"
    "  wherever a session is (rr, log --session, search --session) and\n"
    "  printed for a branch's sessions\n"
    "--branch <name> on log, show, rr and session list reads that branch's\n"
    "  history instead of this folder's (main: a branch folder's parent)\n"
    "long flags take their value apart or joined: --branch feat, --branch=feat\n"
    "\n"
    "global:\n"
    "  --color=auto|always|never  colour output (auto: only at a terminal;\n"
    "                             --no-color and NO_COLOR also turn it off)\n"
    "\n"
    "rules:\n"
    "  - a commit is ONE contiguous run of changed lines in ONE file;\n"
    "    edits separated by a NON-BLANK unchanged line must be committed\n"
    "    separately (blank lines are not anchors and never split an edit)\n"
    "  - commits require an active session unless --no-session is given\n"
    "  - every commit states its intent (why) and its behavior (what this\n"
    "    edit does); edits serving one goal share an intent, never a\n"
    "    behavior; cite another commit by its hash, e.g. #fa9cebd\n";

const HelpCmd *help_find(const char *name) {
    for (const HelpCmd *c = HELP_CMDS; c->name; c++)
        if (strcmp(c->name, name) == 0 || (c->alias && strcmp(c->alias, name) == 0))
            return c;
    return NULL;
}

const HelpCmd *help_sub(const HelpCmd *group, const char *word) {
    if (!group || !group->group || !word)
        return NULL;
    size_t n = strlen(group->name);
    for (const HelpCmd *c = group + 1; c->name; c++) {
        if (strncmp(c->name, group->name, n) != 0 || c->name[n] != ' ')
            break; /* a group's subcommands follow it */
        if (strcmp(c->name + n + 1, word) == 0)
            return c;
    }
    return NULL;
}

void help_flag_sets(const char *name, FlagSets *out) {
    const HelpCmd *c = help_find(name);
    if (!c) {
        fprintf(stderr, "lap: internal error: no help entry for %s\n", name);
        abort();
    }
    int32_t nv = 0, nb = 0;
    for (const HelpFlag *f = c->flags; f->name; f++) {
        const char **list = f->arg ? out->values : out->bools;
        int32_t *n = f->arg ? &nv : &nb;
        if (*n + 3 > HELP_MAX_FLAGS)
            abort();
        list[(*n)++] = f->name;
        if (f->alias)
            list[(*n)++] = f->alias;
    }
    out->values[nv] = NULL;
    out->bools[nb] = NULL;
}

/* A group's synopsis is its subcommands', joined with " | ". */
const char *help_synopsis(const char *name) {
    static char buf[1024];
    const HelpCmd *c = help_find(name);
    if (!c)
        return name;
    size_t k = 0, n = strlen(c->name);
    const HelpCmd *s = c->group ? c + 1 : c;
    for (; s->name; s++) {
        if (c->group && (strncmp(s->name, c->name, n) != 0 || s->name[n] != ' '))
            break;
        if (s != c && s != c + 1)
            for (const char *p = " | "; *p && k + 1 < sizeof buf; p++)
                buf[k++] = *p;
        for (const char *p = s->synopsis; *p && k + 1 < sizeof buf; p++)
            buf[k++] = *p == '\n' ? ' ' : *p;
        if (!c->group)
            break;
    }
    buf[k] = '\0';
    return buf;
}

bool help_asked(int32_t argc, char **argv, const char *const *value_flags) {
    return has_flag(argc, argv, value_flags, "--help") ||
           has_flag(argc, argv, value_flags, "-h");
}

/* text, each line after a '\n' starting with `indent` */
static void put_lines(FILE *f, const char *indent, const char *text) {
    fputs(indent, f);
    for (const char *p = text; *p; p++) {
        fputc(*p, f);
        if (*p == '\n' && p[1])
            fputs(indent, f);
    }
    fputc('\n', f);
}

static void put_flags(FILE *f, const HelpFlag *flags) {
    for (const HelpFlag *x = flags; x->name; x++) {
        char left[96];
        snprintf(left, sizeof left, "%s%s%s%s%s", x->name, x->alias ? ", " : "",
                 x->alias ? x->alias : "", x->arg ? " " : "",
                 x->arg ? x->arg : "");
        const int32_t col = 26;
        fprintf(f, "  %s", left);
        int32_t used = 2 + (int32_t)strlen(left);
        if (used + 2 > col) {
            fputc('\n', f);
            used = 0;
        }
        fprintf(f, "%*s", col - used, "");
        for (const char *p = x->text; *p; p++) {
            fputc(*p, f);
            if (*p == '\n')
                fprintf(f, "%*s", col, "");
        }
        fputc('\n', f);
    }
}

static void put_synopsis(FILE *f, const char *first, const char *more,
                         const HelpCmd *c) {
    fputs(first, f);
    for (const char *p = c->synopsis; *p; p++) {
        fputc(*p, f);
        if (*p == '\n')
            fputs(more, f);
    }
    fputc('\n', f);
}

void help_usage(FILE *f) {
    fputs("lap " LAP_VERSION " - fine-grained edit recorder for AI agents\n"
          "\n"
          "usage: lap <command> [args]\n"
          "       lap <command> --help   (or -h, or lap help <command>): that\n"
          "                              command's flags, one line each\n"
          "\n"
          "commands:\n",
          f);
    for (const HelpCmd *c = HELP_CMDS; c->name; c++) {
        if (c->group)
            continue; /* its subcommands follow, each on its own */
        put_synopsis(f, "  ", "      ", c);
        put_lines(f, "        ", c->summary);
    }
    fputc('\n', f);
    fputs(FOOTER, f);
}

void help_command(FILE *f, const HelpCmd *c) {
    put_synopsis(f, "usage: lap ", "           ", c);
    if (c->alias)
        fprintf(f, "       (also: lap %s)\n", c->alias);
    fputc('\n', f);
    put_lines(f, "", c->summary);
    if (c->group) {
        fputs("\nsubcommands (lap ", f);
        fputs(c->name, f);
        fputs(" <subcommand> --help for one):\n", f);
        size_t n = strlen(c->name);
        for (const HelpCmd *s = c + 1;
             s->name && strncmp(s->name, c->name, n) == 0 && s->name[n] == ' ';
             s++) {
            put_synopsis(f, "  ", "      ", s);
            put_lines(f, "        ", s->summary);
        }
    }
    fputs("\nflags:\n", f);
    put_flags(f, c->flags);
}
