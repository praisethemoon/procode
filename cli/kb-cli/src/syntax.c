#include "syntax.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tree_sitter/api.h>

/* Each grammar's entry point, from its generated parser.c. */
const TSLanguage *tree_sitter_c(void);
const TSLanguage *tree_sitter_typescript(void);
const TSLanguage *tree_sitter_tsx(void);
const TSLanguage *tree_sitter_javascript(void);
const TSLanguage *tree_sitter_python(void);
const TSLanguage *tree_sitter_go(void);
const TSLanguage *tree_sitter_rust(void);
const TSLanguage *tree_sitter_asm(void);

static const struct {
    const char *name;
    const TSLanguage *(*language)(void);
} LANGS[SYNTAX_COUNT] = {
    [SYNTAX_NONE] = {"none", NULL},
    [SYNTAX_C] = {"c", tree_sitter_c},
    [SYNTAX_TYPESCRIPT] = {"typescript", tree_sitter_typescript},
    [SYNTAX_TSX] = {"tsx", tree_sitter_tsx},
    [SYNTAX_JAVASCRIPT] = {"javascript", tree_sitter_javascript},
    [SYNTAX_PYTHON] = {"python", tree_sitter_python},
    [SYNTAX_GO] = {"go", tree_sitter_go},
    [SYNTAX_RUST] = {"rust", tree_sitter_rust},
    [SYNTAX_ASM] = {"asm", tree_sitter_asm},
};

SyntaxLang syntax_lang(const char *path) {
    static const struct {
        const char *ext;
        SyntaxLang lang;
    } map[] = {
        {".c", SYNTAX_C},           {".h", SYNTAX_C},
        {".ts", SYNTAX_TYPESCRIPT}, {".mts", SYNTAX_TYPESCRIPT},
        {".cts", SYNTAX_TYPESCRIPT}, {".tsx", SYNTAX_TSX},
        {".js", SYNTAX_JAVASCRIPT}, {".mjs", SYNTAX_JAVASCRIPT},
        {".cjs", SYNTAX_JAVASCRIPT}, {".jsx", SYNTAX_JAVASCRIPT},
        {".py", SYNTAX_PYTHON},     {".pyi", SYNTAX_PYTHON},
        {".go", SYNTAX_GO},         {".rs", SYNTAX_RUST},
        {".s", SYNTAX_ASM},         {".S", SYNTAX_ASM},
        {".asm", SYNTAX_ASM},       {".nasm", SYNTAX_ASM},
        {NULL, SYNTAX_NONE}};
    if (!path)
        return SYNTAX_NONE;
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash))
        return SYNTAX_NONE;
    for (int32_t i = 0; map[i].ext; i++)
        if (strcmp(dot, map[i].ext) == 0)
            return map[i].lang;
    return SYNTAX_NONE;
}

const char *syntax_lang_name(SyntaxLang l) {
    return l < SYNTAX_COUNT ? LANGS[l].name : "none";
}

/* ---- parsing ------------------------------------------------------------ */

static double now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

typedef struct {
    double deadline;
    bool stopped;
} Budget;

/* Called by the parser as it goes; returning true stops it. A pathological
 * input (a megabyte on one line, an ambiguity the grammar explores
 * exponentially) costs its file the outline, not the whole filing. */
static bool over_budget(TSParseState *state) {
    Budget *b = (Budget *)state->payload;
    if (now_ms() > b->deadline)
        b->stopped = true;
    return b->stopped;
}

typedef struct {
    const char *text;
    uint32_t len;
} Source;

static const char *read_source(void *payload, uint32_t byte, TSPoint pos, uint32_t *n) {
    (void)pos;
    const Source *s = (const Source *)payload;
    if (byte >= s->len) {
        *n = 0;
        return "";
    }
    *n = s->len - byte;
    return s->text + byte;
}

/* Error bytes and missing tokens in a subtree. Only subtrees that report an
 * error are entered, and an ERROR node is counted whole rather than again for
 * what is inside it. */
static void count_errors(TSNode node, uint32_t *bytes, uint32_t *missing) {
    if (ts_node_is_missing(node)) {
        (*missing)++;
        return;
    }
    if (ts_node_is_error(node)) {
        *bytes += ts_node_end_byte(node) - ts_node_start_byte(node);
        return;
    }
    if (!ts_node_has_error(node))
        return;
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; i++)
        count_errors(ts_node_child(node, i), bytes, missing);
}

/* The tree for a text, or NULL when the language has no grammar or the parse
 * ran out of time (then *timed_out). The caller deletes the tree. */
static TSTree *parse(SyntaxLang l, const char *text, size_t len, bool *timed_out) {
    *timed_out = false;
    if (l <= SYNTAX_NONE || l >= SYNTAX_COUNT || len > UINT32_MAX)
        return NULL;
    TSParser *p = ts_parser_new();
    if (!p)
        return NULL;
    if (!ts_parser_set_language(p, LANGS[l].language())) {
        ts_parser_delete(p);
        return NULL;
    }
    Source src = {text, (uint32_t)len};
    TSInput input = {&src, read_source, TSInputEncodingUTF8, NULL};
    Budget budget = {now_ms() + SYNTAX_BUDGET_MS, false};
    TSParseOptions opts = {&budget, over_budget};
    TSTree *tree = ts_parser_parse_with_options(p, NULL, input, opts);
    ts_parser_delete(p);
    *timed_out = !tree && budget.stopped;
    return tree;
}

static void fill_outline(Arena *a, TSNode root, SyntaxOutline *out) {
    uint32_t n = ts_node_named_child_count(root);
    out->v = (SyntaxNode *)arena_alloc(a, (n ? n : 1) * sizeof(SyntaxNode));
    for (uint32_t i = 0; i < n; i++) {
        TSNode c = ts_node_named_child(root, i);
        SyntaxNode *s = &out->v[out->n++];
        /* Node type names are static strings in the grammar's tables, which
         * are compiled into kb, so they outlive the tree. */
        s->type = ts_node_type(c);
        s->start = ts_node_start_byte(c);
        s->end = ts_node_end_byte(c);
        s->error = ts_node_is_error(c);
    }
    count_errors(root, &out->error_bytes, &out->missing);
}

bool syntax_outline(Arena *a, SyntaxLang l, const char *text, size_t len,
                    SyntaxOutline *out) {
    memset(out, 0, sizeof *out);
    TSTree *tree = parse(l, text, len, &out->timed_out);
    if (!tree)
        return false;
    fill_outline(a, ts_tree_root_node(tree), out);
    ts_tree_delete(tree);
    return true;
}

bool syntax_usable(const SyntaxOutline *o, size_t len) {
    if (o->timed_out || (o->n == 0 && len > 0))
        return false;
    return (uint64_t)o->error_bytes * 100 <= (uint64_t)len * SYNTAX_MAX_ERROR_PERCENT;
}

/* ---- cutting into chunks ------------------------------------------------ */

typedef struct {
    Arena *a;
    const char *text;
    size_t target;
    SyntaxCut *v;
    size_t n, cap;
} Cutter;

/* A definition, and the comments and attributes directly above it. */
typedef struct {
    uint32_t start, end;
    TSNode main;
} Unit;

/* What belongs to the node after it: a comment, or an attribute (Rust's
 * `#[derive(...)]`, C's `__attribute__` on its own line). */
static bool is_leading(TSNode n) {
    const char *t = ts_node_type(n);
    return strstr(t, "comment") != NULL || strncmp(t, "attribute", 9) == 0;
}

/* Node types that name something: what a chunk's heading should come from,
 * rather than an include or a directive that happens to open the chunk.
 * Whole words of the type name, split at `_`: a substring would count an
 * assembler's `instruction` as a `struct`. */
static bool is_definition(TSNode n) {
    static const char *const yes[] = {
        "function", "method", "class", "struct", "enum", "union", "interface",
        "type", "impl", "trait", "mod", "label", "definition", "declaration",
        "item", "macro", "namespace", "export", "def", NULL};
    static const char *const no[] = {"import", "include", "use", "comment",
                                     "package", "attribute", NULL};
    if (ts_node_start_byte(n) == ts_node_end_byte(n))
        return false; /* a token the grammar had to assume, not text */
    const char *t = ts_node_type(n);
    bool hit = false;
    while (*t) {
        const char *us = strchr(t, '_');
        size_t len = us ? (size_t)(us - t) : strlen(t);
        for (int32_t i = 0; no[i]; i++)
            if (strlen(no[i]) == len && strncmp(t, no[i], len) == 0)
                return false;
        for (int32_t i = 0; yes[i]; i++)
            if (strlen(yes[i]) == len && strncmp(t, yes[i], len) == 0)
                hit = true;
        t += len + (us ? 1 : 0);
        if (!us)
            break;
    }
    return hit;
}

/* The first non-blank line of a node, trimmed, at most 160 bytes, cut on a
 * character boundary. */
static const char *signature(Cutter *c, TSNode n) {
    const char *p = c->text + ts_node_start_byte(n);
    const char *end = c->text + ts_node_end_byte(n);
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *q = eol ? eol : end;
        while (q > p && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r'))
            q--;
        if (q > p) {
            size_t k = (size_t)(q - p);
            if (k > 160) {
                k = 160;
                while (k > 0 && ((unsigned char)p[k] & 0xC0) == 0x80)
                    k--;
            }
            return arena_strndup(c->a, p, k);
        }
        if (!eol)
            break;
        p = eol + 1;
    }
    return NULL;
}

/* A heading line that says nothing: only brackets and punctuation. */
static bool bare(const char *s) {
    for (; *s; s++)
        if (!strchr("{}()[];,: \t", *s))
            return false;
    return true;
}

static const char *joined(Cutter *c, const char *container, const char *sig) {
    if (!sig || bare(sig))
        return container;
    /* A definition's first child often starts on its own first line (a C
     * function's declarator): the same line again says nothing new. */
    if (container) {
        size_t lc = strlen(container), ls = strlen(sig);
        if (ls <= lc && strcmp(container + lc - ls, sig) == 0)
            return container;
        const char *last = strrchr(container, '>');
        last = last ? last + 2 : container;
        if (strstr(last, sig))
            return container;
    }
    return container ? arena_printf(c->a, "%s > %s", container, sig) : sig;
}

/* A cut at `at`. A second cut at the same place keeps the first one's
 * heading, which names what the chunk opens with. */
static void cut(Cutter *c, uint32_t at, const char *heading) {
    if (c->n && c->v[c->n - 1].start >= at) {
        if (!c->v[c->n - 1].heading)
            c->v[c->n - 1].heading = heading;
        return;
    }
    ARENA_GROW(c->a, c->v, c->n, c->cap, SyntaxCut);
    c->v[c->n].start = at;
    c->v[c->n].heading = heading;
    c->n++;
}

/* A span with nothing to split along, cut at line breaks every `target`
 * bytes or so. */
static void cut_lines(Cutter *c, uint32_t lo, uint32_t hi, const char *heading) {
    cut(c, lo, heading);
    uint32_t pos = lo;
    while (hi - pos > c->target) {
        uint32_t want = pos + (uint32_t)c->target, at = want;
        while (at > pos + c->target / 2 && c->text[at - 1] != '\n')
            at--;
        if (at <= pos + c->target / 2)
            at = want;
        cut(c, at, heading);
        pos = at;
    }
}

static size_t units_of(Cutter *c, TSNode parent, Unit **out) {
    uint32_t n = ts_node_named_child_count(parent);
    Unit *v = (Unit *)arena_alloc(c->a, (n ? n : 1) * sizeof(Unit));
    size_t k = 0;
    bool pending = false;
    uint32_t pending_start = 0;
    for (uint32_t i = 0; i < n; i++) {
        TSNode ch = ts_node_named_child(parent, i);
        if (is_leading(ch) && i + 1 < n) {
            if (!pending)
                pending_start = ts_node_start_byte(ch);
            pending = true;
            continue;
        }
        v[k].start = pending ? pending_start : ts_node_start_byte(ch);
        v[k].end = ts_node_end_byte(ch);
        v[k].main = ch;
        k++;
        pending = false;
    }
    *out = v;
    return k;
}

/* Cuts [lo, hi), the span of `parent`'s children (and what precedes the first
 * of them from lo), into chunks of at most target bytes. */
static void split(Cutter *c, TSNode parent, uint32_t lo, uint32_t hi,
                  const char *container, int32_t depth) {
    Unit *u;
    size_t n = units_of(c, parent, &u);
    if (n == 0) {
        cut_lines(c, lo, hi, container);
        return;
    }
    bool open = false, head_is_def = false;
    uint32_t start = lo;
    for (size_t i = 0; i < n; i++) {
        const uint32_t ustart = i == 0 ? lo : u[i].start;
        const char *head = joined(c, container, signature(c, u[i].main));
        const bool def = is_definition(u[i].main);
        if (open && u[i].end - start <= c->target) {
            /* It fits: merged into the open chunk. The chunk is named by
             * its first definition, not by an include that opened it. */
            if (def && !head_is_def) {
                c->v[c->n - 1].heading = head;
                head_is_def = true;
            }
            continue;
        }
        /* A small chunk still open before a unit too big to join it — a
         * function's `static` and return type, the includes above a large
         * function — goes with that unit's first piece, so a signature is
         * never a chunk of its own. */
        const bool too_big = u[i].end - ustart > c->target;
        const bool absorb = too_big && open && ustart - start < c->target / 4;
        if (absorb && !head_is_def)
            c->v[c->n - 1].heading = NULL; /* the big unit will name it */
        if (!absorb)
            start = ustart;
        if (!too_big) {
            cut(c, start, head);
            open = true;
            head_is_def = def;
            continue;
        }
        open = false;
        if (depth < 32 && ts_node_named_child_count(u[i].main) > 0) {
            /* Along its own children. A definition becomes the container of
             * what is inside it; a block or a statement does not, so the path
             * reads "fn > if (x) {" and not "fn > { > if (x) {". */
            const char *inner = def ? joined(c, container, signature(c, u[i].main)) : container;
            if (!c->n || c->v[c->n - 1].start != start || !c->v[c->n - 1].heading)
                cut(c, start, head);
            split(c, u[i].main, start, u[i].end, inner, depth + 1);
        } else {
            cut_lines(c, start, u[i].end, head);
        }
    }
    (void)hi;
}

bool syntax_cuts(Arena *a, SyntaxLang l, const char *text, size_t len,
                 size_t target, SyntaxCut **cuts, size_t *n) {
    *cuts = NULL;
    *n = 0;
    bool timed_out;
    TSTree *tree = parse(l, text, len, &timed_out);
    if (!tree)
        return false;
    TSNode root = ts_tree_root_node(tree);
    SyntaxOutline o;
    memset(&o, 0, sizeof o);
    fill_outline(a, root, &o);
    if (len == 0 || !syntax_usable(&o, len)) {
        ts_tree_delete(tree);
        return false;
    }
    Cutter c = {a, text, target ? target : 1, NULL, 0, 0};
    split(&c, root, 0, (uint32_t)len, NULL, 0);
    ts_tree_delete(tree);
    /* Merge: neighbours that fit together become one chunk, which takes up
     * the pieces a split leaves behind (the closing lines of a function whose
     * body was split, a lone `return`). The first one's heading names it. */
    size_t k = 0;
    for (size_t i = 0; i < c.n; i++) {
        uint32_t end = i + 1 < c.n ? c.v[i + 1].start : (uint32_t)len;
        if (k > 0 && end - c.v[k - 1].start <= c.target) {
            if (!c.v[k - 1].heading)
                c.v[k - 1].heading = c.v[i].heading;
            continue;
        }
        c.v[k++] = c.v[i];
    }
    c.n = k;
    if (c.n == 0 || c.v[0].start != 0)
        cut(&c, 0, NULL); /* unreachable: the first unit opens at 0 */
    *cuts = c.v;
    *n = c.n;
    return true;
}

/* ---- symbols ------------------------------------------------------------ */

/* One compiled query per language, built on first use from the patterns this
 * grammar accepts: a pattern that names a node the grammar lacks (JavaScript's
 * patterns run against TypeScript) is left out rather than failing the rest.
 * kb is single-threaded outside the embedder, so a plain cache is enough. */
static TSQuery *tag_queries[SYNTAX_COUNT];
static bool tag_tried[SYNTAX_COUNT];

static TSQuery *tag_query(SyntaxLang l) {
    if (tag_tried[l])
        return tag_queries[l];
    tag_tried[l] = true;
    const char *const *pats = SYNTAX_TAGS[l];
    if (!pats)
        return NULL;
    const TSLanguage *lang = LANGS[l].language();
    size_t cap = 1, used = 0;
    for (size_t i = 0; pats[i]; i++)
        cap += strlen(pats[i]) + 1;
    char *src = (char *)malloc(cap);
    if (!src)
        return NULL;
    src[0] = '\0';
    for (size_t i = 0; pats[i]; i++) {
        uint32_t off;
        TSQueryError err;
        TSQuery *one = ts_query_new(lang, pats[i], (uint32_t)strlen(pats[i]), &off, &err);
        if (!one)
            continue;
        ts_query_delete(one);
        size_t k = strlen(pats[i]);
        memcpy(src + used, pats[i], k);
        used += k;
        src[used++] = '\n';
        src[used] = '\0';
    }
    uint32_t off;
    TSQueryError err;
    tag_queries[l] = used ? ts_query_new(lang, src, (uint32_t)used, &off, &err) : NULL;
    free(src);
    return tag_queries[l];
}

static const char *first_line(Arena *a, const char *text, uint32_t start, uint32_t end) {
    const char *p = text + start, *e = text + end;
    const char *nl = memchr(p, '\n', (size_t)(e - p));
    const char *q = nl ? nl : e;
    while (q > p && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r'))
        q--;
    size_t k = (size_t)(q - p);
    if (k > 160) {
        k = 160;
        while (k > 0 && ((unsigned char)p[k] & 0xC0) == 0x80)
            k--;
    }
    return arena_strndup(a, p, k);
}

/* The comment directly above a definition, if its last line touches it. */
static const char *doc_above(Arena *a, const char *text, TSNode def) {
    TSNode prev = ts_node_prev_named_sibling(def);
    if (ts_node_is_null(prev) || !strstr(ts_node_type(prev), "comment"))
        return NULL;
    if (ts_node_end_point(prev).row + 1 < ts_node_start_point(def).row)
        return NULL;
    uint32_t s = ts_node_start_byte(prev), e = ts_node_end_byte(prev);
    if (e - s > 400)
        e = s + 400;
    return arena_strndup(a, text + s, e - s);
}

bool syntax_symbols(Arena *a, SyntaxLang l, const char *text, size_t len,
                    SyntaxSymbol **out, size_t *n) {
    *out = NULL;
    *n = 0;
    bool timed_out;
    TSTree *tree = parse(l, text, len, &timed_out);
    if (!tree)
        return false;
    TSNode root = ts_tree_root_node(tree);
    SyntaxSymbol *v = NULL;
    size_t k = 0, cap = 0;
    if (l == SYNTAX_ASM) {
        /* An assembler's definitions are its labels. */
        uint32_t nc = ts_node_named_child_count(root);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode c = ts_node_named_child(root, i);
            if (strcmp(ts_node_type(c), "label") != 0)
                continue;
            uint32_t s = ts_node_start_byte(c), e = ts_node_end_byte(c);
            while (e > s && (text[e - 1] == ':' || text[e - 1] == ' '))
                e--;
            if (e == s)
                continue;
            ARENA_GROW(a, v, k, cap, SyntaxSymbol);
            v[k].name = arena_strndup(a, text + s, e - s);
            v[k].kind = "label";
            v[k].signature = v[k].name;
            v[k].doc = doc_above(a, text, c);
            v[k].start = s;
            v[k].end = ts_node_end_byte(c);
            v[k].line = ts_node_start_point(c).row + 1;
            k++;
        }
    } else {
        TSQuery *q = tag_query(l);
        TSQueryCursor *cur = q ? ts_query_cursor_new() : NULL;
        if (cur) {
            ts_query_cursor_exec(cur, q, root);
            TSQueryMatch m;
            while (ts_query_cursor_next_match(cur, &m)) {
                TSNode name = {0}, def = {0};
                bool has_name = false, has_def = false;
                const char *kind = NULL;
                for (uint16_t i = 0; i < m.capture_count; i++) {
                    uint32_t cl;
                    const char *cn = ts_query_capture_name_for_id(q, m.captures[i].index, &cl);
                    if (cl == 4 && strncmp(cn, "name", 4) == 0) {
                        name = m.captures[i].node;
                        has_name = true;
                    } else if (cl > 11 && strncmp(cn, "definition.", 11) == 0) {
                        def = m.captures[i].node;
                        has_def = true;
                        kind = arena_strndup(a, cn + 11, cl - 11);
                    }
                }
                if (!has_name || !has_def)
                    continue;
                uint32_t ns = ts_node_start_byte(name), ne = ts_node_end_byte(name);
                if (ne <= ns || ne - ns > 200)
                    continue;
                ARENA_GROW(a, v, k, cap, SyntaxSymbol);
                v[k].name = arena_strndup(a, text + ns, ne - ns);
                v[k].kind = kind;
                v[k].start = ts_node_start_byte(def);
                v[k].end = ts_node_end_byte(def);
                v[k].signature = first_line(a, text, v[k].start, v[k].end);
                v[k].doc = doc_above(a, text, def);
                v[k].line = ts_node_start_point(def).row + 1;
                k++;
            }
            ts_query_cursor_delete(cur);
        }
    }
    ts_tree_delete(tree);
    *out = v;
    *n = k;
    return true;
}

/* ---- imports ------------------------------------------------------------ */

typedef struct {
    Arena *a;
    const char *text;
    SyntaxImport *v;
    size_t n, cap;
} Imports;

static void import_add(Imports *im, const char *spec, size_t len, bool system) {
    if (len == 0 || len > 1024)
        return;
    ARENA_GROW(im->a, im->v, im->n, im->cap, SyntaxImport);
    im->v[im->n].spec = arena_strndup(im->a, spec, len);
    im->v[im->n].system = system;
    im->n++;
}

/* A string node's text without its quotes (or C's angle brackets). */
static void import_quoted(Imports *im, TSNode s, bool system) {
    uint32_t b = ts_node_start_byte(s), e = ts_node_end_byte(s);
    if (e - b < 2)
        return;
    import_add(im, im->text + b + 1, e - b - 2, system);
}

static bool node_is(TSNode n, const char *type) {
    return !ts_node_is_null(n) && strcmp(ts_node_type(n), type) == 0;
}

static bool node_text_is(const Imports *im, TSNode n, const char *want) {
    uint32_t b = ts_node_start_byte(n), e = ts_node_end_byte(n);
    return strlen(want) == e - b && memcmp(im->text + b, want, e - b) == 0;
}

static void imports_c(Imports *im, TSNode n) {
    if (!node_is(n, "preproc_include"))
        return;
    TSNode path = ts_node_child_by_field_name(n, "path", 4);
    if (node_is(path, "string_literal"))
        import_quoted(im, path, false);
    else if (node_is(path, "system_lib_string"))
        import_quoted(im, path, true);
}

static void imports_js(Imports *im, TSNode n) {
    const char *t = ts_node_type(n);
    if (strcmp(t, "import_statement") == 0 || strcmp(t, "export_statement") == 0 ||
        strcmp(t, "import_require_clause") == 0) {
        TSNode src = ts_node_child_by_field_name(n, "source", 6);
        if (node_is(src, "string"))
            import_quoted(im, src, false);
    } else if (strcmp(t, "call_expression") == 0) {
        TSNode fn = ts_node_child_by_field_name(n, "function", 8);
        bool req = node_is(fn, "identifier") && node_text_is(im, fn, "require");
        if (!req && !node_is(fn, "import"))
            return;
        TSNode args = ts_node_child_by_field_name(n, "arguments", 9);
        TSNode first = ts_node_is_null(args) ? args : ts_node_named_child(args, 0);
        if (node_is(first, "string"))
            import_quoted(im, first, false);
    }
}

static void import_node_text(Imports *im, TSNode n, const char *prefix) {
    uint32_t b = ts_node_start_byte(n), e = ts_node_end_byte(n);
    if (!prefix || !prefix[0]) {
        import_add(im, im->text + b, e - b, false);
        return;
    }
    const char *s = arena_printf(im->a, "%s%.*s", prefix, (int)(e - b), im->text + b);
    import_add(im, s, strlen(s), false);
}

/* A name in an import: a dotted_name, or the name of an aliased_import. */
static TSNode py_name(TSNode n) {
    if (node_is(n, "aliased_import"))
        return ts_node_child_by_field_name(n, "name", 4);
    return node_is(n, "dotted_name") ? n : (TSNode){0};
}

static void imports_py(Imports *im, TSNode n) {
    const char *t = ts_node_type(n);
    bool plain = strcmp(t, "import_statement") == 0;
    if (!plain && strcmp(t, "import_from_statement") != 0)
        return;
    const char *dots = NULL; /* "from . import x": the module is each x */
    if (!plain) {
        TSNode mod = ts_node_child_by_field_name(n, "module_name", 11);
        if (node_is(mod, "dotted_name")) {
            import_node_text(im, mod, NULL);
            return;
        }
        if (!node_is(mod, "relative_import"))
            return;
        if (ts_node_named_child_count(mod) > 1) { /* dots and a name */
            import_node_text(im, mod, NULL);
            return;
        }
        uint32_t b = ts_node_start_byte(mod), e = ts_node_end_byte(mod);
        dots = arena_strndup(im->a, im->text + b, e - b);
    }
    uint32_t nc = ts_node_child_count(n);
    for (uint32_t i = 0; i < nc; i++) {
        const char *field = ts_node_field_name_for_child(n, i);
        if (!field || strcmp(field, "name") != 0)
            continue;
        TSNode name = py_name(ts_node_child(n, i));
        if (!ts_node_is_null(name))
            import_node_text(im, name, dots);
    }
}

bool syntax_imports(Arena *a, SyntaxLang l, const char *text, size_t len,
                    SyntaxImport **out, size_t *n) {
    *out = NULL;
    *n = 0;
    void (*visit)(Imports *, TSNode) =
        l == SYNTAX_C ? imports_c
        : l == SYNTAX_TYPESCRIPT || l == SYNTAX_TSX || l == SYNTAX_JAVASCRIPT ? imports_js
        : l == SYNTAX_PYTHON ? imports_py
                             : NULL;
    if (!visit)
        return true;
    bool timed_out;
    TSTree *tree = parse(l, text, len, &timed_out);
    if (!tree)
        return false;
    Imports im = {a, text, NULL, 0, 0};
    /* Every node, depth first, with the cursor rather than recursion: an
     * include can sit under any depth of #if, an import inside a function. */
    TSTreeCursor cur = ts_tree_cursor_new(ts_tree_root_node(tree));
    for (;;) {
        visit(&im, ts_tree_cursor_current_node(&cur));
        if (ts_tree_cursor_goto_first_child(&cur))
            continue;
        while (!ts_tree_cursor_goto_next_sibling(&cur)) {
            if (!ts_tree_cursor_goto_parent(&cur))
                goto walked;
        }
    }
walked:
    ts_tree_cursor_delete(&cur);
    ts_tree_delete(tree);
    *out = im.v;
    *n = im.n;
    return true;
}
