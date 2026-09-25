#include "diff.h"

/* Myers O(ND) with full trace for backtracking, capped: beyond DIFF_D_CAP
 * edit steps the whole changed span collapses into one region. A rewrite
 * that large is "one big change" for lap's purposes anyway.
 */
#define DIFF_D_CAP 1024

typedef struct {
    Region *v;
    int32_t n;
    size_t cap;
} RegionList;

static void region_push(Arena *a, RegionList *rl, Region r) {
    size_t n = (size_t)rl->n;
    ARENA_GROW(a, rl->v, n, rl->cap, Region);
    rl->v[rl->n++] = r;
}

static bool line_eq(const Lines *A, const uint64_t *ha, int32_t i,
                    const Lines *B, const uint64_t *hb, int32_t j) {
    return ha[i] == hb[j] && str_eq(A->lines[i], B->lines[j]);
}

/* Emits regions for the core span A[p, p+N) vs B[p2, p2+M) given an op
 * string ('M' match, 'D' delete-from-old, 'I' insert-from-new) of the
 * canonical edit script. p/p2 are 0-based offsets; regions are 1-based.
 */
static void ops_to_regions(Arena *a, RegionList *rl, const char *ops,
                           int32_t nops, int32_t p, int32_t p2) {
    int32_t oi = p, ni = p2;
    bool open = false;
    Region cur = {0, 0, 0, 0};
    for (int32_t k = 0; k < nops; k++) {
        char op = ops[k];
        if (op == 'M') {
            if (open) {
                region_push(a, rl, cur);
                open = false;
            }
            oi++;
            ni++;
        } else {
            if (!open) {
                cur.old_start = oi + 1;
                cur.new_start = ni + 1;
                cur.old_lines = 0;
                cur.new_lines = 0;
                open = true;
            }
            if (op == 'D') {
                cur.old_lines++;
                oi++;
            } else { /* 'I' */
                cur.new_lines++;
                ni++;
            }
        }
    }
    if (open)
        region_push(a, rl, cur);
}

/* Myers diff of A[p, p+N) vs B[p2, p2+M). Returns false when the cap was
 * exceeded (caller emits one collapsed region).
 */
static bool myers(Arena *a, RegionList *rl, const Lines *A, const uint64_t *ha,
                  int32_t p, int32_t N, const Lines *B, const uint64_t *hb,
                  int32_t p2, int32_t M) {
    int32_t dmax = N + M;
    if (dmax > DIFF_D_CAP)
        dmax = DIFF_D_CAP;
    int32_t vsize = 2 * dmax + 1;
    int32_t off = dmax;
    int32_t *V = (int32_t *)arena_alloc(a, (size_t)vsize * sizeof(int32_t));
    int32_t **trace =
        (int32_t **)arena_alloc(a, (size_t)(dmax + 1) * sizeof(int32_t *));
    V[off + 1] = 0;
    int32_t dfound = -1;
    for (int32_t d = 0; d <= dmax; d++) {
        for (int32_t k = -d; k <= d; k += 2) {
            int32_t x;
            if (k == -d || (k != d && V[off + k - 1] < V[off + k + 1]))
                x = V[off + k + 1];
            else
                x = V[off + k - 1] + 1;
            int32_t y = x - k;
            while (x < N && y < M && line_eq(A, ha, p + x, B, hb, p2 + y)) {
                x++;
                y++;
            }
            V[off + k] = x;
            if (x >= N && y >= M)
                dfound = d;
        }
        trace[d] = (int32_t *)arena_alloc(a, (size_t)vsize * sizeof(int32_t));
        memcpy(trace[d], V, (size_t)vsize * sizeof(int32_t));
        if (dfound >= 0)
            break;
    }
    if (dfound < 0)
        return false;

    /* Backtrack from (N, M), building the op script in reverse. */
    char *rops = (char *)arena_alloc(a, (size_t)(N + M) + 1);
    int32_t nops = 0;
    int32_t x = N, y = M;
    for (int32_t d = dfound; d > 0; d--) {
        const int32_t *Vp = trace[d - 1];
        int32_t k = x - y;
        bool down = (k == -d) || (k != d && Vp[off + k - 1] < Vp[off + k + 1]);
        int32_t prev_k = down ? k + 1 : k - 1;
        int32_t prev_x = Vp[off + prev_k];
        int32_t prev_y = prev_x - prev_k;
        int32_t mid_x = down ? prev_x : prev_x + 1;
        while (x > mid_x) {
            rops[nops++] = 'M';
            x--;
            y--;
        }
        rops[nops++] = down ? 'I' : 'D';
        x = prev_x;
        y = prev_y;
    }
    while (x > 0) { /* d == 0: leading snake is all matches */
        rops[nops++] = 'M';
        x--;
        y--;
    }

    /* reverse in place */
    for (int32_t i = 0, j = nops - 1; i < j; i++, j--) {
        char t = rops[i];
        rops[i] = rops[j];
        rops[j] = t;
    }
    ops_to_regions(a, rl, rops, nops, p, p2);
    return true;
}

static bool line_is_blank(Str s) {
    for (size_t i = 0; i < s.len; i++) {
        char c = s.ptr[i];
        if (c != ' ' && c != '\t' && c != '\r')
            return false;
    }
    return true;
}

/* True when old lines [start, end] (1-based, inclusive; empty range ok)
 * are all blank. */
static bool span_is_blank(const Lines *l, int32_t start, int32_t end) {
    for (int32_t i = start; i <= end; i++) {
        if (i < 1 || i > l->count)
            return false;
        if (!line_is_blank(l->lines[i - 1]))
            return false;
    }
    return true;
}

/* Blank lines carry no identity, so a diff alignment that anchors on them is
 * coincidence, not structure: a gap of unchanged lines splits two edits only
 * if it contains at least one NON-BLANK line. Adjacent regions whose gap is
 * blank-only merge into one region that spans the gap (the blank lines ride
 * along unchanged in old_text/new_text, keeping replay exact).
 */
static void merge_blank_gaps(const Lines *oldl, RegionList *rl) {
    if (rl->n < 2)
        return;
    int32_t w = 0;
    for (int32_t i = 1; i < rl->n; i++) {
        Region *prev = &rl->v[w];
        Region *cur = &rl->v[i];
        int32_t gap_start = prev->old_start + prev->old_lines;
        int32_t gap_end = cur->old_start - 1;
        if (span_is_blank(oldl, gap_start, gap_end)) {
            prev->old_lines = cur->old_start + cur->old_lines -
                              prev->old_start;
            prev->new_lines = cur->new_start + cur->new_lines -
                              prev->new_start;
        } else {
            rl->v[++w] = *cur;
        }
    }
    rl->n = w + 1;
}

Regions diff_lines(Arena *a, Lines oldl, Lines newl) {
    Regions out = {NULL, 0, false};
    RegionList rl = {NULL, 0, 0};
    int32_t n = oldl.count, m = newl.count;

    uint64_t *ha =
        (uint64_t *)arena_alloc(a, (size_t)(n ? n : 1) * sizeof(uint64_t));
    uint64_t *hb =
        (uint64_t *)arena_alloc(a, (size_t)(m ? m : 1) * sizeof(uint64_t));
    for (int32_t i = 0; i < n; i++)
        ha[i] = str_hash(oldl.lines[i]);
    for (int32_t i = 0; i < m; i++)
        hb[i] = str_hash(newl.lines[i]);

    /* trim common prefix */
    int32_t p = 0;
    while (p < n && p < m && line_eq(&oldl, ha, p, &newl, hb, p))
        p++;
    /* trim common suffix (not overlapping the prefix) */
    int32_t s = 0;
    while (s < n - p && s < m - p &&
           line_eq(&oldl, ha, n - 1 - s, &newl, hb, m - 1 - s))
        s++;

    int32_t N = n - p - s;
    int32_t M = m - p - s;

    if (N == 0 && M == 0) {
        /* content identical */
    } else if (N == 0 || M == 0) {
        /* pure insertion or pure deletion: one contiguous region */
        Region r;
        r.old_start = p + 1;
        r.old_lines = N;
        r.new_start = p + 1;
        r.new_lines = M;
        region_push(a, &rl, r);
    } else {
        if (!myers(a, &rl, &oldl, ha, p, N, &newl, hb, p, M)) {
            Region r;
            r.old_start = p + 1;
            r.old_lines = N;
            r.new_start = p + 1;
            r.new_lines = M;
            region_push(a, &rl, r);
            out.truncated = true;
        }
    }

    /* Trailing-newline change: attach to the last line if no region already
     * covers it. */
    if (oldl.eof_nl != newl.eof_nl && n > 0 && m > 0) {
        bool covered = false;
        if (rl.n > 0) {
            Region *last = &rl.v[rl.n - 1];
            int32_t old_end = last->old_start + last->old_lines - 1;
            int32_t new_end = last->new_start + last->new_lines - 1;
            if (old_end >= n || new_end >= m)
                covered = true;
        }
        if (!covered) {
            Region r;
            r.old_start = n;
            r.old_lines = 1;
            r.new_start = m;
            r.new_lines = 1;
            region_push(a, &rl, r);
        }
    }

    merge_blank_gaps(&oldl, &rl);

    out.v = rl.v;
    out.count = rl.n;
    return out;
}

Lines lines_replace(Arena *a, Lines base, int32_t old_start,
                    int32_t old_lines, const Str *repl, int32_t repl_count,
                    bool eof_nl_after) {
    if (old_start < 1 || old_lines < 0 || repl_count < 0 ||
        old_start - 1 + old_lines > base.count)
        return base;
    int32_t newcount = base.count - old_lines + repl_count;
    Lines out;
    out.count = newcount;
    out.eof_nl = eof_nl_after;
    out.lines =
        (Str *)arena_alloc(a, (size_t)(newcount ? newcount : 1) * sizeof(Str));
    int32_t w = 0;
    for (int32_t i = 0; i < old_start - 1; i++)
        out.lines[w++] = base.lines[i];
    for (int32_t i = 0; i < repl_count; i++)
        out.lines[w++] = repl[i];
    for (int32_t i = old_start - 1 + old_lines; i < base.count; i++)
        out.lines[w++] = base.lines[i];
    return out;
}
