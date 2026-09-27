import * as assert from "node:assert/strict";
import { test } from "node:test";

import type { LapDiff } from "coboard";

import { SHOW_EDIT, commentText, localTime, mdCommitText, mdProse, regionLabel, regionLines } from "../lapview";

const at = { oldStart: 4, oldLines: 2, newStart: 4, newLines: 4 };

test("the region reads like lap's own", () => {
    assert.equal(regionLabel(at), "lines 4-7");
    assert.equal(regionLabel({ oldStart: 3, oldLines: 1, newStart: 3, newLines: 0 }), "line 3 (deleted)");
    assert.equal(regionLabel({ oldStart: 1, oldLines: 0, newStart: 1, newLines: 2 }), "lines 1-2 (insertion)");
    assert.equal(regionLabel({ oldStart: 2, oldLines: 1, newStart: 2, newLines: 1 }), "line 2");
    assert.deepEqual(regionLines(at), { start: 3, end: 6 });
    assert.deepEqual(regionLines({ newStart: 3, newLines: 0 }), { start: 2, end: 2 });
});

const H = "1a2b3c4d".padEnd(64, "0");
const diff = (extra: Partial<LapDiff> = {}): LapDiff => ({
    id: "L3", hash: H, file: "f", op: "edit", intent: "why", behavior: "what", forced: false, earlier: [],
    ts: "2026-09-25T20:00:00Z", user: "claude", session: "S1", before: "", after: "", line: 4, ...at, ...extra,
});

test("the intent and behavior are escaped Markdown that still wraps, under the id and short hash, with the session below", () => {
    assert.equal(mdProse("a *b*\nc"), "a \\*b\\*  \nc");
    const c = commentText(diff({ intent: "Tidy the *loop*", behavior: "The loop ends early\non an empty list" }), "T-1: work");
    assert.match(c.author, /^L3 · 1a2b3c4 @ .+ claude:$/);
    assert.equal(
        c.body,
        "**Intent**: Tidy the \\*loop\\*\n\n**Behavior**: The loop ends early  \non an empty list\n\n---\n\n*session S1: T\\-1: work*\n\n&nbsp;",
    );
    const loose = commentText(diff({ id: "L1", ts: "", user: "", session: null }), null);
    assert.match(loose.author, /^L1 · 1a2b3c4 @ :$/);
    assert.match(loose.body, /committed outside any session \\\(\\-\\-no\\-session\\\)/);
});

test("a behavior is shown as written, whatever it says", () => {
    const c = commentText(diff({ behavior: "The data of this field is not present" }), null);
    assert.match(c.body, /\*\*Behavior\*\*: The data of this field is not present\n/);
});

test("a forced commit is marked", () => {
    assert.doesNotMatch(commentText(diff(), null).body, /forced/);
    assert.match(commentText(diff({ forced: true }), null).body, /\n\n\*\*forced\*\*: \*committed with \\-\\-force\\-message, past lap's message checks\*\n\n---/);
});

test("an amended commit shows its latest text, then the earlier ones quoted, oldest first", () => {
    assert.doesNotMatch(commentText(diff(), null).body, /amended/);
    const body = commentText(
        diff({
            intent: "why, better",
            behavior: "what, better",
            earlier: [
                { intent: "why", behavior: "what", user: "claude", ts: "" },
                { intent: "why again", behavior: "two\nlines", user: null, ts: "" },
            ],
        }),
        null,
    ).body;
    assert.match(body, /^\*\*Intent\*\*: why, better\n\n\*\*Behavior\*\*: what, better\n\n\*\*amended\*\*: \*corrected with lap amend 2 times; the earlier texts, oldest first:\*/);
    assert.match(body, /\n\n> claude {2}\n> Intent: why {2}\n> Behavior: what\n\n> Intent: why again {2}\n> Behavior: two {2}\n> lines\n\n---/);
});

test("commits named in the text, by hash or by id, link to themselves", () => {
    const link = (ref: string) => `(command:${SHOW_EDIT}?${encodeURIComponent(JSON.stringify([ref]))})`;
    assert.equal(
        mdCommitText("Undoes #1A2B3C4D and L1029."),
        `Undoes [\\#1A2B3C4D]${link("1a2b3c4d")} and [L1029]${link("L1029")}\\.`,
    );
    assert.equal(mdCommitText("see L2\nand #abcdef0"), `see [L2]${link("L2")}  \nand [\\#abcdef0]${link("abcdef0")}`);
    const c = commentText(diff({ intent: "Follows L2", behavior: "Reverts #abcdef01" }), null);
    assert.ok(c.body.includes(`**Intent**: Follows [L2]${link("L2")}`));
    assert.ok(c.body.includes(`**Behavior**: Reverts [\\#abcdef01]${link("abcdef01")}`));
});

test("localTime shows the date, a space, and the time to the second", () => {
    const iso = "2026-09-27T09:36:48Z";
    const d = new Date(iso);
    const date = d.toLocaleDateString(undefined, { year: "numeric", month: "2-digit", day: "2-digit" });
    const shown = localTime(iso);
    assert.ok(shown.startsWith(`${date} `), shown);
    assert.match(shown.slice(date.length + 1), /^\d{1,2}:\d{2}:48\b/);
    assert.equal(localTime("not a time"), "not a time");
});
