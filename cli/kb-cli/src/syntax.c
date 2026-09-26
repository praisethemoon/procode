#include "syntax.h"

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

bool syntax_outline(Arena *a, SyntaxLang l, const char *text, size_t len,
                    SyntaxOutline *out) {
    memset(out, 0, sizeof *out);
    if (l <= SYNTAX_NONE || l >= SYNTAX_COUNT || len > UINT32_MAX)
        return false;
    TSParser *p = ts_parser_new();
    if (!p)
        return false;
    if (!ts_parser_set_language(p, LANGS[l].language())) {
        ts_parser_delete(p);
        return false;
    }
    Source src = {text, (uint32_t)len};
    TSInput input = {&src, read_source, TSInputEncodingUTF8, NULL};
    Budget budget = {now_ms() + SYNTAX_BUDGET_MS, false};
    TSParseOptions opts = {&budget, over_budget};
    TSTree *tree = ts_parser_parse_with_options(p, NULL, input, opts);
    if (!tree) {
        ts_parser_delete(p);
        out->timed_out = budget.stopped;
        return false;
    }
    TSNode root = ts_tree_root_node(tree);
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
    ts_tree_delete(tree);
    ts_parser_delete(p);
    return true;
}

bool syntax_usable(const SyntaxOutline *o, size_t len) {
    if (o->timed_out || (o->n == 0 && len > 0))
        return false;
    return (uint64_t)o->error_bytes * 100 <= (uint64_t)len * SYNTAX_MAX_ERROR_PERCENT;
}
