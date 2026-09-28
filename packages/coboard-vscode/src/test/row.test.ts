import * as assert from "node:assert/strict";
import { test } from "node:test";

import type { Summary } from "coboard";

import { rowParts } from "../row";

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
