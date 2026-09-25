import * as assert from "node:assert/strict";
import { test } from "node:test";

import { outcomeMessage, refreshPlan } from "../refresh";

test("a document is refreshed from what its locator names", () => {
    assert.deepEqual(refreshPlan("https://learn.microsoft.com/iocp"), {
        kind: "url",
        url: "https://learn.microsoft.com/iocp",
        host: "learn.microsoft.com",
    });
    assert.deepEqual(refreshPlan("/Users/ana/notes/iocp.md"), { kind: "file", path: "/Users/ana/notes/iocp.md" });
    assert.deepEqual(refreshPlan("C:\\notes\\iocp.md"), { kind: "file", path: "C:\\notes\\iocp.md" });
});

test("content handed over directly has nothing to be refreshed from, and says so", () => {
    const inline = refreshPlan("inline:95d89a8f");
    assert.equal(inline.kind, "none");
    assert.match(inline.kind === "none" ? inline.why : "", /handed over directly/);
    assert.equal(refreshPlan("").kind, "none");
    assert.equal(refreshPlan("notes/relative.md").kind, "none");
    assert.equal(refreshPlan("ftp://example.com/a").kind, "none");
});

test("the outcome is told plainly", () => {
    assert.match(outcomeMessage("D-3", { outcome: "unchanged", document: "D-3", fetchedAt: "2026-09-26T00:00:00Z" }), /unchanged.*2026-09-26/);
    assert.match(outcomeMessage("D-3", { outcome: "updated", document: "D-3", fetchedAt: "" }), /re-indexed/);
    assert.match(outcomeMessage("D-3", { outcome: "updated", document: "D-9", fetchedAt: "" }), /re-filed as D-9/);
    assert.match(outcomeMessage("D-3", { outcome: "cannot", why: "no source." }), /cannot be refreshed: no source/);
    assert.match(outcomeMessage("D-3", { outcome: "declined" }), /not refreshed/);
});
