#include "cmd.h"
#include "help.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        help_usage(stderr);
        return KB_EXIT_ERR;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
        printf("kb %s\n", KB_VERSION);
        return KB_EXIT_OK;
    }
    int32_t argc2 = argc - 2;
    char **argv2 = argv + 2;
    bool json = has_flag(argc2, argv2, NULL, "--json");
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 ||
        strcmp(cmd, "help") == 0) {
        /* kb help <command> [<subcommand>]: that one's help */
        const HelpCmd *c = argc2 > 0 ? help_find(argv2[0]) : NULL;
        if (argc2 > 0 && (!c || !c->run)) {
            err_out(json, "unknown_command", "unknown command \"%s\"", argv2[0]);
            return KB_EXIT_ERR;
        }
        if (argc2 > 1 && argv2[1][0] != '-') {
            const HelpCmd *s = help_sub(c, argv2[1]);
            if (!s) {
                err_out(json, "unknown_command", "unknown command \"%s %s\"",
                        argv2[0], argv2[1]);
                return KB_EXIT_ERR;
            }
            c = s;
        }
        if (c)
            help_command(stdout, c);
        else
            help_usage(stdout);
        return KB_EXIT_OK;
    }

    const HelpCmd *c = help_find(cmd);
    if (!c || !c->run) {
        err_out(json, "unknown_command", "unknown command \"%s\"", cmd);
        if (!json) {
            fputc('\n', stderr);
            help_usage(stderr);
        }
        return KB_EXIT_ERR;
    }
    /* --help anywhere on the line answers before anything is checked or
     * opened; a group's subcommand, when named, answers for itself */
    const char *const *values = help_values(c->name);
    if (help_asked(argc2, argv2, values)) {
        const HelpCmd *s = help_sub(c, positional_arg(argc2, argv2, values, 0));
        help_command(stdout, s ? s : c);
        return KB_EXIT_OK;
    }

    help_running = c;
    Arena *a = arena_new(1 << 16);
    int32_t rc = c->run(a, argc2, argv2);
    arena_free(a);
    return (int)rc;
}
