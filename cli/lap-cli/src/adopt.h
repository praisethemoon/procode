/* Adoption: placing a branch's commits to one file on the parent's version
 * of that file (SPEC.md, Merging). Pure: no I/O, and nothing allocated
 * outside the caller's arena.
 *
 * Three versions of the file take part. The base is the file as of the
 * branch's base; the parent is the file as the parent has it now; the
 * branch's commits, in order, turn the base into the branch's version,
 * each region given in the branch's coordinates at its point.
 *
 * The parent's changes are the diff base -> parent. A branch commit whose
 * region neither overlaps nor touches one of them moves by the net lines
 * the parent added or removed above it; every placed commit shifts the
 * ones after it. Overlapping or touching a parent change is a conflict
 * (both sides inserting at one point, or one at the edge of the other's
 * change: either order would be a guess), and so is a commit whose old
 * text is not the parent's text where it would land. The first conflict
 * stops the file: no later commit to it is placed.
 *
 * One exception: a commit whose region is exactly one parent change, and
 * whose new text is the parent's text there, is already done — the parent
 * made the same change. Nothing is placed for it and nothing stops; that
 * parent change is then common ground for the commits after it. A file
 * deleted on both sides, or created on both with the same lines, is this
 * case too. So is a run of consecutive edits that stay inside one parent
 * change and, together, leave the parent's text there: a change both sides
 * made in several steps. Near is not identical: one line different, or a
 * run that stops short of the parent's text, is a conflict.
 */
#ifndef LAP_ADOPT_H
#define LAP_ADOPT_H

#include "rec.h"

typedef struct {
    int32_t placed;  /* commits placed, counted from the first; those
                        already done count as placed */
    bool *already;   /* commit k: the parent had already made exactly its
                        change, so nothing of it is to be adopted */
    int32_t *start;  /* placed commit k: its start in the parent's version
                        as it stood just before it */
    bool *eof_nl;    /* placed commit k: the trailing-newline state after
                        it, in the parent's version */
    Lines result;    /* the parent's version with the placed commits */
    const char *why; /* when placed < n: why the next one was not */
} Placement;

/* Places commits[0..n) (commit records to one file) on parent, given the
 * file at the branch's base; parent_has says whether the parent has the
 * file at all, which its lines cannot (an empty file is no lines too).
 * Lines are compared as lap compares them, blind to a CRLF line ending's
 * '\r'. */
void adopt_place(Arena *a, Lines base, Lines parent, bool parent_has,
                 const Rec *const *commits, int32_t n, Placement *out);

#endif /* LAP_ADOPT_H */
