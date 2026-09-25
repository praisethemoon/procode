/* What a reader does with an answer that is not the answer it expected.
 *
 * The rule these all state: A MISSING FIELD IS A DEFAULT AND NEVER A DROPPED
 * ROW. A document whose title the store did not carry is still a document, and
 * a reader that filtered it out would hide the one row somebody needs in order
 * to go and fix it. An empty string is visibly empty; an absent row is
 * invisible.
 */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import {
    arr,
    obj,
    readChunk,
    readCollection,
    readDocument,
    readHit,
    readSource,
    readStore,
    strOrNull,
} from "../shape";

test("a row with nothing in it is still a row, with everything empty", () => {
    const d = readDocument({});
    assert.equal(d.id, "");
    assert.equal(d.title, "");
    assert.equal(d.bytes, 0);
    assert.deepEqual(d.meta, {});
});

test("a field of the wrong type reads as absent rather than as itself", () => {
    /* `bytes: "lots"` in a UI is `"lots".toLocaleString` — or a template that
     * renders the word where a size belongs. */
    const d = readDocument({ id: 7, title: null, bytes: "lots", meta: [1, 2], chunkCount: "3" });
    assert.equal(d.id, "");
    assert.equal(d.title, "");
    assert.equal(d.bytes, 0);
    assert.equal(d.chunkCount, 0);
    assert.deepEqual(d.meta, {}, "an array is not a free-form object");
});

test("an unnamed tier reads as global, because project is a claim and global is not", () => {
    /* §1.4 makes the tier part of provenance. Attributing an unnamed one to
     * the project would say "this belongs to this codebase" on no authority. */
    assert.equal(readStore("project"), "project");
    assert.equal(readStore("global"), "global");
    for (const odd of ["Project", "", null, undefined, 3, {}]) {
        assert.equal(readStore(odd), "global", `${JSON.stringify(odd)} was read as the project tier`);
    }
});

test("a heading is a string with something in it, or nothing at all", () => {
    assert.equal(strOrNull("x"), "x");
    assert.equal(strOrNull(""), null);
    assert.equal(strOrNull(null), null);
    assert.equal(strOrNull(undefined), null);
    assert.equal(strOrNull(0), null);
});

test("a chunk that was not asked to carry its text has no text key at all", () => {
    /* Not `text: ""`. The difference is "this chunk is empty" against "the
     * text was not fetched", and a UI showing the first for the second is a
     * blank panel with no explanation. */
    assert.equal("text" in readChunk({ id: "C-1" }), false);
    assert.equal(readChunk({ id: "C-1", text: "" }).text, "");
    assert.equal(readChunk({ id: "C-1", text: "hi" }).text, "hi");
});

test("a span that is missing is zero-to-zero rather than undefined arithmetic", () => {
    assert.deepEqual(readChunk({}).span, { start: 0, end: 0 });
    assert.deepEqual(readChunk({ span: { start: "a", end: 5 } }).span, { start: 0, end: 5 });
});

test("a hit with no scores reads as zeros, not as a missing object", () => {
    const h = readHit({ chunk: "C-1" });
    assert.deepEqual(h.scores, { bm25: 0, vector: 0, fused: 0 });
    assert.deepEqual([...h.matched], []);
    assert.equal(h.stale, false);
    assert.equal(h.alsoGlobal, false);
});

test("matched keeps the store's order and drops only what is not a name", () => {
    assert.deepEqual([...readHit({ matched: ["semantic", "keyword"] }).matched], [
        "semantic",
        "keyword",
    ]);
    assert.deepEqual([...readHit({ matched: "keyword" }).matched], [], "a bare string is not a list");
});

test("a source and a collection read the same way", () => {
    assert.equal(readSource({}).docCount, 0);
    assert.equal(readSource({ kind: "url", locator: "x" }).kind, "url");
    assert.equal(readCollection({ name: "papers" }).documents, 0);
    assert.equal(readCollection({}).name, "");
});

test("the small readers say what they are asked and never throw", () => {
    /* They stand between `JSON.parse`'s `any` and every other module, so one
     * of them throwing would be an exception arriving from a row rather than
     * from a call. */
    for (const v of [null, undefined, 0, "", [], {}, NaN, Infinity]) {
        assert.doesNotThrow(() => {
            obj(v);
            arr(v);
            readDocument(v);
            readHit(v);
            readChunk(v);
        });
    }
    assert.deepEqual(arr("nope"), []);
    assert.deepEqual(obj([1]), {});
});
