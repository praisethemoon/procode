#include "chunk.h"

#include "kb.h"

/* ---- language detection ----------------------------------------------- */

static bool ends_with_ci(const char *s, const char *suffix) {
    if (!s)
        return false;
    size_t n = strlen(s), m = strlen(suffix);
    if (m > n)
        return false;
    for (size_t i = 0; i < m; i++) {
        char a = s[n - m + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

static bool any_suffix(const char *s, const char *const *list) {
    for (int32_t i = 0; list[i]; i++) {
        if (ends_with_ci(s, list[i]))
            return true;
    }
    return false;
}

const char *chunk_lang_name(Lang l) {
    switch (l) {
    case LANG_MARKDOWN: return "markdown";
    case LANG_HTML: return "html";
    case LANG_CODE: return "code";
    case LANG_TEXT: return "text";
    }
    return "text";
}

Lang chunk_lang(const char *mime, const char *path) {
    static const char *const md_ext[] = {".md", ".markdown", ".mdx", NULL};
    static const char *const html_ext[] = {".html", ".htm", ".xhtml", NULL};
    static const char *const code_ext[] = {
        ".c",   ".h",    ".cc",  ".cpp", ".cxx", ".hpp", ".hh",  ".m",
        ".mm",  ".java", ".js",  ".mjs", ".cjs", ".jsx", ".ts",  ".tsx",
        ".go",  ".rs",   ".py",  ".rb",  ".php", ".cs",  ".swift", ".kt",
        ".zig", ".lua",  ".sh",  ".bash", ".sql", ".css", ".scss", ".tc",
        ".pl",  ".ex",   ".exs", ".hs",  ".ml",  ".scala", ".dart", ".s",
        ".S",   ".asm",  ".nasm", ".mts", ".cts", ".pyi", NULL};

    /* Markdown and HTML are checked first because both have mime types that
     * would also satisfy the text/x- rule below. */
    if (mime) {
        if (strcmp(mime, "text/markdown") == 0 ||
            strcmp(mime, "text/x-markdown") == 0)
            return LANG_MARKDOWN;
        if (strcmp(mime, "text/html") == 0 ||
            strcmp(mime, "application/xhtml+xml") == 0)
            return LANG_HTML;
        if (strncmp(mime, "text/x-", 7) == 0 ||
            strcmp(mime, "application/javascript") == 0 ||
            strcmp(mime, "text/javascript") == 0 ||
            strcmp(mime, "application/typescript") == 0 ||
            strcmp(mime, "application/x-sh") == 0 ||
            strcmp(mime, "application/json") == 0)
            return LANG_CODE;
    }
    if (path && path[0]) {
        if (any_suffix(path, md_ext))
            return LANG_MARKDOWN;
        if (any_suffix(path, html_ext))
            return LANG_HTML;
        if (any_suffix(path, code_ext))
            return LANG_CODE;
    }
    return LANG_TEXT;
}

/* Extension to mime, for the common documentation and source types. The
 * chunker also looks at the path, so this only has to be right often enough
 * to be useful — an explicit --mime always wins. Matched case-sensitively:
 * `.S` is assembly that goes through the C preprocessor, `.s` is not. */
const char *chunk_mime_from_path(const char *path) {
    static const struct {
        const char *ext;
        const char *mime;
    } map[] = {{".md", "text/markdown"},        {".markdown", "text/markdown"},
               {".mdx", "text/markdown"},       {".html", "text/html"},
               {".htm", "text/html"},           {".txt", "text/plain"},
               {".rst", "text/plain"},          {".c", "text/x-c"},
               {".h", "text/x-c"},              {".cc", "text/x-c++"},
               {".cpp", "text/x-c++"},          {".cxx", "text/x-c++"},
               {".hpp", "text/x-c++"},          {".hh", "text/x-c++"},
               {".m", "text/x-objc"},           {".mm", "text/x-objc"},
               {".py", "text/x-python"},        {".pyi", "text/x-python"},
               {".rs", "text/x-rust"},
               {".go", "text/x-go"},            {".java", "text/x-java"},
               {".kt", "text/x-kotlin"},        {".swift", "text/x-swift"},
               {".rb", "text/x-ruby"},          {".php", "text/x-php"},
               {".cs", "text/x-csharp"},        {".lua", "text/x-lua"},
               {".zig", "text/x-zig"},          {".s", "text/x-asm"},
               {".S", "text/x-asm"},            {".asm", "text/x-asm"},
               {".nasm", "text/x-asm"},
               {".js", "text/javascript"},      {".mjs", "text/javascript"},
               {".cjs", "text/javascript"},     {".jsx", "text/javascript"},
               {".ts", "application/typescript"}, {".tsx", "application/typescript"},
               {".mts", "application/typescript"}, {".cts", "application/typescript"},
               {".json", "application/json"},   {".yaml", "application/yaml"},
               {".yml", "application/yaml"},    {".toml", "application/toml"},
               {".sh", "application/x-sh"},     {".bash", "application/x-sh"},
               {".css", "text/x-css"},          {".scss", "text/x-css"},
               {".sql", "text/x-sql"},          {".cmake", "text/x-cmake"},
               {".tc", "text/x-typec"},         {NULL, NULL}};
    if (!path || !path[0])
        return NULL;
    size_t n = strlen(path);
    for (int32_t i = 0; map[i].ext; i++) {
        size_t m = strlen(map[i].ext);
        if (n > m && strcmp(path + n - m, map[i].ext) == 0)
            return map[i].mime;
    }
    return NULL;
}

bool chunk_mime_supported(const char *mime) {
    static const char *const text_like[] = {
        "application/xhtml+xml",  "application/javascript",
        "application/typescript", "application/x-sh",
        "application/json",       "application/xml",
        "application/x-yaml",     "application/yaml",
        "application/toml",       NULL};
    if (strncmp(mime, "text/", 5) == 0)
        return true;
    for (int32_t i = 0; text_like[i]; i++)
        if (strcmp(mime, text_like[i]) == 0)
            return true;
    return false;
}

uint32_t chunk_tokens_of(size_t bytes) {
    return (uint32_t)((bytes + KB_BYTES_PER_TOKEN - 1) / KB_BYTES_PER_TOKEN);
}

/* ---- sections ---------------------------------------------------------
 *
 * A section is a heading-to-heading (or declaration-to-declaration) span.
 * Sections tile the document: the first starts at 0, the last ends at len,
 * and each begins where the previous one ended.
 */

typedef struct {
    size_t start;
    const char *heading;
    const char *context;
} Section;

typedef struct {
    Arena *a;
    Section *v;
    size_t n, cap;
    /* The heading open at each level (1-6), for the context of the next. */
    const char *open[7];
} Sections;

static void section_add(Sections *s, size_t start, const char *heading) {
    /* The caller walks forward, so a repeated start is the same boundary
     * found twice — an empty section helps nobody. The placeholder section
     * at offset 0 has no heading, so a document that opens with one still
     * gets it. */
    if (s->n && s->v[s->n - 1].start == start) {
        if (!s->v[s->n - 1].heading)
            s->v[s->n - 1].heading = heading;
        return;
    }
    ARENA_GROW(s->a, s->v, s->n, s->cap, Section);
    s->v[s->n].start = start;
    s->v[s->n].heading = heading;
    s->v[s->n].context = NULL;
    s->n++;
}

/* A heading at `level` (1-6): its context is the headings still open above
 * it, and it closes every deeper one. A document that skips a level (# then
 * ###) simply has no heading at the one it skipped. */
static void section_add_level(Sections *s, size_t start, const char *heading,
                              int32_t level) {
    const char *context = NULL;
    for (int32_t k = 1; k < level; k++) {
        if (!s->open[k])
            continue;
        context = context ? arena_printf(s->a, "%s > %s", context, s->open[k])
                          : s->open[k];
    }
    section_add(s, start, heading);
    if (s->n && s->v[s->n - 1].start == start && !s->v[s->n - 1].context)
        s->v[s->n - 1].context = context;
    s->open[level] = heading;
    for (int32_t k = level + 1; k <= 6; k++)
        s->open[k] = NULL;
}

static const char *trim_copy(Arena *a, const char *p, size_t n, size_t cap) {
    while (n && (*p == ' ' || *p == '\t' || *p == '\r')) {
        p++;
        n--;
    }
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r'))
        n--;
    if (n == 0)
        return NULL;
    if (n > cap) {
        /* The clamp is a byte count and a heading is text, so the cut can
         * land inside a character. A heading goes out in a search hit and
         * in `kb chunk`, both of which are JSON, and half a character there
         * makes the whole document unparseable for the caller. */
        n = cap;
        n -= utf8_dangling(p, n);
        if (n == 0)
            return NULL;
    }
    return arena_strndup(a, p, n);
}

/* ---- markdown ---------------------------------------------------------- */

static bool fence_line(Str l) {
    return (l.len >= 3 && l.ptr[0] == '`' && l.ptr[1] == '`' &&
            l.ptr[2] == '`') ||
           (l.len >= 3 && l.ptr[0] == '~' && l.ptr[1] == '~' &&
            l.ptr[2] == '~');
}

static void sections_markdown(Sections *out, const char *text, Lines l) {
    bool in_fence = false;
    for (int32_t i = 0; i < l.count; i++) {
        Str line = l.lines[i];
        if (fence_line(line)) {
            in_fence = !in_fence;
            continue;
        }
        /* A '#' inside a fenced block is a shell comment or a preprocessor
         * directive, not a heading. Splitting there would cut a code sample
         * in half. */
        if (in_fence || line.len == 0 || line.ptr[0] != '#')
            continue;
        size_t h = 0;
        while (h < line.len && h < 6 && line.ptr[h] == '#')
            h++;
        if (h >= line.len || (line.ptr[h] != ' ' && line.ptr[h] != '\t'))
            continue;
        const char *title = trim_copy(out->a, line.ptr + h, line.len - h, 200);
        /* ATX headings may be closed with trailing '#'s. */
        if (title) {
            size_t tn = strlen(title);
            while (tn && title[tn - 1] == '#')
                tn--;
            title = trim_copy(out->a, title, tn, 200);
        }
        section_add_level(out, (size_t)(line.ptr - text), title, (int32_t)h);
    }
}

/* ---- html -------------------------------------------------------------- */

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Text of an element with tags removed and whitespace collapsed. Entities
 * are left as written: decoding them here would make the heading disagree
 * with the bytes the span points at. */
static const char *html_text(Arena *a, const char *p, size_t n) {
    char *buf = (char *)arena_alloc(a, n + 1);
    size_t w = 0;
    bool in_tag = false, space = false;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '<') {
            in_tag = true;
            continue;
        }
        if (c == '>') {
            in_tag = false;
            continue;
        }
        if (in_tag)
            continue;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = w > 0;
            continue;
        }
        if (space && w > 0)
            buf[w++] = ' ';
        space = false;
        buf[w++] = c;
    }
    buf[w] = '\0';
    return w ? buf : NULL;
}

static void sections_html(Sections *out, const char *text, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (text[i] != '<' || lower(text[i + 1]) != 'h')
            continue;
        char d = text[i + 2];
        if (d < '1' || d > '6')
            continue;
        char after = text[i + 3];
        if (after != '>' && after != ' ' && after != '\t' && after != '\n' &&
            after != '\r' && after != '/')
            continue;
        /* The element's own end tag bounds the heading text. A missing one
         * means malformed markup; take the rest of the document rather than
         * refuse to chunk it. */
        size_t j = i + 3;
        while (j < len && text[j] != '>')
            j++;
        size_t body = j < len ? j + 1 : len;
        size_t end = body;
        while (end + 3 < len &&
               !(text[end] == '<' && text[end + 1] == '/' &&
                 lower(text[end + 2]) == 'h' && text[end + 3] == d))
            end++;
        if (end + 3 >= len)
            end = len;
        section_add_level(out, i, html_text(out->a, text + body, end - body),
                          d - '0');
    }
}

/* ---- source code -------------------------------------------------------
 *
 * A top-level declaration starts at column 0 with the brace depth back at
 * zero. Depth is tracked with a scanner that understands strings, character
 * literals and both comment forms, because a '{' inside a string is not a
 * block. The scanner resets string state at every newline: a language whose
 * strings span lines (Python's triple quotes) would otherwise desynchronize
 * the rest of the file, and a slightly over-eager split beats a file that
 * never splits at all.
 */

typedef struct {
    int32_t depth;
    bool in_block_comment;
} CodeScan;

static bool comment_or_attr_line(Str l) {
    size_t i = 0;
    while (i < l.len && (l.ptr[i] == ' ' || l.ptr[i] == '\t'))
        i++;
    if (i >= l.len)
        return true; /* blank */
    char c = l.ptr[i];
    if (c == '#' || c == '*' || c == '@')
        return true;
    if (i + 1 < l.len && c == '/' && (l.ptr[i + 1] == '/' || l.ptr[i + 1] == '*'))
        return true;
    if (i + 1 < l.len && c == '-' && l.ptr[i + 1] == '-')
        return true;
    return false;
}

static bool blank_line(Str l) {
    for (size_t i = 0; i < l.len; i++) {
        if (l.ptr[i] != ' ' && l.ptr[i] != '\t' && l.ptr[i] != '\r')
            return false;
    }
    return true;
}

static void code_scan_line(CodeScan *st, Str l) {
    bool in_str = false, in_chr = false;
    for (size_t i = 0; i < l.len; i++) {
        char c = l.ptr[i];
        if (st->in_block_comment) {
            if (c == '*' && i + 1 < l.len && l.ptr[i + 1] == '/') {
                st->in_block_comment = false;
                i++;
            }
            continue;
        }
        if (in_str || in_chr) {
            if (c == '\\') {
                i++;
            } else if ((in_str && c == '"') || (in_chr && c == '\'')) {
                in_str = in_chr = false;
            }
            continue;
        }
        if (c == '/' && i + 1 < l.len && l.ptr[i + 1] == '/')
            return; /* line comment: nothing after it counts */
        if (c == '#' && i == 0)
            return; /* shell/python comment or preprocessor directive */
        if (c == '/' && i + 1 < l.len && l.ptr[i + 1] == '*') {
            st->in_block_comment = true;
            i++;
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '\'') {
            in_chr = true;
        } else if (c == '{') {
            st->depth++;
        } else if (c == '}') {
            if (st->depth > 0)
                st->depth--;
        }
    }
}

/* A code section's heading is the declaration that opens it: the first line
 * in its span that is neither blank nor a comment. */
static const char *code_heading(Arena *a, const char *text, size_t start,
                                size_t end) {
    const char *first_nonblank = NULL;
    size_t first_nonblank_len = 0;
    size_t i = start;
    while (i < end) {
        size_t j = i;
        while (j < end && text[j] != '\n')
            j++;
        Str line = str_n(text + i, j - i);
        if (!blank_line(line)) {
            if (!comment_or_attr_line(line))
                return trim_copy(a, line.ptr, line.len, 160);
            if (!first_nonblank) {
                first_nonblank = line.ptr;
                first_nonblank_len = line.len;
            }
        }
        i = j + 1;
    }
    return first_nonblank ? trim_copy(a, first_nonblank, first_nonblank_len,
                                      160)
                          : NULL;
}

static void sections_code(Sections *out, const char *text, Lines l) {
    CodeScan st = {0, false};
    /* First line of the current run of blank/comment/attribute lines. A doc
     * comment belongs with the declaration it documents, so a boundary found
     * at a declaration moves back to the top of that run. */
    int32_t run_start = -1;
    int32_t body_lines = 0; /* code lines in the current section so far */

    for (int32_t i = 0; i < l.count; i++) {
        Str line = l.lines[i];
        bool skippable = blank_line(line) || comment_or_attr_line(line);
        /* Column 0 at depth 0 is where a top-level declaration can start.
         * A line opening with a closing delimiter is the tail of the one
         * before it, not the head of a new one. */
        if (!skippable && st.depth == 0 && body_lines > 0 &&
            line.ptr[0] != ' ' && line.ptr[0] != '\t' && line.ptr[0] != '}' &&
            line.ptr[0] != ')' && line.ptr[0] != ']' && line.ptr[0] != ',' &&
            line.ptr[0] != ';') {
            int32_t at = (run_start >= 0) ? run_start : i;
            section_add(out, (size_t)(l.lines[at].ptr - text), NULL);
            body_lines = 0;
        }
        if (skippable) {
            if (run_start < 0)
                run_start = i;
        } else {
            run_start = -1;
            body_lines++;
        }
        code_scan_line(&st, line);
    }
}

/* ---- assembly ---------------------------------------------------------- */

typedef struct {
    Arena *a;
    Chunk *v;
    size_t n, cap;
} ChunkBuf;

static void chunk_add(ChunkBuf *b, size_t start, size_t end,
                      const char *heading, const char *context) {
    if (end <= start)
        return;
    ARENA_GROW(b->a, b->v, b->n, b->cap, Chunk);
    b->v[b->n].start = start;
    b->v[b->n].end = end;
    b->v[b->n].heading = heading;
    b->v[b->n].context = context;
    b->v[b->n].tokens = chunk_tokens_of(end - start);
    b->n++;
}

/* Pulls a window end back to a line break, or failing that a space, when one
 * sits in the last fifth of the window — a chunk that stops mid-word reads
 * badly in a result list and embeds no better. */
static size_t snap_back(const char *text, size_t lo, size_t want) {
    size_t slack = (want - lo) / 5;
    size_t floor_ = want > lo + slack ? want - slack : lo + 1;
    for (size_t i = want; i > floor_; i--) {
        if (text[i - 1] == '\n')
            return i;
    }
    for (size_t i = want; i > floor_; i--) {
        if (text[i - 1] == ' ')
            return i;
    }
    return want;
}

static void window_split(ChunkBuf *b, const char *text, size_t start,
                         size_t end, const char *heading, const char *context,
                         size_t target, size_t overlap) {
    size_t pos = start;
    while (pos < end) {
        if (end - pos <= target) {
            /* The tail always closes on the section's end, so the last span
             * of a document reaches its final byte. */
            chunk_add(b, pos, end, heading, context);
            return;
        }
        size_t cut = snap_back(text, pos, pos + target);
        chunk_add(b, pos, cut, heading, context);
        size_t next = cut > pos + overlap ? cut - overlap : pos + 1;
        if (next <= pos)
            next = pos + 1;
        pos = next;
    }
}

/* Code with a grammar: the chunks syntax_cuts finds, or, when the file has
 * no usable tree, line windows over the whole of it. TypeScript that will not
 * parse is tried as TSX before giving up: a single file filed without a path
 * has only its mime type, and that is the same for both. */
static Chunks split_syntax(Arena *a, const char *text, size_t len, SyntaxLang syn,
                           size_t target_bytes, size_t overlap_bytes) {
    SyntaxCut *cuts;
    size_t n;
    bool ok = syntax_cuts(a, syn, text, len, target_bytes, &cuts, &n);
    if (!ok && syn == SYNTAX_TYPESCRIPT)
        ok = syntax_cuts(a, SYNTAX_TSX, text, len, target_bytes, &cuts, &n);
    ChunkBuf b = {a, NULL, 0, 0};
    if (!ok) {
        window_split(&b, text, 0, len, NULL, NULL, target_bytes, overlap_bytes);
    } else {
        for (size_t i = 0; i < n; i++)
            chunk_add(&b, cuts[i].start, i + 1 < n ? cuts[i + 1].start : len,
                      cuts[i].heading, NULL);
    }
    Chunks out = {b.v, b.n};
    return out;
}

/* "title > context > heading", leaving out empty parts and any that only
 * repeats the one before it (a Markdown file whose first heading is its
 * title). */
char *chunk_header(Arena *a, Lang lang, const char *title, const Chunk *c) {
    (void)lang;
    const char *parts[3] = {title, c->context, c->heading};
    char *out = NULL;
    const char *last = NULL;
    for (int32_t i = 0; i < 3; i++) {
        const char *p = parts[i];
        if (!p || !p[0] || (last && strcmp(p, last) == 0))
            continue;
        /* A context that starts with the title (an H1 that names the
         * document) keeps only what follows it. */
        if (i == 1 && title && strncmp(p, title, strlen(title)) == 0 &&
            strncmp(p + strlen(title), " > ", 3) == 0)
            p += strlen(title) + 3;
        out = out ? arena_printf(a, "%s > %s", out, p) : arena_strdup(a, p);
        last = parts[i];
    }
    return out;
}

Chunks chunk_split(Arena *a, const char *text, size_t len, Lang lang, SyntaxLang syn,
                   size_t target_bytes, size_t overlap_bytes) {
    Chunks out = {NULL, 0};
    if (len == 0)
        return out;
    if (target_bytes == 0)
        target_bytes = 1;
    if (overlap_bytes >= target_bytes)
        overlap_bytes = target_bytes / 2;
    if (lang == LANG_CODE && syn != SYNTAX_NONE)
        return split_syntax(a, text, len, syn, target_bytes, overlap_bytes);

    Sections sec = {a, NULL, 0, 0, {NULL}};
    Lines l = split_lines(a, text, len);
    section_add(&sec, 0, NULL); /* everything before the first heading */
    switch (lang) {
    case LANG_MARKDOWN: sections_markdown(&sec, text, l); break;
    case LANG_HTML: sections_html(&sec, text, len); break;
    case LANG_CODE: sections_code(&sec, text, l); break;
    case LANG_TEXT: break;
    }

    ChunkBuf b = {a, NULL, 0, 0};
    for (size_t i = 0; i < sec.n; i++) {
        size_t start = sec.v[i].start;
        size_t end = (i + 1 < sec.n) ? sec.v[i + 1].start : len;
        const char *heading = sec.v[i].heading;
        /* A code section names itself, so its heading is read off its own
         * span rather than tracked while the boundaries are found. */
        if (lang == LANG_CODE)
            heading = code_heading(a, text, start, end);
        window_split(&b, text, start, end, heading, sec.v[i].context,
                     target_bytes, overlap_bytes);
    }
    out.v = b.v;
    out.n = b.n;
    return out;
}
