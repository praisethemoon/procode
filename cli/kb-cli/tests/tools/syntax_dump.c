/* Parses files with kb's grammars and prints, per file, the language, how
 * many top-level nodes the tree has, how much of the file is inside ERROR
 * nodes, whether kb would split it by the tree or fall back to line windows,
 * and how long the parse took.
 *
 *   kb-syntax-dump [--nodes] [--chunks [BYTES]] [--symbols] <file>...
 *
 * --nodes also lists every top-level node's type and span; --chunks lists the
 * chunks syntax_cuts makes at a target of BYTES (default 1600), with their
 * sizes and headings; --symbols lists the definitions syntax_symbols finds.
 */

#include "../../src/platform.h"
#include "../../src/syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    bool nodes = false;
    size_t chunks = 0;
    bool symbols = false;
    Arena *a = arena_new(1 << 20);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--nodes") == 0) {
            nodes = true;
            continue;
        }
        if (strcmp(argv[i], "--symbols") == 0) {
            symbols = true;
            continue;
        }
        if (strcmp(argv[i], "--chunks") == 0) {
            chunks = 1600;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                chunks = (size_t)atoi(argv[++i]);
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
        SyntaxSymbol *syms;
        size_t ns;
        if (symbols && syntax_symbols(a, l, text, len, &syms, &ns))
            for (size_t k = 0; k < ns; k++)
                printf("    %-10s %-28s line %-5u %s\n", syms[k].kind, syms[k].name, syms[k].line,
                       syms[k].doc ? "(doc)" : "");
        SyntaxCut *cuts;
        size_t nc;
        if (chunks && syntax_cuts(a, l, text, len, chunks, &cuts, &nc)) {
            for (size_t k = 0; k < nc; k++) {
                uint32_t end = k + 1 < nc ? cuts[k + 1].start : (uint32_t)len;
                printf("    chunk %3zu %6u-%-6u %5u bytes  %s\n", k, cuts[k].start, end,
                       end - cuts[k].start, cuts[k].heading ? cuts[k].heading : "-");
            }
        }
    }
    return 0;
}
