#include "gitignore.h"

#include "platform.h"

#include <string.h>

typedef struct {
    const char *base; /* the .gitignore's directory, relative to the top */
    const char *pat;  /* no leading `/`, no trailing `/`, no `!` */
    bool negate, dir_only, anchored;
} Rule;

struct GitIgnore {
    Arena *a;
    Rule *v;
    size_t n, cap;
};

GitIgnore *gitignore_new(Arena *a) {
    GitIgnore *g = (GitIgnore *)arena_alloc0(a, sizeof(GitIgnore));
    g->a = a;
    return g;
}

/* ---- matching ----------------------------------------------------------- */

/* A `[...]` class at p (p[0] == '['), against c. Returns 1 or 0 for a match
 * or not and sets *end past the `]`; -1 when the class never closes, in which
 * case the `[` is an ordinary character. */
static int32_t class_match(const char *p, char c, const char **end) {
    const char *q = p + 1;
    bool negate = *q == '!' || *q == '^';
    if (negate)
        q++;
    bool hit = false;
    bool first = true;
    while (*q && (first || *q != ']')) {
        first = false;
        char lo = *q;
        if (lo == '\\' && q[1])
            lo = *++q;
        char hi = lo;
        if (q[1] == '-' && q[2] && q[2] != ']') {
            hi = q[2];
            q += 2;
            if (hi == '\\' && q[1])
                hi = *++q;
        }
        if ((unsigned char)c >= (unsigned char)lo &&
            (unsigned char)c <= (unsigned char)hi)
            hit = true;
        q++;
    }
    if (*q != ']')
        return -1;
    *end = q + 1;
    return (hit != negate) ? 1 : 0;
}

static bool wm(const char *p0, const char *p, const char *s) {
    while (*p) {
        if (*p == '*') {
            /* `**` as a whole segment: any number of directories. */
            if (p[1] == '*' && (p == p0 || p[-1] == '/') &&
                (p[2] == '/' || p[2] == '\0')) {
                if (p[2] == '\0')
                    return true;
                const char *q = p + 3;
                for (const char *t = s;;) {
                    if (wm(p0, q, t))
                        return true;
                    t = strchr(t, '/');
                    if (!t)
                        return false;
                    t++;
                }
            }
            while (*p == '*')
                p++;
            for (const char *t = s;; t++) {
                if (wm(p0, p, t))
                    return true;
                if (!*t || *t == '/')
                    return false;
            }
        }
        if (!*s)
            return false;
        if (*p == '?') {
            if (*s == '/')
                return false;
            p++;
            s++;
            continue;
        }
        if (*p == '[') {
            /* A class never matches `/`, and neither does a literal `[`. */
            if (*s == '/')
                return false;
            const char *end;
            int32_t r = class_match(p, *s, &end);
            if (r == 0)
                return false;
            if (r == 1) {
                p = end;
                s++;
                continue;
            }
            /* An unclosed `[` is itself. */
        } else if (*p == '\\' && p[1]) {
            p++;
        }
        if (*p != *s)
            return false;
        p++;
        s++;
    }
    return *s == '\0';
}

bool gitignore_wildmatch(const char *pattern, const char *path) {
    return wm(pattern, pattern, path);
}

bool gitignore_match(const GitIgnore *g, const char *path, bool is_dir) {
    bool ignored = false;
    for (size_t i = 0; i < g->n; i++) {
        const Rule *r = &g->v[i];
        if (r->dir_only && !is_dir)
            continue;
        const char *rel = path;
        size_t bl = strlen(r->base);
        if (bl) {
            if (strncmp(path, r->base, bl) != 0 || path[bl] != '/')
                continue;
            rel = path + bl + 1;
        }
        const char *subject = rel;
        if (!r->anchored) {
            const char *slash = strrchr(rel, '/');
            if (slash)
                subject = slash + 1;
        }
        if (gitignore_wildmatch(r->pat, subject))
            ignored = !r->negate;
    }
    return ignored;
}

/* ---- reading ------------------------------------------------------------ */

void gitignore_add(GitIgnore *g, const char *base, const char *text, size_t len) {
    const char *b = arena_strdup(g->a, base);
    for (const char *p = text; p < text + len;) {
        const char *nl = memchr(p, '\n', (size_t)(text + len - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(text + len - p);
        const char *line = p;
        p = nl ? nl + 1 : text + len;
        if (n && line[n - 1] == '\r')
            n--;
        /* Trailing spaces go, unless the last one is escaped. */
        while (n && line[n - 1] == ' ' && !(n >= 2 && line[n - 2] == '\\'))
            n--;
        if (n == 0 || line[0] == '#')
            continue;
        Rule r;
        memset(&r, 0, sizeof r);
        r.base = b;
        if (line[0] == '!') {
            r.negate = true;
            line++;
            n--;
        }
        if (n && line[n - 1] == '/') {
            r.dir_only = true;
            n--;
        }
        if (memchr(line, '/', n))
            r.anchored = true;
        if (n && line[0] == '/') {
            line++;
            n--;
        }
        if (n == 0)
            continue;
        r.pat = arena_strndup(g->a, line, n);
        ARENA_GROW(g->a, g->v, g->n, g->cap, Rule);
        g->v[g->n++] = r;
    }
}

void gitignore_load(GitIgnore *g, const char *top, const char *base) {
    char *path = base[0] ? arena_printf(g->a, "%s/%s/.gitignore", top, base)
                         : arena_printf(g->a, "%s/.gitignore", top);
    char *text;
    size_t len;
    if (plat_is_file(path) && plat_read_file(g->a, path, &text, &len))
        gitignore_add(g, base, text, len);
}
