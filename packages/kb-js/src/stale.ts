/* index-api.md §5's staleness, for the rows that do not carry it.
 *
 * A SEARCH HIT ANSWERS THIS AND A LISTED DOCUMENT DOES NOT. §4 puts `stale` on
 * the hit, computed by the store against its own threshold; `kb ls` answers
 * `Document` rows, which §1.2 gives a `fetchedAt` and no verdict. §2 of the UI
 * spec shows the badge on every row in both states of the list — browsing and
 * searching — so the browse path needs the verdict computed somewhere, and
 * that somewhere is here rather than inside a component, where no test could
 * reach it.
 *
 * WHERE THE THRESHOLD IS SET IS A QUESTION THE SPECIFICATIONS DO NOT SETTLE.
 * §5 writes `?olderThan=90d` as the example on the route and never names a
 * default; §2 of the UI spec says "older than the staleness threshold" and
 * does not say whose. Ninety days is taken from the one number either document
 * writes down, and it is a parameter rather than a constant so that the surface
 * above can offer it and a store that starts reporting its own can override it.
 *
 * WHEN A HIT CARRIES `stale`, THAT IS THE ANSWER. `staleOf` exists so the two
 * paths cannot disagree: it prefers what the store said and computes only when
 * the store said nothing. A UI that recomputed over a hit would be a second
 * opinion about a question already answered, and the two would drift the day
 * the store's threshold changed.
 */

export const DEFAULT_STALE_DAYS = 90;

const MS_PER_DAY = 24 * 60 * 60 * 1000;

/* Whether a timestamp is older than `days` before `now`.
 *
 * AN UNREADABLE OR ABSENT TIMESTAMP IS NOT STALE. A document whose age cannot
 * be established has not been shown to be old, and badging it would put a
 * warning on a row for a reason that is not true — which is the way to teach a
 * reader that the badge means nothing. §5's argument is about a passage that
 * *cannot say* how old it is, and the answer to that is the provenance block
 * in §3.1 showing an empty date, not a badge asserting an age.
 *
 * A TIMESTAMP IN THE FUTURE IS NOT STALE EITHER, which follows from the same
 * comparison and is worth stating: a clock skew across a shared store should
 * not make a document that was fetched a moment ago read as fresh-and-then-
 * suddenly-ancient. */
export function isStale(
    fetchedAt: string | null | undefined,
    now: number,
    days: number = DEFAULT_STALE_DAYS,
): boolean {
    if (typeof fetchedAt !== "string" || fetchedAt.length === 0) {
        return false;
    }
    const at = Date.parse(fetchedAt);
    if (Number.isNaN(at)) {
        return false;
    }
    if (!Number.isFinite(days) || days < 0) {
        return false;
    }
    return now - at > days * MS_PER_DAY;
}

/* The verdict for a row that may or may not already carry one. */
export function staleOf(
    row: { fetchedAt?: string | null; stale?: boolean },
    now: number,
    days: number = DEFAULT_STALE_DAYS,
): boolean {
    return typeof row.stale === "boolean" ? row.stale : isStale(row.fetchedAt, now, days);
}

/* Sort key for §2's "every document in scope, newest first".
 *
 * A ROW WITH NO READABLE DATE SORTS LAST rather than first. `Date.parse` of
 * nonsense is `NaN`, and `NaN` in a comparator is how a list silently stops
 * being sorted at all — every comparison involving it answers false, so the
 * array keeps whatever order the engine's sort happened to leave. Mapping it to
 * a floor makes the undated rows a block at the bottom, which is where a row
 * whose age is unknown belongs in a list whose whole ordering is age.
 *
 * THE FLOOR IS FINITE AND `-Infinity` IS NOT GOOD ENOUGH, which is the same
 * bug one step along: `b - a` over two undated rows is `-Infinity - -Infinity`,
 * which is `NaN`, which is a comparator that has stopped answering. Two rows
 * that both lack a date have to compare EQUAL, and only a finite floor does
 * that. */
export const NO_DATE = Number.MIN_SAFE_INTEGER;

export function fetchedAtKey(fetchedAt: string | null | undefined): number {
    if (typeof fetchedAt !== "string" || fetchedAt.length === 0) {
        return NO_DATE;
    }
    const at = Date.parse(fetchedAt);
    return Number.isNaN(at) ? NO_DATE : at;
}

/* §2's default list: "every document in scope, newest first".
 *
 * STABLE, AND THAT IS PART OF THE ANSWER. Two documents fetched in the same
 * second — a directory walk files a whole tree at one timestamp — would
 * otherwise reshuffle on every repaint, which is a list that flickers while a
 * reader is trying to read it. `Array.prototype.sort` is stable in every Node
 * this package supports, so the store's own order is what breaks the tie. */
export function newestFirst<T extends { fetchedAt?: string | null }>(rows: readonly T[]): T[] {
    return [...rows].sort((a, b) => fetchedAtKey(b.fetchedAt) - fetchedAtKey(a.fetchedAt));
}
