/* kb's grammars: which one a path gets, what each makes of a small file in
 * its language, and when a file falls back to line windows. */

#include "test.h"

#include "../../src/platform.h"
#include "../../src/syntax.h"

#include <stdlib.h>
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


/* Cuts one fixture (a snapshot of a real file) and checks what every cut must
 * be: the chunks tile the file from byte 0, each is within the budget (or
 * barely past it, by the whitespace after its last node), and every chunk but
 * possibly the first has a heading. Returns the headings, joined by "\n". */
static const char *cut_file(Arena *a, const char *name, size_t target) {
    char *path = arena_printf(a, "tests/fixtures/syntax/%s", name);
    char *text;
    size_t len;
    if (!plat_read_file(a, path, &text, &len)) {
        ASSERT_TRUE(!"fixture unreadable");
        return "";
    }
    SyntaxCut *cuts;
    size_t n;
    ASSERT_TRUE(syntax_cuts(a, syntax_lang(name), text, len, target, &cuts, &n));
    ASSERT_TRUE(n > 1);
    if (n == 0)
        return "";
    ASSERT_EQ_I(cuts[0].start, 0);
    char *heads = arena_strdup(a, "");
    for (size_t i = 0; i < n; i++) {
        uint32_t end = i + 1 < n ? cuts[i + 1].start : (uint32_t)len;
        ASSERT_TRUE(end > cuts[i].start);
        ASSERT_TRUE(end - cuts[i].start <= target + target / 10);
        ASSERT_TRUE(i == 0 || cuts[i].heading != NULL);
        heads = arena_printf(a, "%s%s\n", heads, cuts[i].heading ? cuts[i].heading : "-");
    }
    return heads;
}

static bool has_line(const char *heads, const char *want) {
    char *needle = (char *)malloc(strlen(want) + 3);
    sprintf(needle, "\n%s\n", want);
    char *hay = (char *)malloc(strlen(heads) + 2);
    sprintf(hay, "\n%s", heads);
    bool ok = strstr(hay, needle) != NULL;
    if (!ok)
        printf("    no heading \"%s\" in:\n%s", want, heads);
    free(needle);
    free(hay);
    return ok;
}

static void test_syntax_cuts(void) {
    Arena *a = arena_new(1 << 20);
    t_begin("syntax cuts: C, a definition whole when it fits, along its tree when not");
    const char *h = cut_file(a, "gitignore.c", 1600);
    ASSERT_TRUE(has_line(h, "void gitignore_add(GitIgnore *g, const char *base, const char *text, size_t len) {"));
    ASSERT_TRUE(has_line(h, "static bool wm(const char *p0, const char *p, const char *s) { > if (*p == '[') {"));

    t_begin("syntax cuts: TypeScript methods under their class");
    h = cut_file(a, "client.ts", 1600);
    ASSERT_TRUE(has_line(h, "export class Kb { > async addDir(dir: string, options: AddDirOptions): Promise<KbDirAdded> {"));
    ASSERT_TRUE(has_line(h, "export interface KbSearchResult {"));

    t_begin("syntax cuts: TSX components");
    h = cut_file(a, "Body.tsx", 1600);
    ASSERT_TRUE(has_line(h, "export function Body(props: { text: string; mime: string }): JSX.Element {"));
    ASSERT_TRUE(has_line(h, "function HtmlPart(props: { node: HtmlNode }): JSX.Element {"));

    t_begin("syntax cuts: a JavaScript script");
    h = cut_file(a, "playground.mjs", 1600);
    ASSERT_TRUE(has_line(h, "const write = (file, text) => {"));

    t_begin("syntax cuts: Python, a long function split at its statements");
    h = cut_file(a, "convert.py", 1600);
    ASSERT_TRUE(has_line(h, "def vocabulary(tok):"));
    ASSERT_TRUE(has_line(h, "def main(): > ap = argparse.ArgumentParser()"));

    t_begin("syntax cuts: Go methods");
    h = cut_file(a, "ring.go", 600);
    ASSERT_TRUE(has_line(h, "func (r *Ring) Read() ([]byte, error) {"));

    t_begin("syntax cuts: Rust, attributes with their item, functions under their impl");
    h = cut_file(a, "lexer.rs", 600);
    ASSERT_TRUE(has_line(h, "pub enum Token {"));
    ASSERT_TRUE(has_line(h, "impl<'a> Lexer<'a> { > fn ident(&mut self, first: char) -> Token {"));

    t_begin("syntax cuts: assembly, one chunk per function, named by its label");
    h = cut_file(a, "strings.s", 600);
    ASSERT_TRUE(has_line(h, "kb_memchr:"));
    ASSERT_TRUE(has_line(h, "kb_strlen:"));

    t_begin("syntax cuts: no usable tree, no cuts (the caller windows the file)");
    SyntaxCut *cuts;
    size_t n;
    const char *bad = "int f( { return ;;; }}} @@@ ### int\n";
    ASSERT_TRUE(!syntax_cuts(a, SYNTAX_C, bad, strlen(bad), 1600, &cuts, &n));
    ASSERT_TRUE(!syntax_cuts(a, SYNTAX_NONE, "x", 1, 1600, &cuts, &n));
    arena_free(a);
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

    test_syntax_cuts();
}
