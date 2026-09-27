#include "cmd.h"

int32_t cmd_init(Arena *a, int32_t argc, char **argv) {
    static const char *const bool_flags[] = {"--json", NULL};
    bool json = has_flag(argc, argv, NULL, "--json");
    if (!flags_known(argc, argv, NULL, bool_flags))
        return LAP_EXIT_ERR;
    char cwd[LAP_PATH_MAX];
    if (!plat_getcwd(cwd, sizeof cwd)) {
        err_out(json, "internal", "cannot get working directory");
        return LAP_EXIT_FATAL;
    }
    for (char *p = cwd; *p; p++)
        if (*p == '\\')
            *p = '/';
    char err[512];
    if (!repo_init(a, cwd, err, sizeof err)) {
        err_out(json, "init_failed", "%s", err);
        return LAP_EXIT_ERR;
    }
    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_puts(&sb, "{\"ok\":true,\"root\":");
        json_escape_c(&sb, cwd);
        sb_puts(&sb, "}");
        puts(sb_finish(&sb));
    } else {
        printf("initialized empty lap repository in %s/%s\n", cwd, LAP_DIR);
        printf("next: lap session start \"<what you are about to do>\"\n");
    }
    return LAP_EXIT_OK;
}
