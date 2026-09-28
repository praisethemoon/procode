import * as assert from "node:assert/strict";
import { test } from "node:test";

import { EMPTY, isActive, keywordCounts, matches, toggleKeyword } from "../filter";

const page = (id: string, title: string, description = "", keywords: string[] = []) => ({ id, title, description, keywords });
const PAGES = [
    page("A-1", "Merge notes", "how lap merge adopts a branch", ["lap", "merge"]),
    page("A-2", "Search benchmark", "kb search latency", ["kb", "bench"]),
    page("A-3", "Design note: the rail", "", ["kb", "design note"]),
];
const ids = (f: typeof EMPTY) => PAGES.filter((p) => matches(p, f)).map((p) => p.id);

test("no filter keeps every page", () => {
    assert.equal(isActive(EMPTY), false);
    assert.deepEqual(ids(EMPTY), ["A-1", "A-2", "A-3"]);
});

test("text matches the id, title, description and keywords, every word, in any case", () => {
    assert.deepEqual(ids({ ...EMPTY, text: "a-2" }), ["A-2"]);
    assert.deepEqual(ids({ ...EMPTY, text: "MERGE" }), ["A-1"]);
    assert.deepEqual(ids({ ...EMPTY, text: "latency" }), ["A-2"]);
    assert.deepEqual(ids({ ...EMPTY, text: "design note" }), ["A-3"]);
    assert.deepEqual(ids({ ...EMPTY, text: "kb rail" }), ["A-3"], "both words, anywhere in the page");
    assert.deepEqual(ids({ ...EMPTY, text: "   " }), ["A-1", "A-2", "A-3"]);
    assert.equal(isActive({ ...EMPTY, text: "x" }), true);
});

test("chosen keywords keep a page carrying any of them, and combine with the text", () => {
    assert.deepEqual(ids({ ...EMPTY, keywords: ["kb"] }), ["A-2", "A-3"]);
    assert.deepEqual(ids({ ...EMPTY, keywords: ["merge", "bench"] }), ["A-1", "A-2"]);
    assert.deepEqual(ids({ text: "rail", keywords: ["kb"] }), ["A-3"]);
    const on = toggleKeyword(EMPTY, "kb");
    assert.deepEqual(on.keywords, ["kb"]);
    assert.deepEqual(toggleKeyword(on, "kb").keywords, []);
});

test("the keyword choices are counted, most used first, then by name", () => {
    assert.deepEqual(keywordCounts(PAGES), [
        { keyword: "kb", count: 2 },
        { keyword: "bench", count: 1 },
        { keyword: "design note", count: 1 },
        { keyword: "lap", count: 1 },
        { keyword: "merge", count: 1 },
    ]);
});
