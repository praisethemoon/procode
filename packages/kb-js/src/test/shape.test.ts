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
    readBatchAdded,
    readDirAdded,
    readEmbedded,
    readFiled,
    readDocument,
    readHit,
    readModelStatus,
    readSource,
    readStatus,
    strOrNull,
} from "../shape";
import { KbError } from "../errors";

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

test("a path's score is absent when that path did not find the hit, never zero", () => {
    /* "the vector path found nothing here" and "it scored 0" are different
     * answers; only the store may say which. */
    const h = readHit({ chunk: "C-1" });
    assert.deepEqual(h.scores, { fused: 0 });
    assert.deepEqual(readHit({ scores: { vector: 0.61, fused: 0.016 } }).scores, { vector: 0.61, fused: 0.016 });
    assert.deepEqual(readHit({ scores: { bm25: 2.5, vector: "x", fused: 0.03 } }).scores, { bm25: 2.5, fused: 0.03 });
    assert.deepEqual([...h.matched], []);
    assert.equal(h.stale, false);
});

test("a rerank score is kept when the store sent a number, and absent otherwise", () => {
    /* Only the hits the cross-encoder reached carry one; the rest keep their
     * fused score alone. A logit can be negative and is kept as it is. */
    assert.deepEqual(readHit({ scores: { bm25: 2.5, fused: 0.03, rerank: -1.25 } }).scores, {
        bm25: 2.5,
        fused: 0.03,
        rerank: -1.25,
    });
    assert.equal("rerank" in readHit({ scores: { fused: 0.03 } }).scores, false);
    assert.equal("rerank" in readHit({ scores: { fused: 0.03, rerank: "4" } }).scores, false);
    assert.equal("rerank" in readHit({ scores: { fused: 0.03, rerank: null } }).scores, false);
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

test("the model status reads both sides typed, and null where either is absent", () => {
    const config = {
        model: "nomic-embed-text-v1.5",
        arch: "nomic-bert",
        dim: 768,
        pooling: "mean",
        maxTokens: 1024,
        queryPrefix: "search_query: ",
        documentPrefix: "search_document: ",
        normalize: true,
        quantization: "int8",
        weights: "Q4_K_M",
        tokenizer: 1,
        fingerprint: "053c01",
    };
    const both = readModelStatus({
        recorded: { ...config, sha256: "ab".repeat(32) },
        available: { path: "/home/ana/.kb/models/m.gguf", bytes: 84106624, ...config },
        current: true,
    });
    assert.equal(both.recorded?.dim, 768);
    assert.equal(both.recorded?.sha256.length, 64);
    assert.equal(both.available?.path, "/home/ana/.kb/models/m.gguf");
    assert.equal(both.current, true);
    assert.equal("missing" in both, false);

    const none = readModelStatus({ recorded: null, available: null, missing: "no embedding model in ~/.kb/models", current: null });
    assert.equal(none.recorded, null);
    assert.equal(none.available, null);
    assert.match(none.missing ?? "", /no embedding model/);
    assert.equal(none.current, null);

    const e = new KbError("model_mismatch", "another model", ["search"], {
        stored: config,
        loaded: { ...config, weights: "F16", fingerprint: "99" },
    });
    assert.equal(e.detailsOf("model_mismatch")?.loaded.weights, "F16");
    assert.equal(new KbError("model_mismatch", "x", [], { stored: 1 }).detailsOf("model_mismatch"), null);
});

test("a folder's answer reads whole, and a folder with no source says null rather than an id", () => {
    const filed = readDirAdded({
        ok: true,
        source: "S-4",
        root: "/work/lap/cli/kb-cli",
        collection: "code",
        files: 92,
        added: 3,
        updated: 1,
        unchanged: 88,
        forgotten: ["D-51"],
        missing: [],
        skipped: { ignored: 2, hidden: 0, vendored: 1, generated: 0, binary: 0, large: 1, unreadable: 0, otherTypes: 0 },
        embedded: 14,
    });
    assert.equal(filed.source, "S-4");
    assert.equal(filed.root, "/work/lap/cli/kb-cli");
    assert.equal(filed.files, 92);
    assert.equal(filed.unchanged, 88);
    assert.deepEqual([...filed.forgotten], ["D-51"]);
    assert.deepEqual([...filed.missing], []);
    assert.equal(filed.skipped.large, 1);
    assert.equal(filed.skipped.otherTypes, 0);
    assert.equal(filed.embedded, 14);
    assert.equal(filed.pending, 0, "a binary that predates the budget embedded everything");
    assert.equal(readDirAdded({ embedded: 3, pending: 120 }).pending, 120);

    /* An empty folder filed for the first time: there is no source, and ""
     * would be read by a caller as an id to look up. */
    const empty = readDirAdded({ ok: true, source: null, root: "/x", collection: "c", files: 0 });
    assert.equal(empty.source, null);
    assert.equal(empty.added, 0);
    assert.deepEqual(empty.skipped, {
        ignored: 0,
        hidden: 0,
        vendored: 0,
        generated: 0,
        binary: 0,
        large: 0,
        unreadable: 0,
        otherTypes: 0,
    });
    // Only names survive in either list.
    assert.deepEqual([...readDirAdded({ missing: ["a.c", 3, null, "b/c.h"] }).missing], ["a.c", "b/c.h"]);
});

test("pending is kept on every add's answer, and is zero from a binary that predates it", () => {
    const single = readFiled({ ok: true, document: "D-7", chunkCount: 900, created: true, pending: 812 });
    assert.equal(single.document, "D-7");
    assert.equal(single.chunkCount, 900);
    assert.equal(single.pending, 812);
    assert.equal(readFiled({ document: "D-7" }).pending, 0);
    assert.equal(readFiled({ document: "D-7", pending: "5" }).pending, 0);

    const batch = readBatchAdded({ ok: true, added: [{ document: "D-1" }, { document: "D-2" }], count: 2, pending: 40 });
    assert.deepEqual(batch.added.map((a) => a.document), ["D-1", "D-2"]);
    assert.equal(batch.count, 2);
    assert.equal(batch.pending, 40);
    /* No count from the store: the rows' own number, not a guess at a total. */
    const old = readBatchAdded({ added: [{ document: "D-1" }] });
    assert.equal(old.count, 1);
    assert.equal(old.pending, 0);
    /* A batch's rows do not carry pending: it is the batch's, once. */
    assert.equal("pending" in batch.added[0], false);
});

test("embed's answer reads whole, and a missing count is zero", () => {
    assert.deepEqual(readEmbedded({ ok: true, embedded: 812, kept: 88, skipped: 3, pending: 0 }), {
        embedded: 812,
        kept: 88,
        skipped: 3,
        pending: 0,
    });
    assert.deepEqual(readEmbedded({}), { embedded: 0, kept: 0, skipped: 0, pending: 0 });
});

test("status carries the vectors when the store reports them, and leaves them out when it does not", () => {
    const base = { ok: true, path: "/w/.kb", present: true, readable: true, olderThan: "90d" };
    const withVectors = readStatus({
        ...base,
        index: { keyword: { current: true }, vectors: { count: 88, missing: 812, current: false } },
    });
    assert.deepEqual(withVectors.vectors, { count: 88, missing: 812, current: false });
    assert.equal("vectors" in readStatus(base), false);
    assert.equal("vectors" in readStatus({ ...base, readable: false }), false);
});
