/* index-ui.md §6's scheme.
 *
 * Every assertion here is about ONE TAB PER DOCUMENT. VSCode focuses an
 * existing editor when the resource and the view type match and it compares
 * resources as strings, so two spellings that do not fold to one are two tabs
 * for one document — the failure §6 names by hand.
 */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import {
    KB_SCHEME,
    PLACES,
    fallbackTitle,
    isPlaceable,
    parseTarget,
    referenceUri,
    targetPath,
    targetRef,
    targetUri,
} from "../uri";

test("every spelling of one reference folds to one target", () => {
    for (const written of ["D-241", "d-241", "/D-241", "kb:/D-241", "kb:///D-241", "  KB:/d-241 "]) {
        assert.deepEqual(
            parseTarget(written),
            { sort: "entity", kind: "document", id: "D-241" },
            `${written} did not resolve to D-241`,
        );
    }
});

test("the canonical URI is one string, so two spellings are one tab", () => {
    const spellings = ["D-241", "d-241", "kb:///D-241"];
    const uris = spellings.map((s) => referenceUri(s));
    assert.deepEqual([...new Set(uris)], ["kb:/D-241"]);
});

/** The kind of an entity target, or null when it is not one. */
function kindOf(raw: string): string | null {
    const t = parseTarget(raw);
    return t === null || t.sort !== "entity" ? null : t.kind;
}

test("§1.1's three prefixes resolve and nothing else does", () => {
    assert.equal(kindOf("S-3"), "source");
    assert.equal(kindOf("D-241"), "document");
    assert.equal(kindOf("C-99812"), "chunk");
    for (const bad of ["T-451", "E-3", "X-1", "241", "D-", "D", "-1", "", "   ", "kb:/"]) {
        assert.equal(parseTarget(bad), null, `${JSON.stringify(bad)} resolved to something`);
    }
});

test("a padded identifier names nothing rather than its unpadded neighbour", () => {
    /* §1.1's identifiers are monotonic and never padded, so `D-007` is not a
     * reference the store ever handed out. Resolving it to `D-7` would be two
     * references for one document, and therefore two tabs. */
    assert.equal(parseTarget("D-007"), null);
    assert.equal(parseTarget("D-0"), null, "a bare zero is not an identifier either");
    const ten = parseTarget("D-10");
    assert.ok(ten !== null && ten.sort === "entity" && ten.id === "D-10", "a trailing zero is fine");
});

test("collections is a place and is the only one", () => {
    assert.deepEqual(parseTarget("collections"), { sort: "place", place: "collections" });
    assert.deepEqual(parseTarget("kb:/COLLECTIONS"), { sort: "place", place: "collections" });
    assert.deepEqual([...PLACES], ["collections"]);
});

test("a chunk is a reference and is not a place", () => {
    /* §6 gives URIs to a document, a source and the collection list. A tab
     * whose whole content was one chunk would be a passage with its provenance
     * cut off, which is what §3.1 exists to prevent — so a chunk RESOLVES (a
     * hit names one, and §3.2 scrolls to it) and does not open. */
    const chunk = parseTarget("C-99812");
    assert.ok(chunk !== null);
    assert.equal(isPlaceable(chunk), false);
    for (const placeable of ["D-241", "S-3", "collections"]) {
        const t = parseTarget(placeable);
        assert.ok(t !== null);
        assert.equal(isPlaceable(t), true, `${placeable} should be a place`);
    }
});

test("the reference, the URI and the path agree", () => {
    const t = parseTarget("d-241");
    assert.ok(t !== null);
    assert.equal(targetRef(t), "D-241");
    assert.equal(targetUri(t), "kb:/D-241");
    assert.equal(targetPath(t), "/D-241");
    assert.equal(KB_SCHEME, "kb");
});

test("a tab has a name before the store has answered", () => {
    assert.equal(fallbackTitle({ sort: "entity", kind: "document", id: "D-241" }), "D-241");
    assert.equal(fallbackTitle({ sort: "place", place: "collections" }), "Collections");
});

test("nothing in another scheme resolves here", () => {
    for (const bad of ["coboard:/T-1", "file:///D-241", "https://example.test/D-241"]) {
        assert.equal(parseTarget(bad), null, `${bad} resolved`);
    }
});
