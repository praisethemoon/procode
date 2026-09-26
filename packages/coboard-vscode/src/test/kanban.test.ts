import * as assert from "node:assert/strict";
import { test } from "node:test";

import type { Summary } from "coboard";

import { columns, moves } from "../kanban";

const t = (id: string, status: string, priority = "medium"): Summary =>
    ({ id, kind: "ticket", title: id, status, priority, updated: "" }) as Summary;

test("one column per status, in the order work moves, every ticket in exactly one", () => {
    const tickets = [t("T-1", "done"), t("T-2", "todo"), t("T-3", "doing"), t("T-4", "todo"), t("T-5", "blocked")];
    const cols = columns(tickets);
    assert.deepEqual(cols.map((c) => c.status), ["todo", "doing", "blocked", "review", "done"]);
    assert.deepEqual(cols.map((c) => c.tickets.map((x) => x.id)), [["T-2", "T-4"], ["T-3"], ["T-5"], [], ["T-1"]]);
    assert.equal(cols.reduce((n, c) => n + c.tickets.length, 0), tickets.length);
});

test("a column is most urgent first, then oldest id", () => {
    const col = columns([t("T-9", "todo", "low"), t("T-12", "todo", "urgent"), t("T-3", "todo"), t("T-10", "todo", "high"), t("T-2", "todo")])[0];
    assert.deepEqual(col.tickets.map((x) => x.id), ["T-12", "T-10", "T-2", "T-3", "T-9"]);
});

test("a drop changes a status only when it is a different, real one", () => {
    assert.equal(moves(t("T-1", "todo"), "doing"), true);
    assert.equal(moves(t("T-1", "todo"), "todo"), false);
    assert.equal(moves(t("T-1", "todo"), "open"), false);
});
