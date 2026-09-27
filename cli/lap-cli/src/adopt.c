#include "adopt.h"

#include "diff.h"

/* A parent change, in the branch's current coordinates: it replaced lines
 * [start, start+len) of the branch's version with lines whose count differs
 * from len by delta. */
typedef struct {
    int32_t start;
    int32_t len;
    int32_t delta;
} Change;

/* Lines a[at-1 ..) equal want, blind to CRLF '\r's (a and want both
 * already stripped). */
static bool text_at(Lines a, int32_t at, const Str *want, int32_t n) {
    if (at < 1 || at - 1 + n > a.count)
        return false;
    for (int32_t i = 0; i < n; i++) {
        if (!str_eq(a.lines[at - 1 + i], want[i]))
            return false;
    }
    return true;
}

static Str *strip_text(Arena *a, const Str *v, int32_t n) {
    Lines l = {(Str *)v, n, true};
    return lines_without_cr(a, l).lines;
}

void adopt_place(Arena *a, Lines base, Lines parent, bool parent_has,
                 const Rec *const *commits, int32_t n, Placement *out) {
    memset(out, 0, sizeof *out);
    out->start = (int32_t *)arena_alloc(a, (size_t)(n ? n : 1) *
                                               sizeof(int32_t));
    out->eof_nl = (bool *)arena_alloc(a, (size_t)(n ? n : 1) * sizeof(bool));
    out->already = (bool *)arena_alloc0(a, (size_t)(n ? n : 1) * sizeof(bool));

    Lines branch = base;
    Lines merged = parent;
    bool mexists = parent_has; /* the parent's file, as commits are placed */
    Regions pr = diff_lines(a, lines_without_cr(a, base),
                            lines_without_cr(a, parent));
    Change *ch = (Change *)arena_alloc(
        a, (size_t)(pr.count ? pr.count : 1) * sizeof(Change));
    for (int32_t i = 0; i < pr.count; i++) {
        ch[i].start = pr.v[i].old_start;
        ch[i].len = pr.v[i].old_lines;
        ch[i].delta = pr.v[i].new_lines - pr.v[i].old_lines;
    }

    for (int32_t k = 0; k < n; k++) {
        const Rec *c = commits[k];
        bool del = strcmp(c->op, "delete") == 0;
        int32_t s = c->old_start < 1 ? 1 : c->old_start;
        int32_t e = s + c->old_lines;
        int32_t offset = 0, hits = 0, hit = -1;
        for (int32_t i = 0; i < pr.count; i++) {
            if (ch[i].len < 0)
                continue; /* made common ground by an already-done commit */
            int32_t ps = ch[i].start, pe = ch[i].start + ch[i].len;
            if (s <= pe && ps <= e) {
                if (hits++ == 0)
                    hit = i;
            } else if (pe < s) {
                offset += ch[i].delta;
            }
        }
        int32_t at = s + offset;
        Lines mnocr = lines_without_cr(a, merged);

        /* Existence first: lines alone cannot tell an empty file from a
         * missing one. A delete of a file the parent deleted, or a create
         * of the same lines the parent created, is already done; creating
         * a file the parent has otherwise, or editing one it deleted, is a
         * conflict. */
        bool create = strcmp(c->op, "create") == 0;
        bool both = false;
        if (del && !mexists) {
            both = true;
        } else if (create && mexists) {
            both = merged.count == c->new_n && merged.eof_nl == c->eof_nl &&
                   text_at(mnocr, 1, strip_text(a, c->new_text, c->new_n),
                           c->new_n);
            if (!both) {
                out->why = "both sides create it";
                break;
            }
        } else if (!create && !del && !mexists) {
            out->why = "the parent deleted the file";
            break;
        }
        if (both) {
            branch = del ? (Lines){NULL, 0, true} : merged;
            for (int32_t i = 0; i < pr.count; i++)
                ch[i].len = -1; /* the branch's version is the parent's */
            out->already[k] = true;
            out->start[k] = 1;
            out->eof_nl[k] = merged.eof_nl;
            out->placed = k + 1;
            continue;
        }

        /* The parent made exactly this change: already done. */
        if (!del && hits == 1 && ch[hit].start == s &&
            ch[hit].len == c->old_lines &&
            ch[hit].delta == c->new_n - c->old_lines &&
            text_at(mnocr, at, strip_text(a, c->new_text, c->new_n),
                    c->new_n) &&
            (c->eof_nl == branch.eof_nl || c->eof_nl == merged.eof_nl)) {
            branch = lines_replace(a, branch, s, c->old_lines, c->new_text,
                                   c->new_n, c->eof_nl);
            ch[hit].len = -1;
            for (int32_t i = 0; i < pr.count; i++) {
                if (ch[i].start >= e)
                    ch[i].start += c->new_n - c->old_lines;
            }
            out->already[k] = true;
            out->start[k] = at;
            out->eof_nl[k] = merged.eof_nl;
            out->placed = k + 1;
            continue;
        }

        const char *why = NULL;
        if (hits > 0)
            why = ch[hit].len == 0 && c->old_lines == 0 && ch[hit].start == s
                      ? "both sides insert at the same point"
                      : "it overlaps or touches a change the parent made";
        if (!why &&
            !text_at(mnocr, at, strip_text(a, c->old_text, c->old_n),
                     c->old_n))
            why = "the text it replaces is not the parent's there";
        if (!why && del && at - 1 + c->old_lines != merged.count)
            why = "it deletes a file the parent changed";
        if (why) {
            out->why = why;
            break;
        }

        /* the newline at the end: the commit's, if it changed it; else
         * whatever the parent's version has */
        bool eof = c->eof_nl != branch.eof_nl ? c->eof_nl : merged.eof_nl;
        if (del) {
            merged.lines = NULL;
            merged.count = 0;
            merged.eof_nl = true;
            branch = merged;
            eof = true;
        } else {
            merged = lines_replace(a, merged, at, c->old_lines, c->new_text,
                                   c->new_n, eof);
            branch = lines_replace(a, branch, s, c->old_lines, c->new_text,
                                   c->new_n, c->eof_nl);
        }
        mexists = !del;
        for (int32_t i = 0; i < pr.count; i++) {
            if (ch[i].start >= e)
                ch[i].start += c->new_n - c->old_lines;
        }
        out->start[k] = at;
        out->eof_nl[k] = eof;
        out->placed = k + 1;
    }
    out->result = merged;
}
