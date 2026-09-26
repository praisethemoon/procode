import * as assert from "node:assert/strict";
import { test } from "node:test";

import type { Summary } from "coboard";

import { EMPTY, UNASSIGNED, clear, counts, fieldCount, hasQuery, isActive, matches, options, toggle, visible } from "../filter";

const s = (id: string, title: string, extra: Partial<Summary> = {}): Summary =>
    ({
        id,
        kind: id.startsWith("E") ? "epic" : id.startsWith("M") ? "milestone" : "ticket",
        title,
        status: id.startsWith("T") ? "todo" : "open",
        updated: "2026-09-26T00:00:00Z",
        ...extra,
    }) as Summary;

const board: Summary[] = [
    s("E-1", "kb: close the audit gaps"),
    s("E-4", "Board: views"),
    s("M-2", "A complete store", { epic: "E-1" }),
    s("T-12", "Spec: reference material", { epic: "E-1", milestone: null, status: "done", priority: "medium", size: "xs", assignee: "claude", labels: ["spec"] }),
    s("T-9", "Small gaps", { epic: "E-1", milestone: "M-2", status: "doing", priority: "low", size: "m", assignee: null, labels: ["kb-cli"] }),
    s("T-34", "Board sidebar: a filter bar", { epic: "E-4", milestone: null, status: "doing", priority: "high", size: "m", assignee: "claude", labels: ["coboard-vscode"] }),
];

const ids = (set: Set<string>) => [...set].sort();

test("no filter shows everything and marks nothing", () => {
    assert.equal(isActive(EMPTY), false);
    const v = visible(board, EMPTY);
    assert.equal(v.shown.size, board.length);
    assert.equal(v.matched.size, 0);
    assert.equal(isActive({ ...EMPTY, text: "   " }), false, "whitespace is not a filter");
});

test("text matches id or title, case-insensitively, and an id means exactly that item", () => {
    assert.equal(matches(board[5], { ...EMPTY, text: "FILTER bar" }), true);
    assert.equal(matches(board[5], { ...EMPTY, text: "t-34" }), true);
    assert.equal(matches(board[3], { ...EMPTY, text: "T-1" }), false, "T-1 is not T-12");
    assert.equal(matches(board[3], { ...EMPTY, text: "T-12" }), true);
});

test("a matching ticket keeps its milestone and epic above it", () => {
    const v = visible(board, { ...EMPTY, text: "small gaps" });
    assert.deepEqual(ids(v.matched), ["T-9"]);
    assert.deepEqual(ids(v.shown), ["E-1", "M-2", "T-9"]);
});

test("a matching epic shows everything under it", () => {
    const v = visible(board, { ...EMPTY, text: "audit" });
    assert.deepEqual(ids(v.matched), ["E-1"]);
    assert.deepEqual(ids(v.shown), ["E-1", "M-2", "T-12", "T-9"]);
});

test("fields combine with each other and with the text; values within a field are alternatives", () => {
    let f = toggle(EMPTY, "status", "doing");
    assert.deepEqual(ids(visible(board, f).matched), ["T-34", "T-9"]);
    f = toggle(f, "assignee", "claude");
    assert.deepEqual(ids(visible(board, f).matched), ["T-34"]);
    f = toggle(toggle(EMPTY, "priority", "low"), "priority", "high");
    assert.deepEqual(ids(visible(board, f).matched), ["T-34", "T-9"]);
    assert.equal(fieldCount(f), 2);
    assert.deepEqual(ids(visible(board, { ...toggle(EMPTY, "status", "doing"), text: "sidebar" }).matched), ["T-34"]);
    assert.deepEqual(toggle(toggle(EMPTY, "size", "m"), "size", "m").fields.size, [], "toggling twice clears it");
});

test("unassigned, labels and kind filter what they say", () => {
    assert.deepEqual(ids(visible(board, toggle(EMPTY, "assignee", UNASSIGNED)).matched), ["T-9"]);
    assert.deepEqual(ids(visible(board, toggle(EMPTY, "label", "spec")).matched), ["T-12"]);
    assert.deepEqual(ids(visible(board, toggle(EMPTY, "kind", "epic")).matched), ["E-1", "E-4"]);
});

test("the options are the fixed vocabularies and what occurs on the board", () => {
    const o = options(board);
    assert.deepEqual(o.kind, ["epic", "milestone", "ticket"]);
    assert.ok(o.status.includes("blocked") && o.status.includes("open"));
    assert.deepEqual(o.assignee, ["claude", UNASSIGNED], "unassigned sorts last");
    assert.deepEqual(o.label, ["coboard-vscode", "kb-cli", "spec"]);
});

test("counts say how many items each value would match, given the other fields and the text", () => {
    const all = counts(board, EMPTY);
    assert.equal(all.status.get("doing"), 2);
    assert.equal(all.status.get("open"), 3, "epics and milestones are open");
    assert.equal(all.assignee.get(UNASSIGNED), 1);
    assert.equal(all.label.get("blocked"), undefined, "a value nothing has counts nothing");
    // Another field narrows the counts; the field's own selection does not.
    const f = toggle(toggle(EMPTY, "label", "kb-cli"), "status", "doing");
    const c = counts(board, f);
    assert.equal(c.status.get("doing"), 1);
    assert.equal(c.status.get("done"), undefined);
    assert.equal(c.label.get("coboard-vscode"), 1, "choosing another label would add T-34");
    assert.equal(counts(board, { ...EMPTY, text: "gaps" }).kind.get("ticket"), 1);
});

test("open only hides what is done, keeps a done parent as context, and survives clearing", () => {
    const b = [...board, s("M-9", "Shipped", { epic: "E-4", status: "done" }), s("T-40", "Late fix", { epic: "E-4", milestone: "M-9", status: "todo" })];
    const f = { ...EMPTY, openOnly: true };
    assert.equal(isActive(f), true);
    assert.equal(hasQuery(f), false, "the ✕ has nothing to clear");
    const v = visible(b, f);
    assert.equal(v.matched.has("T-12"), false, "a done ticket is hidden");
    assert.equal(v.shown.has("T-12"), false, "even under its open epic");
    assert.equal(v.matched.has("M-9"), false);
    assert.equal(v.shown.has("M-9"), true, "a done milestone stays above its open ticket");
    assert.ok(v.matched.has("T-40") && v.matched.has("E-4"));
    assert.equal(counts(b, f).status.get("done"), undefined, "counts respect it");
    const g = clear({ ...toggle(f, "label", "spec"), text: "gaps" });
    assert.equal(g.openOnly, true);
    assert.equal(hasQuery(g), false);
});
