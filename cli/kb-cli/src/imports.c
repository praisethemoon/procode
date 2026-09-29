#include "imports.h"

#include "cmd.h"
#include "doc.h"

#include <stdlib.h>
#include <string.h>

/* ---- paths inside the folder -------------------------------------------- */

/* `rel` joined onto `dir` (both folder-relative, '/'-separated; "" is the
 * root), with "." and ".." worked out. NULL when it would climb out of the
 * folder, or names the folder itself. */
static const char *path_join(Arena *a, const char *dir, const char *rel) {
    const char *full = dir[0] ? arena_printf(a, "%s/%s", dir, rel) : rel;
    size_t n = strlen(full);
    const char **seg = (const char **)arena_alloc(a, (n / 2 + 2) * sizeof(char *));
    size_t *segn = (size_t *)arena_alloc(a, (n / 2 + 2) * sizeof(size_t));
    size_t k = 0;
    for (const char *p = full; *p;) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 0 || (len == 1 && p[0] == '.')) {
            /* nothing */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (k == 0)
                return NULL;
            k--;
        } else {
            seg[k] = p;
            segn[k++] = len;
        }
        p += len + (e ? 1 : 0);
    }
    if (k == 0)
        return NULL;
    StrBuf sb;
    sb_init(&sb, a);
    for (size_t i = 0; i < k; i++) {
        if (i)
            sb_putc(&sb, '/');
        sb_putn(&sb, seg[i], segn[i]);
    }
    return sb_finish(&sb);
}

/* The folder a file is in: "" for one at the root. */
static const char *dir_of(Arena *a, const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? arena_strndup(a, path, (size_t)(slash - path)) : "";
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && strcmp(s + n - k, suffix) == 0;
}

/* `path` if it is a filed file, else NULL. */
static const char *found(const char *path, ImportExists exists, void *ud) {
    return path && exists(ud, path) ? path : NULL;
}

/* ---- per language ------------------------------------------------------- */

static const char *resolve_c(Arena *a, const char *from, const SyntaxImport *imp,
                             ImportExists exists, void *ud) {
    const char *beside = path_join(a, dir_of(a, from), imp->spec);
    const char *root = path_join(a, "", imp->spec);
    const char *first = imp->system ? root : beside;
    const char *then = imp->system ? beside : root;
    const char *hit = found(first, exists, ud);
    return hit ? hit : found(then, exists, ud);
}

static const char *const JS_EXTS[] = {".ts",  ".tsx", ".d.ts", ".js",  ".jsx",
                                      ".mjs", ".cjs", ".mts",  ".cts", NULL};

static const char *resolve_js(Arena *a, const char *from, const SyntaxImport *imp,
                              ImportExists exists, void *ud) {
    const char *spec = imp->spec;
    if (strncmp(spec, "./", 2) != 0 && strncmp(spec, "../", 3) != 0)
        return NULL; /* a package name, or an absolute path: not the folder's */
    const char *base = path_join(a, dir_of(a, from), spec);
    if (!base)
        return NULL;
    const char *hit = found(base, exists, ud);
    for (int32_t i = 0; !hit && JS_EXTS[i]; i++)
        hit = found(arena_printf(a, "%s%s", base, JS_EXTS[i]), exists, ud);
    for (int32_t i = 0; !hit && JS_EXTS[i]; i++)
        hit = found(arena_printf(a, "%s/index%s", base, JS_EXTS[i]), exists, ud);
    /* TypeScript written for ES modules imports "./x.js" from x.ts. */
    static const char *const EMITTED[][2] = {
        {".js", ".ts"}, {".js", ".tsx"}, {".jsx", ".tsx"}, {".mjs", ".mts"}, {".cjs", ".cts"}};
    for (size_t i = 0; !hit && i < sizeof EMITTED / sizeof EMITTED[0]; i++) {
        if (!ends_with(base, EMITTED[i][0]))
            continue;
        size_t stem = strlen(base) - strlen(EMITTED[i][0]);
        hit = found(arena_printf(a, "%.*s%s", (int)stem, base, EMITTED[i][1]), exists, ud);
    }
    return hit;
}

/* "a.b.c" as "a/b/c". */
static const char *dotted_path(Arena *a, const char *dotted) {
    char *p = arena_strdup(a, dotted);
    for (char *q = p; *q; q++)
        if (*q == '.')
            *q = '/';
    return p;
}

/* A module or a package at `dir`/`rest`. */
static const char *py_module(Arena *a, const char *dir, const char *rest,
                             ImportExists exists, void *ud) {
    if (!rest[0])
        return found(path_join(a, dir, "__init__.py"), exists, ud);
    const char *hit = found(path_join(a, dir, arena_printf(a, "%s.py", rest)), exists, ud);
    if (!hit)
        hit = found(path_join(a, dir, arena_printf(a, "%s/__init__.py", rest)), exists, ud);
    if (!hit)
        hit = found(path_join(a, dir, arena_printf(a, "%s.pyi", rest)), exists, ud);
    return hit;
}

static const char *resolve_py(Arena *a, const char *from, const SyntaxImport *imp,
                              ImportExists exists, void *ud) {
    const char *spec = imp->spec;
    const char *dir = dir_of(a, from);
    if (spec[0] == '.') {
        size_t dots = strspn(spec, ".");
        for (size_t up = 1; up < dots; up++) {
            if (!dir[0])
                return NULL; /* above the folder */
            dir = dir_of(a, dir);
        }
        return py_module(a, dir, dotted_path(a, spec + dots), exists, ud);
    }
    /* Absolute: from the file's own folder up to the root, so a src/ layout
     * or a package inside a larger folder is found where it sits. */
    const char *rest = dotted_path(a, spec);
    for (;;) {
        const char *hit = py_module(a, dir, rest, exists, ud);
        if (hit || !dir[0])
            return hit;
        dir = dir_of(a, dir);
    }
}

const char *import_resolve(Arena *a, SyntaxLang l, const char *from,
                           const SyntaxImport *imp, ImportExists exists,
                           void *ud) {
    if (!imp->spec || !imp->spec[0])
        return NULL;
    switch (l) {
    case SYNTAX_C:
        return resolve_c(a, from, imp, exists, ud);
    case SYNTAX_TYPESCRIPT:
    case SYNTAX_TSX:
    case SYNTAX_JAVASCRIPT:
        return resolve_js(a, from, imp, exists, ud);
    case SYNTAX_PYTHON:
        return resolve_py(a, from, imp, exists, ud);
    default:
        return NULL;
    }
}

/* ---- keeping the links -------------------------------------------------- */

static int32_t cmp_str(const void *x, const void *y) {
    return strcmp(*(const char *const *)x, *(const char *const *)y);
}

static bool has(const char *const *sorted, size_t n, const char *s) {
    return n && bsearch(&s, sorted, n, sizeof(char *), cmp_str) != NULL;
}

static int32_t cmp_doc_path(const void *x, const void *y) {
    return strcmp((*(const Document *const *)x)->path, (*(const Document *const *)y)->path);
}

/* The folder's documents, by path, for resolving. */
typedef struct {
    const char **paths;
    const char **ids; /* in the order of paths */
    size_t n;
} Folder;

static bool folder_exists(void *ud, const char *path) {
    const Folder *f = (const Folder *)ud;
    return has(f->paths, f->n, path);
}

static const char *folder_id(const Folder *f, const char *path) {
    const char **hit = (const char **)bsearch(&path, f->paths, f->n, sizeof(char *), cmp_str);
    return hit ? f->ids[hit - f->paths] : NULL;
}

/* An edge as one string, "from\tto", so the wanted and the existing sets
 * are sorted string arrays compared by merging. */
static const char *edge(Arena *a, const char *from, const char *to) {
    return arena_printf(a, "%s\t%s", from, to);
}

static size_t unique(const char **v, size_t n) {
    qsort(v, n, sizeof(char *), cmp_str);
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (k == 0 || strcmp(v[k - 1], v[i]) != 0)
            v[k++] = v[i];
    return k;
}

static bool write_edge(Arena *a, Store *s, const char *key, bool present,
                       const char *now, char *err, size_t errsz) {
    const char *tab = strchr(key, '\t');
    Link l;
    l.from = arena_strndup(a, key, (size_t)(tab - key));
    l.rel = LINK_IMPORTS;
    l.to = tab + 1;
    l.created_at = now;
    size_t len;
    char *line = doc_encode_link(a, &l, present, &len);
    return store_append(s, STORE_DOCUMENTS, line, len, err, errsz);
}

bool imports_sync(Arena *a, Store *s, const char *source_id,
                  const DirFile *files, size_t nfiles,
                  const char *const *gone, size_t ngone, size_t *linked,
                  size_t *unlinked, char *err, size_t errsz) {
    *linked = *unlinked = 0;
    if (!source_id)
        return true;
    /* The folder's documents by path, missing files' included: an import of
     * a file kept with --no-forget still names a document. */
    Folder f = {NULL, NULL, 0};
    const Document **docs = (const Document **)arena_alloc(
        a, (s->documents.n ? s->documents.n : 1) * sizeof(Document *));
    for (size_t i = 0; i < s->documents.n; i++)
        if (strcmp(s->documents.v[i].source, source_id) == 0 && s->documents.v[i].path[0])
            docs[f.n++] = &s->documents.v[i];
    qsort(docs, f.n, sizeof(Document *), cmp_doc_path);
    f.paths = (const char **)arena_alloc(a, (f.n ? f.n : 1) * sizeof(char *));
    f.ids = (const char **)arena_alloc(a, (f.n ? f.n : 1) * sizeof(char *));
    for (size_t i = 0; i < f.n; i++) {
        f.paths[i] = docs[i]->path;
        f.ids[i] = docs[i]->id;
    }

    /* What the files' code calls for. */
    const char **want = NULL;
    size_t nwant = 0, capwant = 0;
    const char **scanned = (const char **)arena_alloc(a, (nfiles ? nfiles : 1) * sizeof(char *));
    size_t nscanned = 0;
    for (size_t i = 0; i < nfiles; i++) {
        const DirFile *df = &files[i];
        const char *from_id = folder_id(&f, df->rel);
        if (!from_id)
            continue;
        scanned[nscanned++] = from_id;
        SyntaxLang l = doc_syntax(df->mime, df->rel);
        SyntaxImport *imps;
        size_t nimp;
        if (l == SYNTAX_NONE || !syntax_imports(a, l, df->content, df->len, &imps, &nimp))
            continue;
        for (size_t k = 0; k < nimp; k++) {
            const char *to = import_resolve(a, l, df->rel, &imps[k], folder_exists, &f);
            const char *to_id = to ? folder_id(&f, to) : NULL;
            if (!to_id || strcmp(to_id, from_id) == 0)
                continue;
            ARENA_GROW(a, want, nwant, capwant, const char *);
            want[nwant++] = edge(a, from_id, to_id);
        }
    }
    nwant = unique(want, nwant);
    qsort(scanned, nscanned, sizeof(char *), cmp_str);
    const char **gone_sorted = (const char **)arena_alloc(a, (ngone ? ngone : 1) * sizeof(char *));
    for (size_t i = 0; i < ngone; i++)
        gone_sorted[i] = gone[i];
    qsort(gone_sorted, ngone, sizeof(char *), cmp_str);

    /* What is there now: the imports links out of the files just read, and
     * any into or out of a document just forgotten. */
    const char **have = NULL;
    size_t nhave = 0, caphave = 0;
    for (size_t i = 0; i < s->documents.nlinks; i++) {
        const Link *lk = &s->documents.links[i];
        if (strcmp(lk->rel, LINK_IMPORTS) != 0)
            continue;
        if (!has(scanned, nscanned, lk->from) && !has(gone_sorted, ngone, lk->from) &&
            !has(gone_sorted, ngone, lk->to))
            continue;
        ARENA_GROW(a, have, nhave, caphave, const char *);
        have[nhave++] = edge(a, lk->from, lk->to);
    }
    nhave = unique(have, nhave);

    char now[32];
    plat_timestamp(now);
    size_t i = 0, j = 0;
    while (i < nwant || j < nhave) {
        int32_t c = i == nwant ? 1 : j == nhave ? -1 : strcmp(want[i], have[j]);
        if (c == 0) {
            i++;
            j++;
        } else if (c < 0) {
            if (!write_edge(a, s, want[i++], true, now, err, errsz))
                return false;
            (*linked)++;
        } else {
            if (!write_edge(a, s, have[j++], false, now, err, errsz))
                return false;
            (*unlinked)++;
        }
    }
    return true;
}
