/* kb's grammars: which one a path gets, what each makes of a small file in
 * its language, and when a file falls back to line windows. */

#include "test.h"

#include "../../src/syntax.h"

#include <string.h>

static SyntaxOutline outline(Arena *a, const char *path, const char *text) {
    SyntaxOutline o;
    if (!syntax_outline(a, syntax_lang(path), text, strlen(text), &o))
        memset(&o, 0, sizeof o);
    return o;
}

/* The top-level node types, in order, joined with spaces. */
static bool types_are(const SyntaxOutline *o, const char *want) {
    char got[512] = "";
    for (size_t i = 0; i < o->n; i++) {
        if (i)
            strncat(got, " ", sizeof got - strlen(got) - 1);
        strncat(got, o->v[i].type, sizeof got - strlen(got) - 1);
    }
    if (strcmp(got, want) != 0)
        printf("    got \"%s\"\n", got);
    return strcmp(got, want) == 0;
}

static bool clean(const SyntaxOutline *o, const char *text) {
    return o->error_bytes == 0 && o->missing == 0 && syntax_usable(o, strlen(text));
}

void test_syntax(void) {
    t_begin("syntax: the grammar comes from the extension");
    ASSERT_TRUE(syntax_lang("a/b.c") == SYNTAX_C);
    ASSERT_TRUE(syntax_lang("b.h") == SYNTAX_C);
    ASSERT_TRUE(syntax_lang("x.ts") == SYNTAX_TYPESCRIPT);
    ASSERT_TRUE(syntax_lang("x.mts") == SYNTAX_TYPESCRIPT);
    ASSERT_TRUE(syntax_lang("x.tsx") == SYNTAX_TSX);
    ASSERT_TRUE(syntax_lang("x.jsx") == SYNTAX_JAVASCRIPT);
    ASSERT_TRUE(syntax_lang("x.cjs") == SYNTAX_JAVASCRIPT);
    ASSERT_TRUE(syntax_lang("x.pyi") == SYNTAX_PYTHON);
    ASSERT_TRUE(syntax_lang("x.go") == SYNTAX_GO);
    ASSERT_TRUE(syntax_lang("x.rs") == SYNTAX_RUST);
    ASSERT_TRUE(syntax_lang("boot.S") == SYNTAX_ASM);
    ASSERT_TRUE(syntax_lang("boot.s") == SYNTAX_ASM);
    ASSERT_TRUE(syntax_lang("boot.asm") == SYNTAX_ASM);
    ASSERT_TRUE(syntax_lang("boot.nasm") == SYNTAX_ASM);
    ASSERT_TRUE(syntax_lang("README.md") == SYNTAX_NONE);
    ASSERT_TRUE(syntax_lang("Makefile") == SYNTAX_NONE);
    ASSERT_TRUE(syntax_lang("dir.c/file") == SYNTAX_NONE);  /* a dot in a folder */
    ASSERT_TRUE(syntax_lang(NULL) == SYNTAX_NONE);
    ASSERT_TRUE(strcmp(syntax_lang_name(SYNTAX_TSX), "tsx") == 0);

    Arena *a = arena_new(1 << 16);
    t_begin("syntax: each grammar reads its own language cleanly");
    const char *c = "int f(void) { return 1; }\nstruct s { int x; };\n#define N 3\n";
    SyntaxOutline o = outline(a, "a.c", c);
    ASSERT_TRUE(types_are(&o, "function_definition struct_specifier preproc_def"));
    ASSERT_TRUE(clean(&o, c));
    ASSERT_EQ_I(o.v[0].start, 0);
    ASSERT_EQ_I(o.v[0].end, 25);
    const char *ts = "export function f(x: number): number { return x; }\n"
                     "interface I { a: string }\nclass C {}\n";
    o = outline(a, "a.ts", ts);
    ASSERT_TRUE(types_are(&o, "export_statement interface_declaration class_declaration"));
    ASSERT_TRUE(clean(&o, ts));
    const char *tsx = "export const V = () => <div className=\"x\">hi</div>;\n";
    o = outline(a, "a.tsx", tsx);
    ASSERT_TRUE(types_are(&o, "export_statement"));
    ASSERT_TRUE(clean(&o, tsx));
    /* The same text as plain TypeScript is not TypeScript: JSX needs .tsx. */
    o = outline(a, "a.ts", tsx);
    ASSERT_TRUE(o.error_bytes > 0 || o.missing > 0);
    const char *jsx = "function g() { return <b/>; }\nconst h = 1;\n";
    o = outline(a, "a.jsx", jsx);
    ASSERT_TRUE(types_are(&o, "function_declaration lexical_declaration"));
    ASSERT_TRUE(clean(&o, jsx));
    const char *py = "import os\n\ndef f(x):\n    return x\n\nclass K:\n    pass\n";
    o = outline(a, "a.py", py);
    ASSERT_TRUE(types_are(&o, "import_statement function_definition class_definition"));
    ASSERT_TRUE(clean(&o, py));
    const char *go = "package main\n\nimport \"fmt\"\n\nfunc main() { fmt.Println(1) }\n\n"
                     "type T struct{ A int }\n";
    o = outline(a, "a.go", go);
    ASSERT_TRUE(types_are(&o, "package_clause import_declaration function_declaration type_declaration"));
    ASSERT_TRUE(clean(&o, go));
    const char *rs = "use std::io;\n\nfn main() {}\n\nstruct S { a: i32 }\nimpl S { fn f(&self) {} }\n";
    o = outline(a, "a.rs", rs);
    ASSERT_TRUE(types_are(&o, "use_declaration function_item struct_item impl_item"));
    ASSERT_TRUE(clean(&o, rs));

    t_begin("syntax: one assembly grammar reads GNU as, NASM, ARM and RISC-V");
    const char *att = "    .globl main\nmain:\n    pushq %rbp\n    movq %rsp, %rbp\n    ret\n";
    o = outline(a, "x.s", att);
    ASSERT_TRUE(syntax_usable(&o, strlen(att)));
    bool label = false;
    for (size_t i = 0; i < o.n; i++)
        label |= strcmp(o.v[i].type, "label") == 0;
    ASSERT_TRUE(label);
    const char *nasm = "section .text\n    global _start\n_start:\n    mov rax, 60\n"
                       "    xor rdi, rdi\n    syscall\n";
    o = outline(a, "x.asm", nasm);
    ASSERT_TRUE(syntax_usable(&o, strlen(nasm)));
    const char *arm = "    .global f\nf:\n    ldrb w3, [x1], #1\n    strb w3, [x0], #1\n    ret\n";
    o = outline(a, "x.s", arm);
    ASSERT_TRUE(syntax_usable(&o, strlen(arm)));
    const char *rv = "    .globl _start\n_start:\n    li a0, 1\n    la a1, message\n    ecall\n";
    o = outline(a, "x.S", rv);
    ASSERT_TRUE(syntax_usable(&o, strlen(rv)));

    t_begin("syntax: a file that is mostly error falls back to line windows");
    const char *bad = "int f( { return ;;; }}} @@@ ### int\n";
    o = outline(a, "bad.c", bad);
    ASSERT_TRUE(o.error_bytes > 0);
    ASSERT_TRUE(!syntax_usable(&o, strlen(bad)));
    /* A few unreadable bytes in a large file do not. */
    SyntaxOutline few = {NULL, 1, 5, 0, false};
    ASSERT_TRUE(syntax_usable(&few, 1000));
    SyntaxOutline many = {NULL, 1, 150, 0, false};
    ASSERT_TRUE(!syntax_usable(&many, 1000));
    SyntaxOutline stopped = {NULL, 0, 0, 0, true};
    ASSERT_TRUE(!syntax_usable(&stopped, 1000));

    t_begin("syntax: no grammar, no outline; empty text, an empty outline");
    SyntaxOutline none;
    ASSERT_TRUE(!syntax_outline(a, SYNTAX_NONE, "x", 1, &none));
    SyntaxOutline empty;
    ASSERT_TRUE(syntax_outline(a, SYNTAX_C, "", 0, &empty));
    ASSERT_EQ_I(empty.n, 0);
    ASSERT_TRUE(syntax_usable(&empty, 0));
    arena_free(a);
}
