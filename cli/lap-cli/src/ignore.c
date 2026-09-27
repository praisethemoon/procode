#include "ignore.h"

#include "platform.h"
#include "str.h"

typedef struct {
    const char *pat; /* normalized: no leading '/', no trailing '/' */
    bool dir_only;
    bool anchored; /* contains '/', match from root; else basename match */
} Pattern;

struct Ignore {
    Pattern *v;
    size_t n, cap;
};

/* ".git" without the slash: in a git worktree it is a file pointing at the
 * main checkout's repository, and it is git's, not the project's. */
static const char *DEFAULTS[] = {".lap/", ".git", ".hg/", ".svn/",
                                 ".DS_Store"};

/* Segment glob: '*' any run (no '/'), '?' one char. */
static bool seg_match(const char *pat, size_t plen, const char *s,
                      size_t slen) {
    size_t pi = 0, si = 0;
    size_t star_pi = (size_t)-1, star_si = 0;
    while (si < slen) {
        if (pi < plen && (pat[pi] == s[si] || pat[pi] == '?')) {
            pi++;
            si++;
        } else if (pi < plen && pat[pi] == '*') {
            star_pi = pi++;
            star_si = si;
        } else if (star_pi != (size_t)-1) {
            pi = star_pi + 1;
            si = ++star_si;
        } else {
            return false;
        }
    }
    while (pi < plen && pat[pi] == '*')
        pi++;
    return pi == plen;
}

/* Splits s on '/' into at most max segments; returns count. */
static int split_segs(const char *s, Str *out, int max) {
    int n = 0;
    const char *p = s;
    while (*p && n < max) {
        const char *slash = strchr(p, '/');
        if (!slash) {
            out[n++] = str_c(p);
            break;
        }
        out[n++] = str_n(p, (size_t)(slash - p));
        p = slash + 1;
    }
    return n;
}

#define MAX_SEGS 128

/* Matches '/'-separated pattern segments (with '**') against path segments. */
static bool segs_match(const Str *ps, int pn, const Str *ss, int sn) {
    if (pn == 0)
        return sn == 0;
    if (str_eq_c(ps[0], "**")) {
        /* '**' swallows 0..sn leading segments */
        for (int skip = 0; skip <= sn; skip++) {
            if (segs_match(ps + 1, pn - 1, ss + skip, sn - skip))
                return true;
        }
        return false;
    }
    if (sn == 0)
        return false;
    if (!seg_match(ps[0].ptr, ps[0].len, ss[0].ptr, ss[0].len))
        return false;
    return segs_match(ps + 1, pn - 1, ss + 1, sn - 1);
}

bool ignore_match_pattern(const char *pattern, const char *relpath,
                          bool is_dir) {
    /* normalize pattern */
    size_t plen = strlen(pattern);
    bool dir_only = false;
    if (plen > 0 && pattern[plen - 1] == '/') {
        dir_only = true;
        plen--;
    }
    const char *pp = pattern;
    bool anchored = false;
    if (plen > 0 && pp[0] == '/') {
        pp++;
        plen--;
        anchored = true; /* a leading '/' anchors even a slash-free pattern */
    }
    if (plen == 0)
        return false;
    if (dir_only && !is_dir)
        return false;

    char pbuf[LAP_PATH_MAX];
    if (plen >= sizeof pbuf)
        return false;
    memcpy(pbuf, pp, plen);
    pbuf[plen] = '\0';

    if (strchr(pbuf, '/') != NULL)
        anchored = true;

    Str psegs[MAX_SEGS], ssegs[MAX_SEGS];
    int sn = split_segs(relpath, ssegs, MAX_SEGS);
    if (sn == 0)
        return false;

    if (!anchored) {
        /* basename match at any depth */
        Str base = ssegs[sn - 1];
        return seg_match(pbuf, plen, base.ptr, base.len);
    }
    int pn = split_segs(pbuf, psegs, MAX_SEGS);
    return segs_match(psegs, pn, ssegs, sn);
}

static void add_pattern(Arena *a, Ignore *ig, const char *line, size_t len) {
    /* trim */
    while (len > 0 && (line[0] == ' ' || line[0] == '\t')) {
        line++;
        len--;
    }
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
                       line[len - 1] == '\r')) {
        len--;
    }
    if (len == 0 || line[0] == '#' || line[0] == '!')
        return;
    Pattern p;
    p.pat = arena_strndup(a, line, len);
    p.dir_only = len > 0 && line[len - 1] == '/';
    p.anchored = false; /* recomputed at match time by ignore_match_pattern */
    ARENA_GROW(a, ig->v, ig->n, ig->cap, Pattern);
    ig->v[ig->n++] = p;
}

Ignore *ignore_load(Arena *a, const char *repo_root) {
    Ignore *ig = (Ignore *)arena_alloc0(a, sizeof(Ignore));
    for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++)
        add_pattern(a, ig, DEFAULTS[i], strlen(DEFAULTS[i]));
    char path[LAP_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", repo_root, LAP_IGNORE_NAME);
    char *data;
    size_t len;
    if (plat_read_file(a, path, &data, &len)) {
        Lines l = split_lines(a, data, len);
        for (int i = 0; i < l.count; i++)
            add_pattern(a, ig, l.lines[i].ptr, l.lines[i].len);
    }
    return ig;
}

bool ignore_match(const Ignore *ig, const char *relpath, bool is_dir) {
    for (size_t i = 0; i < ig->n; i++) {
        if (ignore_match_pattern(ig->v[i].pat, relpath, is_dir))
            return true;
    }
    if (!is_dir) {
        /* a file inside an ignored directory is ignored */
        char buf[LAP_PATH_MAX];
        size_t n = strlen(relpath);
        if (n < sizeof buf) {
            memcpy(buf, relpath, n + 1);
            for (size_t i = n; i > 0; i--) {
                if (buf[i - 1] == '/') {
                    buf[i - 1] = '\0';
                    for (size_t j = 0; j < ig->n; j++) {
                        if (ignore_match_pattern(ig->v[j].pat, buf, true))
                            return true;
                    }
                }
            }
        }
    }
    return false;
}
