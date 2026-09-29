/* Line diff and edit-region detection.
 *
 * lap's core rule: one commit = one region. A region is a maximal contiguous
 * run of changed lines (insertions, deletions, replacements) with no
 * unchanged line inside it. Two changed areas separated by at least one
 * unchanged line are two regions.
 *
 * Semantics of a Region: replace old lines [old_start, old_start+old_lines)
 * with new lines [new_start, new_start+new_lines). Starts are 1-based.
 * old_lines == 0 means pure insertion before old line old_start;
 * new_lines == 0 means pure deletion.
 */
#ifndef LAP_DIFF_H
#define LAP_DIFF_H

#include "str.h"

typedef struct {
    int32_t old_start;
    int32_t old_lines;
    int32_t new_start;
    int32_t new_lines;
} Region;

typedef struct {
    Region *v;
    int32_t count;
    /* True when the diff exceeded the effort cap and collapsed to a single
     * region covering the whole changed span. Huge rewrites are one change.
     */
    bool truncated;
} Regions;

/* Regions turning old into new. A change only in the trailing-newline state
 * of the last line is reported as a 1-line region on that line.
 */
Regions diff_lines(Arena *a, Lines oldl, Lines newl);

/* What `--lines A-B` names among a file's regions. A region is named whole
 * by its range (in the current file, or in the last-committed one for a pure
 * deletion); a pure insertion can also be named in part, any range inside
 * it, which is then committed on its own. A replacement cannot: which of its
 * old lines would go with a part of its new ones?
 */
typedef enum {
    LINES_PICK_OK,
    LINES_PICK_NONE,        /* the range touches no region */
    LINES_PICK_CROSSES,     /* it runs past the edge of an insertion */
    LINES_PICK_REPLACEMENT, /* it is part of a region that replaces lines */
} LinesPick;

/* On LINES_PICK_OK, *out is the region to commit (a part of an insertion
 * keeps the insertion's old_start: in the committed file the part lands
 * where the whole would). *which is the index of the region the range
 * lies in, or touches, for any result but LINES_PICK_NONE.
 */
LinesPick regions_pick_lines(const Regions *rs, int32_t a, int32_t b,
                             Region *out, int32_t *which);

/* Replaces base[old_start, old_start+old_lines) with repl, returns the new
 * Lines with eof_nl set to eof_nl_after. Bounds are validated; out-of-range
 * input returns base unchanged.
 */
Lines lines_replace(Arena *a, Lines base, int32_t old_start,
                    int32_t old_lines, const Str *repl, int32_t repl_count,
                    bool eof_nl_after);

#endif /* LAP_DIFF_H */
