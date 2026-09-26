/* Parses files with kb's grammars and prints, per file, the language, how
 * many top-level nodes the tree has, how much of the file is inside ERROR
 * nodes, whether kb would split it by the tree or fall back to line windows,
 * and how long the parse took.
 *
 *   kb-syntax-dump [--nodes] <file>...
 *
 * --nodes also lists every top-level node's type and span.
 */

#include "../../src/platform.h"
#include "../../src/syntax.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    bool nodes = false;
    Arena *a = arena_new(1 << 20);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--nodes") == 0) {
            nodes = true;
            continue;
        }
        char *text;
        size_t len;
        if (!plat_read_file(a, argv[i], &text, &len)) {
            printf("%s: unreadable\n", argv[i]);
            continue;
        }
        SyntaxLang l = syntax_lang(argv[i]);
        SyntaxOutline o;
        double t0 = now_ms();
        bool ok = syntax_outline(a, l, text, len, &o);
        double dt = now_ms() - t0;
        printf("%-10s %s %4zu nodes  %5.1f%% error  %2u missing  %-6s %6.1f ms  %s\n",
               syntax_lang_name(l), ok ? "ok " : "no ", o.n,
               len ? 100.0 * o.error_bytes / (double)len : 0.0, o.missing,
               syntax_usable(&o, len) ? "tree" : "lines", dt, argv[i]);
        for (size_t k = 0; nodes && k < o.n; k++)
            printf("    %-28s %6u-%-6u%s\n", o.v[k].type, o.v[k].start, o.v[k].end,
                   o.v[k].error ? "  ERROR" : "");
    }
    return 0;
}
