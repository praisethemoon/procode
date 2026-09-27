#include "cmd.h"

static const char *USAGE =
    "lap " LAP_VERSION " - fine-grained edit recorder for AI agents\n"
    "\n"
    "usage: lap <command> [args]\n"
    "\n"
    "commands:\n"
    "  init                       create a lap repository here\n"
    "  status [--json]            pending edits per file, numbered\n"
    "  commit <file> -i \"intent\" -b \"behavior\"   record ONE edit of one\n"
    "                             file: why it exists, what it makes the\n"
    "                             code do; prints its id and short hash\n"
    "         -F <file>           read both from Intent:/Behavior: sections\n"
    "                             of a file (- = stdin)\n"
    "         [--edit <n> | --lines <a>-<b>]   pick one of several edits\n"
    "         [--force-message]   skip the repetition checks (not length)\n"
    "         [--dry-run]         show what would be recorded; write nothing\n"
    "         [--no-session]      commit outside any session\n"
    "         [--branch <name>]   the line of history this records to;\n"
    "                             required where branches exist\n"
    "                             (LAP_BRANCH stands in for it)\n"
    "  log [--session S] [--file F] [-n N] [--json]\n"
    "  show <commit> [--full-file] [--json]   id, hash or hash prefix\n"
    "  search [--file F [--line N]] [--text STR [--added|--removed]]\n"
    "         [--msg STR] [--session S] [--since TS] [--until TS]\n"
    "         [--limit N] [--json]\n"
    "  session [start \"purpose\" | end | list | current] [--json]\n"
    "          [-F <file>]        on start: read the purpose from a file\n"
    "          [--meta key=value]...  on start: tag the session, e.g.\n"
    "                             --meta ticket=T-12; on list: filter by it\n"
    "          [--branch <name>]  on start: as for commit\n"
    "  verify [--deep] [--json]   check the log hash chain (and caches)\n"
    "  rebuild [--verify]         reconstruct every cache from the log\n"
    "  rr [<session> | <from> <to>]   review request: trajectory + net\n"
    "     [--no-diff] [--json]        change; <from>/<to> are commits\n"
    "  branch start [name] --from <folder>   make this folder a branch of\n"
    "                             another: its own line of history from\n"
    "                             that folder's head\n"
    "  merge <branch> [--dry-run] [--json]   after git merged a branch's\n"
    "                             code here: adopt its commits into this\n"
    "                             history; what cannot be placed is left\n"
    "                             to commit by hand\n"
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

/* Flags that belong to lap rather than to a command, and so may appear on
 * either side of it. tty_init has already read them; main only has to see
 * past them to find the command. */
static bool is_global_flag(const char *arg) {
    return strcmp(arg, "--no-color") == 0 ||
           strncmp(arg, "--color=", sizeof "--color=" - 1) == 0;
}

int main(int argc, char **argv) {
    if (!tty_init(argc, argv)) {
        err_out(tty_json(), "bad_color",
                "--color expects auto, always or never");
        return LAP_EXIT_ERR;
    }
    int32_t at = 1;
    while (at < argc && is_global_flag(argv[at]))
        at++;
    if (at >= argc) {
        fputs(USAGE, stderr);
        return LAP_EXIT_ERR;
    }
    const char *cmd = argv[at];
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
        printf("lap %s\n", LAP_VERSION);
        return LAP_EXIT_OK;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 ||
        strcmp(cmd, "help") == 0) {
        fputs(USAGE, stdout);
        return LAP_EXIT_OK;
    }

    Arena *a = arena_new(1 << 16);
    int32_t argc2 = argc - at - 1;
    char **argv2 = argv + at + 1;
    int32_t rc;
    if (strcmp(cmd, "init") == 0)
        rc = cmd_init(a, argc2, argv2);
    else if (strcmp(cmd, "status") == 0)
        rc = cmd_status(a, argc2, argv2);
    else if (strcmp(cmd, "commit") == 0)
        rc = cmd_commit(a, argc2, argv2);
    else if (strcmp(cmd, "log") == 0)
        rc = cmd_log(a, argc2, argv2);
    else if (strcmp(cmd, "show") == 0)
        rc = cmd_show(a, argc2, argv2);
    else if (strcmp(cmd, "search") == 0)
        rc = cmd_search(a, argc2, argv2);
    else if (strcmp(cmd, "session") == 0)
        rc = cmd_session(a, argc2, argv2);
    else if (strcmp(cmd, "verify") == 0)
        rc = cmd_verify(a, argc2, argv2);
    else if (strcmp(cmd, "rebuild") == 0)
        rc = cmd_rebuild(a, argc2, argv2);
    else if (strcmp(cmd, "rr") == 0 || strcmp(cmd, "review") == 0)
        rc = cmd_rr(a, argc2, argv2);
    else if (strcmp(cmd, "branch") == 0)
        rc = cmd_branch(a, argc2, argv2);
    else if (strcmp(cmd, "merge") == 0)
        rc = cmd_merge(a, argc2, argv2);
    else {
        err_out(tty_json(), "unknown_command", "unknown command \"%s\"", cmd);
        if (!tty_json()) {
            fputc('\n', stderr);
            fputs(USAGE, stderr);
        }
        rc = LAP_EXIT_ERR;
    }
    arena_free(a);
    return (int)rc;
}
