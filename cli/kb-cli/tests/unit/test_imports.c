/* What a file imports (syntax_imports), and the folder file each import
 * names (import_resolve). */

#include "test.h"

#include "../../src/imports.h"

#include <stdlib.h>
#include <string.h>

/* The imports of a small file in a path's language, as "spec" or "<spec>"
 * joined with spaces. */
static const char *imports_of(Arena *a, const char *path, const char *text) {
    SyntaxImport *v;
    size_t n;
    if (!syntax_imports(a, syntax_lang(path), text, strlen(text), &v, &n))
        return "(no parse)";
    char *out = arena_strdup(a, "");
    for (size_t i = 0; i < n; i++)
        out = arena_printf(a, "%s%s%s%s%s", out, i ? " " : "", v[i].system ? "<" : "",
                           v[i].spec, v[i].system ? ">" : "");
    return out;
}

/* The folder, as a sorted list of its files. */
static const char *const FILES[] = {
    "include/api.h", "py/pkg/__init__.py", "py/pkg/helper.py", "py/pkg/sub/deep.py",
    "src/common.h", "src/main.c",       "src/util/str.h",     "web/app.ts",
    "web/c.js",     "web/lib/f.ts",     "web/lib/index.ts",   NULL};

static bool in_folder(void *ud, const char *path) {
    (void)ud;
    for (int32_t i = 0; FILES[i]; i++)
        if (strcmp(FILES[i], path) == 0)
            return true;
    return false;
}

static const char *resolve(Arena *a, const char *from, const char *spec, bool system) {
    SyntaxImport imp = {spec, system};
    const char *to = import_resolve(a, syntax_lang(from), from, &imp, in_folder, NULL);
    return to ? to : "(none)";
}

void test_imports(void) {
    Arena *a = arena_new(1 << 16);

    t_begin("imports: C's includes, quoted and system, under #if too");
    ASSERT_EQ_S(imports_of(a, "m.c",
                           "#include \"util/str.h\"\n#include <stdio.h>\n#ifdef X\n"
                           "#include \"y.h\"\n#endif\n// #include \"no.h\"\n"),
                "util/str.h <stdio.h> y.h");

    t_begin("imports: TS and JS, every form, and nothing from a string or comment");
    ASSERT_EQ_S(imports_of(a, "a.ts",
                           "import a from './a.js';\nexport * from \"../b\";\n"
                           "import x = require('./r');\nconst c = require('./c');\n"
                           "import('./d');\nconst s = './not';\n// import e from './e'\n"),
                "./a.js ../b ./r ./c ./d");

    t_begin("imports: Python's import and from-import, relative and absolute");
    ASSERT_EQ_S(imports_of(a, "m.py",
                           "import os, pkg.mod as m\nfrom . import sib, other as o\n"
                           "from ..up import thing\nfrom pkg.sub import name\n"),
                "os pkg.mod .sib .other ..up pkg.sub");

    t_begin("imports: a language without rules lists none");
    ASSERT_EQ_S(imports_of(a, "m.go", "package m\nimport \"fmt\"\n"), "");

    t_begin("resolve: C, beside the file first, then from the root; <…> the other way");
    ASSERT_EQ_S(resolve(a, "src/main.c", "util/str.h", false), "src/util/str.h");
    ASSERT_EQ_S(resolve(a, "src/util/str.h", "../common.h", false), "src/common.h");
    ASSERT_EQ_S(resolve(a, "src/main.c", "include/api.h", true), "include/api.h");
    ASSERT_EQ_S(resolve(a, "src/main.c", "stdio.h", true), "(none)");
    ASSERT_EQ_S(resolve(a, "src/main.c", "../../outside.h", false), "(none)");

    t_begin("resolve: TS and JS, extensions, index, and the .ts behind a .js");
    ASSERT_EQ_S(resolve(a, "web/app.ts", "./lib/f", false), "web/lib/f.ts");
    ASSERT_EQ_S(resolve(a, "web/app.ts", "./lib", false), "web/lib/index.ts");
    ASSERT_EQ_S(resolve(a, "web/lib/index.ts", "./f.js", false), "web/lib/f.ts");
    ASSERT_EQ_S(resolve(a, "web/app.ts", "./c.js", false), "web/c.js");
    ASSERT_EQ_S(resolve(a, "web/app.ts", "react", false), "(none)");

    t_begin("resolve: Python, relative from the package, absolute from each folder up");
    ASSERT_EQ_S(resolve(a, "py/pkg/main.py", ".helper", false), "py/pkg/helper.py");
    ASSERT_EQ_S(resolve(a, "py/pkg/main.py", ".sub.deep", false), "py/pkg/sub/deep.py");
    ASSERT_EQ_S(resolve(a, "py/pkg/sub/deep.py", "..helper", false), "py/pkg/helper.py");
    ASSERT_EQ_S(resolve(a, "py/pkg/sub/deep.py", "..", false), "py/pkg/__init__.py");
    ASSERT_EQ_S(resolve(a, "py/pkg/main.py", "pkg.sub.deep", false), "py/pkg/sub/deep.py");
    ASSERT_EQ_S(resolve(a, "py/pkg/main.py", "os", false), "(none)");
    ASSERT_EQ_S(resolve(a, "py/pkg/main.py", "....far", false), "(none)");

    arena_free(a);
}
