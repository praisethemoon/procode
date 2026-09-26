/* Source code as its grammar sees it, through tree-sitter (vendored under
 * third_party/, compiled into kb; nothing is loaded at runtime).
 *
 * WHICH GRAMMAR comes from the file's extension, and only from it: a file
 * named `x.ts` is TypeScript whatever it contains, as it is to its compiler.
 *
 *   C           .c .h
 *   TypeScript  .ts .mts .cts        TSX  .tsx
 *   JavaScript  .js .mjs .cjs .jsx   (the JavaScript grammar reads JSX)
 *   Python      .py .pyi
 *   Go          .go
 *   Rust        .rs
 *   Assembly    .s .S .asm .nasm     one grammar for many dialects: GNU as
 *                                    (AT&T and Intel), NASM-style, ARM,
 *                                    RISC-V, MIPS and others
 *
 * A PARSE ERROR IS NOT A FAILURE of the file. tree-sitter always returns a
 * tree, with ERROR nodes around what it could not read; the outline reports
 * how many bytes those cover, and syntax_usable() says whether the tree is
 * good enough to split the file by. When it is not — too much of it is error,
 * or the parse ran out of time — the caller splits the file into line windows
 * as it would any other text.
 */
#ifndef KB_SYNTAX_H
#define KB_SYNTAX_H

#include "arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SYNTAX_NONE,
    SYNTAX_C,
    SYNTAX_TYPESCRIPT,
    SYNTAX_TSX,
    SYNTAX_JAVASCRIPT,
    SYNTAX_PYTHON,
    SYNTAX_GO,
    SYNTAX_RUST,
    SYNTAX_ASM,
    SYNTAX_COUNT
} SyntaxLang;

/* The grammar for a path, by its extension; SYNTAX_NONE for any other. */
SyntaxLang syntax_lang(const char *path);
/* "c", "typescript", "tsx", "javascript", "python", "go", "rust", "asm";
 * "none" for SYNTAX_NONE. */
const char *syntax_lang_name(SyntaxLang l);

/* One node directly under the root: a function, a type, an import, a
 * comment, a label — whatever the grammar says the file is made of. */
typedef struct {
    const char *type; /* the grammar's node type, e.g. "function_definition" */
    uint32_t start, end; /* byte span in the text */
    bool error;          /* an ERROR node: text the grammar could not read */
} SyntaxNode;

typedef struct {
    SyntaxNode *v;
    size_t n;
    /* Bytes inside ERROR nodes and places where the grammar had to assume
     * a missing token, anywhere in the tree. */
    uint32_t error_bytes;
    uint32_t missing;
    bool timed_out; /* the parse was stopped: no tree, n == 0 */
} SyntaxOutline;

/* Parses `text` and fills `out` with the root's named children. False only
 * when the language has no grammar or the parse was stopped for taking too
 * long (SYNTAX_BUDGET_MS); a file with errors in it still gives an outline. */
#define SYNTAX_BUDGET_MS 2000
bool syntax_outline(Arena *a, SyntaxLang l, const char *text, size_t len,
                    SyntaxOutline *out);

/* Whether an outline is good enough to split its file by: it exists, and at
 * most SYNTAX_MAX_ERROR_PERCENT of the file is inside ERROR nodes. A few
 * unreadable lines (a macro the C grammar cannot expand, a syntax extension)
 * leave the rest of the file well structured; a file that is mostly error is
 * not in this language at all, or not finished. */
#define SYNTAX_MAX_ERROR_PERCENT 10
bool syntax_usable(const SyntaxOutline *o, size_t len);

/* WHERE A FILE IS CUT INTO CHUNKS, from its tree: split-then-merge, as cAST
 * does it.
 *
 *   - Siblings are grouped into units: a definition with the comments right
 *     above it, which belong to it.
 *   - Units are merged, in order, while the chunk stays within `target`
 *     bytes, so small neighbours (includes, one-line declarations, short
 *     functions) share a chunk and a whole definition that fits is never cut.
 *   - A unit larger than `target` is split along its own children, with its
 *     signature pushed onto the container path; one with no children to
 *     split along (a huge string, a table of data) is cut at line breaks.
 *
 * Chunk i runs from cuts[i].start to cuts[i + 1].start (the last to the end
 * of the file); cuts[0].start is 0, so the chunks tile the file with no gap.
 * A cut's heading is `container > signature` — "struct Store > bool
 * store_open(...)" — the first line of the first definition in the chunk,
 * under the definitions it sits inside.
 *
 * False when the file has no usable tree (syntax_usable); the caller then
 * cuts it into line windows. */
typedef struct {
    uint32_t start;
    const char *heading;
} SyntaxCut;

bool syntax_cuts(Arena *a, SyntaxLang l, const char *text, size_t len,
                 size_t target, SyntaxCut **cuts, size_t *n);

/* THE DEFINITIONS IN A FILE: what its grammar's tags query (upstream's
 * queries/tags.scm, compiled into kb by tools/gen_syntax_tags.py) names as a
 * definition — functions, methods, classes, structs, types, modules — and,
 * for assembly, its labels. What code search needs to know about a file
 * before it reads it: the keyword index boosts these names (a chunk's
 * symbols count three times), and a filed folder's documents carry them in
 * their meta. */
typedef struct {
    const char *name;
    const char *kind;      /* "function", "method", "class", "type", "label", ... */
    const char *signature; /* the definition's first line */
    const char *doc;       /* the comment right above it, or NULL */
    uint32_t start, end;   /* the definition's span */
    uint32_t line;         /* 1-based line it starts on */
} SyntaxSymbol;

bool syntax_symbols(Arena *a, SyntaxLang l, const char *text, size_t len,
                    SyntaxSymbol **out, size_t *n);

/* The generated patterns, per language (syntax_tags.c). */
extern const char *const *const SYNTAX_TAGS[SYNTAX_COUNT];

#endif /* KB_SYNTAX_H */
