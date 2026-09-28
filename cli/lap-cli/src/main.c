#include "cmd.h"
#include "help.h"

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
        help_usage(stderr);
        return LAP_EXIT_ERR;
    }
    const char *cmd = argv[at];
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
        printf("lap %s\n", LAP_VERSION);
        return LAP_EXIT_OK;
    }
    int32_t argc2 = argc - at - 1;
    char **argv2 = argv + at + 1;
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 ||
        strcmp(cmd, "help") == 0) {
        /* lap help <command> [<subcommand>]: that one's help */
        const HelpCmd *c = argc2 > 0 ? help_find(argv2[0]) : NULL;
        if (argc2 > 0 && (!c || !c->run)) {
            err_out(tty_json(), "unknown_command", "unknown command \"%s\"",
                    argv2[0]);
            return LAP_EXIT_ERR;
        }
        if (argc2 > 1) {
            const HelpCmd *s = help_sub(c, argv2[1]);
            if (!s) {
                err_out(tty_json(), "unknown_command",
                        "unknown command \"%s %s\"", argv2[0], argv2[1]);
                return LAP_EXIT_ERR;
            }
            c = s;
        }
        if (c)
            help_command(stdout, c);
        else
            help_usage(stdout);
        return LAP_EXIT_OK;
    }

    const HelpCmd *c = help_find(cmd);
    if (!c || !c->run) {
        err_out(tty_json(), "unknown_command", "unknown command \"%s\"", cmd);
        if (!tty_json()) {
            fputc('\n', stderr);
            help_usage(stderr);
        }
        return LAP_EXIT_ERR;
    }
    /* --help anywhere on the line answers before anything is checked or
     * opened; a group's subcommand, when named, answers for itself */
    FlagSets fs;
    help_flag_sets(c->name, &fs);
    if (help_asked(argc2, argv2, fs.values)) {
        const HelpCmd *s = help_sub(c, positional_arg(argc2, argv2, fs.values, 0));
        help_command(stdout, s ? s : c);
        return LAP_EXIT_OK;
    }

    Arena *a = arena_new(1 << 16);
    int32_t rc = c->run(a, argc2, argv2);
    arena_free(a);
    return (int)rc;
}
