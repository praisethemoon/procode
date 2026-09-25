import * as assert from "node:assert/strict";
import { test } from "node:test";

import { commentText, mdProse, regionLabel, regionLines } from "../lapview";

const at = { oldStart: 4, oldLines: 2, newStart: 4, newLines: 4 };

test("the region reads like lap's own", () => {
    assert.equal(regionLabel(at), "lines 4-7");
    assert.equal(regionLabel({ oldStart: 3, oldLines: 1, newStart: 3, newLines: 0 }), "line 3 (deleted)");
    assert.equal(regionLabel({ oldStart: 1, oldLines: 0, newStart: 1, newLines: 2 }), "lines 1-2 (insertion)");
    assert.equal(regionLabel({ oldStart: 2, oldLines: 1, newStart: 2, newLines: 1 }), "line 2");
    assert.deepEqual(regionLines(at), { start: 3, end: 6 });
    assert.deepEqual(regionLines({ newStart: 3, newLines: 0 }), { start: 2, end: 2 });
});

test("the reason is escaped Markdown that still wraps, with the session below it", () => {
    assert.equal(mdProse("a *b*\nc"), "a \\*b\\*  \nc");
    const c = commentText(
        { id: "L3", file: "f", op: "edit", msg: "why", ts: "2026-09-25T20:00:00Z", user: "claude", session: "S1", before: "", after: "", line: 4, ...at },
        "T-1: work",
    );
    assert.match(c.author, /^L3 @ .+ claude:$/);
    assert.equal(c.body, "why\n\n---\n\n*session S1: T\\-1: work*\n\n&nbsp;");
    const loose = commentText({ id: "L1", file: "f", op: "edit", msg: "m", ts: "", user: "", session: null, before: "", after: "", line: 1, ...at }, null);
    assert.match(loose.body, /committed outside any session \\\(\\-\\-no\\-session\\\)/);
});
