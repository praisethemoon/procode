/* index-api.md §5's staleness, for the rows that do not carry it.
 *
 * A hit answers this and a listed document does not, so §2 of the UI spec's
 * badge needs the verdict computed for the browse path — and the two paths
 * must not disagree, which is what `staleOf` is for.
 */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import { DEFAULT_STALE_DAYS, NO_DATE, fetchedAtKey, isStale, newestFirst, staleOf } from "../stale";

const NOW = Date.parse("2026-09-25T00:00:00Z");
const day = 24 * 60 * 60 * 1000;

test("the threshold is the one number either specification writes down", () => {
    /* §5's route is `?olderThan=90d` and neither document names a default.
     * Ninety is taken from the example rather than invented, and it is a
     * parameter so the surface can offer it. */
    assert.equal(DEFAULT_STALE_DAYS, 90);
});

test("older than the threshold is stale and newer is not", () => {
    assert.equal(isStale(new Date(NOW - 91 * day).toISOString(), NOW), true);
    assert.equal(isStale(new Date(NOW - 89 * day).toISOString(), NOW), false);
});

test("exactly at the threshold is not yet stale", () => {
    /* `older than` is strict. A document fetched exactly ninety days ago has
     * not passed the threshold, and a boundary that flipped a day early would
     * badge a row the store's own `?olderThan=90d` would not return. */
    assert.equal(isStale(new Date(NOW - 90 * day).toISOString(), NOW), false);
    assert.equal(isStale(new Date(NOW - 90 * day - 1).toISOString(), NOW), true);
});

test("a document whose age cannot be established is not stale", () => {
    /* §5's argument is about a passage that CANNOT SAY how old it is, and the
     * answer to that is an empty date in §3.1's provenance block — not a badge
     * asserting an age nobody knows. A badge that appears for a reason that is
     * not true teaches a reader that the badge means nothing. */
    for (const missing of ["", null, undefined, "not a date", "0000-13-45"]) {
        assert.equal(isStale(missing, NOW), false, `${JSON.stringify(missing)} was badged stale`);
    }
});

test("a timestamp in the future is not stale either", () => {
    /* A shared store crossing a clock skew should not make a document fetched
     * a moment ago read as ancient. */
    assert.equal(isStale(new Date(NOW + 10 * day).toISOString(), NOW), false);
});

test("a threshold that is not a number badges nothing rather than everything", () => {
    assert.equal(isStale(new Date(NOW - 1000 * day).toISOString(), NOW, Number.NaN), false);
    assert.equal(isStale(new Date(NOW - 1000 * day).toISOString(), NOW, -1), false);
    /* Zero days IS a threshold and means everything with a past date. */
    assert.equal(isStale(new Date(NOW - 1).toISOString(), NOW, 0), true);
});

test("what the store said wins, so the two paths cannot drift apart", () => {
    /* A hit carries `stale`, computed against the store's own threshold. A UI
     * that recomputed would hold a second opinion about a question already
     * answered, and the two would disagree the day the store's threshold
     * changed. */
    const fresh = new Date(NOW - 1 * day).toISOString();
    const old = new Date(NOW - 1000 * day).toISOString();
    assert.equal(staleOf({ fetchedAt: fresh, stale: true }, NOW), true);
    assert.equal(staleOf({ fetchedAt: old, stale: false }, NOW), false);
    /* And a row with no verdict falls through to the computation. */
    assert.equal(staleOf({ fetchedAt: old }, NOW), true);
    assert.equal(staleOf({ fetchedAt: fresh }, NOW), false);
    assert.equal(staleOf({}, NOW), false);
});

/* ------------------------------------------------------------ the order */

test("a row with no readable date sorts last, and never unsorts the list", () => {
    /* `Date.parse` of nonsense is NaN, and NaN in a comparator makes every
     * comparison false — which is how a list silently stops being sorted at
     * all rather than being sorted wrongly. */
    assert.equal(fetchedAtKey("2026-01-01T00:00:00Z"), Date.parse("2026-01-01T00:00:00Z"));
    assert.equal(fetchedAtKey(""), NO_DATE);
    assert.equal(fetchedAtKey(null), NO_DATE);
    assert.equal(fetchedAtKey("whenever"), NO_DATE);

    assert.deepEqual(
        newestFirst([
            { fetchedAt: "2026-01-01T00:00:00Z" },
            { fetchedAt: "nonsense" },
            { fetchedAt: "2026-06-01T00:00:00Z" },
            { fetchedAt: "" },
        ]).map((r) => r.fetchedAt),
        ["2026-06-01T00:00:00Z", "2026-01-01T00:00:00Z", "nonsense", ""],
    );
});

test("the floor is finite, because two undated rows have to compare equal", () => {
    /* `-Infinity - -Infinity` is `NaN`, which is a comparator that has stopped
     * answering — the exact unsortedness the floor exists to prevent, one step
     * along. This is the assertion that would fail if somebody "tidied" the
     * sentinel into `-Infinity`. */
    assert.ok(Number.isFinite(NO_DATE), "the no-date floor is not finite");
    assert.equal(fetchedAtKey(null) - fetchedAtKey("nonsense"), 0);
});

test("rows fetched at the same instant keep the store's order", () => {
    /* A directory walk files a whole tree at one timestamp. An unstable sort
     * would reshuffle them on every repaint, which is a list that flickers
     * while somebody is reading it. */
    const same = "2026-06-01T00:00:00Z";
    const rows = [
        { id: "a", fetchedAt: same },
        { id: "b", fetchedAt: same },
        { id: "c", fetchedAt: same },
    ];
    assert.deepEqual(newestFirst(rows).map((r) => r.id), ["a", "b", "c"]);
    assert.deepEqual(newestFirst(newestFirst(rows)).map((r) => r.id), ["a", "b", "c"]);
});

test("sorting does not disturb what it was given", () => {
    const rows = [{ fetchedAt: "2020-01-01T00:00:00Z" }, { fetchedAt: "2026-01-01T00:00:00Z" }];
    newestFirst(rows);
    assert.deepEqual(rows.map((r) => r.fetchedAt), [
        "2020-01-01T00:00:00Z",
        "2026-01-01T00:00:00Z",
    ]);
});
