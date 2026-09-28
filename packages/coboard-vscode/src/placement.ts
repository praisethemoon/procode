/* Where a lap edit's diff opens. Pure, so the rule is tested without VS Code.
 *
 * From a review (or a ticket's page): in the editor group just right of the
 * panel, which VS Code creates when there is none. Every later click lands
 * in that same group, so the diffs pile up as tabs beside the panel and
 * never cover it; a group the user closed, or a panel they moved, is
 * followed by the next click, since the column is worked out from where the
 * panel is now.
 *
 * From a link inside a diff (no panel): the group the last diff opened in,
 * while it is still open; otherwise beside the active editor. */

export const BESIDE = "beside";

export function diffColumn(
    panel: number | undefined,
    remembered: number | undefined,
    open: readonly number[],
): number | typeof BESIDE {
    if (panel !== undefined && panel > 0) return panel + 1;
    if (remembered !== undefined && open.includes(remembered)) return remembered;
    return BESIDE;
}
