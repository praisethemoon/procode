import * as assert from "node:assert/strict";
import { test } from "node:test";

import { folderMessage, outcomeMessage, refreshPlan } from "../refresh";

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

test("a folder's filing is told as its totals, and a file gone is named by what happened to it", () => {
    const filed = {
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
    };
    assert.equal(
        folderMessage(filed),
        "Filed kb-cli into code as S-4: 92 files, 3 added, 1 updated, 88 unchanged, 1 forgotten, 4 skipped.",
    );
    assert.match(
        folderMessage({ ...filed, files: 1, forgotten: [], missing: ["src/old.c"] }),
        /1 file, .*0 forgotten.*1 file gone from the folder is still filed: src\/old\.c\./,
    );
    assert.match(
        folderMessage({ ...filed, source: null, files: 0, added: 0, unchanged: 0, updated: 0, forgotten: [] }),
        /^kb-cli holds no file to file into code; 4 files skipped\.$/,
    );
    assert.match(folderMessage({ ...filed, root: "C:\\work\\notes" }), /^Filed notes into code/);
});
