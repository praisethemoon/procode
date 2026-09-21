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

/* Replaces base[old_start, old_start+old_lines) with repl, returns the new
 * Lines with eof_nl set to eof_nl_after. Bounds are validated; out-of-range
 * input returns base unchanged.
 */
Lines lines_replace(Arena *a, Lines base, int32_t old_start,
                    int32_t old_lines, const Str *repl, int32_t repl_count,
                    bool eof_nl_after);

#endif /* LAP_DIFF_H */
