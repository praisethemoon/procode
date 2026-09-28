import * as assert from "node:assert/strict";
import { test } from "node:test";

import type { Summary } from "coboard";

import { quickArchivable, rowParts } from "../row";

const s = (extra: Partial<Summary> & Pick<Summary, "id" | "kind" | "title" | "status">): Summary =>
    ({ updated: "2026-09-28T00:00:00Z", ...extra }) as Summary;

test("a ticket row is its status icon, its id and its title; the status is only in the icon and the tooltip", () => {
    const p = rowParts(s({ id: "T-12", kind: "ticket", title: "declare time() on Windows", status: "doing", epic: "E-1" }));
    assert.deepEqual(p, {
        icon: "play-circle",
        tone: "doing",
        id: "T-12",
        title: "declare time() on Windows",
        archived: false,
        tooltip: "T-12 — declare time() on Windows\nticket, doing",
    });
    assert.ok(!p.title.includes("doing") && !p.id.includes("doing"));
});

test("each ticket status has its own icon", () => {
    const icon = (status: string) => rowParts(s({ id: "T-1", kind: "ticket", title: "x", status })).icon;
    assert.deepEqual(
        ["todo", "doing", "blocked", "review", "done", "odd"].map(icon),
        ["circle-large-outline", "play-circle", "error", "eye", "pass-filled", "circle-large-outline"],
    );
});

test("a milestone row leads with its id and is toned by its kind", () => {
    assert.deepEqual(rowParts(s({ id: "M-15", kind: "milestone", title: "Robustness, round 2", status: "open", epic: "E-15" })), {
        icon: "milestone",
        tone: "milestone",
        id: "M-15",
        title: "Robustness, round 2",
        archived: false,
        tooltip: "M-15 — Robustness, round 2\nmilestone, open",
    });
    assert.equal(rowParts(s({ id: "M-1", kind: "milestone", title: "x", status: "done" })).icon, "pass");
});

test("an epic row leads with its id and is toned by its kind", () => {
    assert.deepEqual(rowParts(s({ id: "E-15", kind: "epic", title: "Views: quality of life", status: "open" })), {
        icon: "project",
        tone: "epic",
        id: "E-15",
        title: "Views: quality of life",
        archived: false,
        tooltip: "E-15 — Views: quality of life\nepic, open",
    });
    assert.equal(rowParts(s({ id: "E-2", kind: "epic", title: "x", status: "done" })).icon, "pass");
});

test("an archived row says so in its marker and tooltip", () => {
    const p = rowParts(s({ id: "T-1003", kind: "ticket", title: "old", status: "done", archived: true }));
    assert.equal(p.archived, true);
    assert.equal(p.tooltip, "T-1003 — old\nticket, done, archived");
    assert.equal(p.id, "T-1003");
});

test("an epic is offered a quick archive when every milestone and milestone-less ticket in it is done", () => {
    const e = (id: string, extra: Partial<Summary> = {}) => s({ id, kind: "epic", title: id, status: "open", ...extra });
    const m = (id: string, epic: string, status: string, extra: Partial<Summary> = {}) => s({ id, kind: "milestone", title: id, status, epic, ...extra });
    const t = (id: string, epic: string, status: string, milestone: string | null = null, extra: Partial<Summary> = {}) =>
        s({ id, kind: "ticket", title: id, status, epic, milestone, ...extra });
    const board = [
        e("E-1"), m("M-1", "E-1", "done"), t("T-1", "E-1", "done"), t("T-2", "E-1", "todo", "M-1"),
        e("E-2"), m("M-2", "E-2", "done"), t("T-3", "E-2", "doing"),
        e("E-3"), m("M-3", "E-3", "open"),
        e("E-4"),
        e("E-5"), t("T-4", "E-5", "done"), t("T-5", "E-5", "todo", null, { archived: true }),
        e("E-6", { archived: true }), t("T-6", "E-6", "done"),
    ];
    assert.deepEqual([...quickArchivable(board)].sort(), ["E-1", "E-5"]);
    // E-1: its milestone and its own ticket are done; a ticket inside the milestone is the milestone's to answer for.
    // E-2: a milestone-less ticket is still doing. E-3: its milestone is open. E-4: nothing in it.
    // E-5: the unfinished ticket is archived, out of the way. E-6: already archived.
});
