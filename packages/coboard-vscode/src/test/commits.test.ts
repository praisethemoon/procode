import * as assert from "node:assert/strict";
import { test } from "node:test";

import type { LapCommit } from "coboard/lap";

import { baseMessage, groupCommits, splitPath } from "../commits";

const c = (id: string, file: string, msg: string, op = "edit"): LapCommit => ({ id, ts: "", user: "claude", file, op, msg });

test("numbered fragments of one change to one file are one group, oldest first", () => {
    // lap lists newest first.
    const newestFirst = [
        c("L6", "a.ts", "Tidy."),
        c("L5", "views.tsx", "(3/3) The toggle."),
        c("L4", "views.tsx", "(2/3) The toggle."),
        c("L3", "views.tsx", "(1/3) The toggle."),
        c("L2", "kanban.ts", "Columns.", "create"),
        c("L1", "a.ts", "Tidy."),
    ];
    const groups = groupCommits(newestFirst);
    assert.deepEqual(
        groups.map((g) => [g.file, g.msg, g.commits.map((x) => x.id)]),
        [
            ["a.ts", "Tidy.", ["L1"]],
            ["kanban.ts", "Columns.", ["L2"]],
            ["views.tsx", "The toggle.", ["L3", "L4", "L5"]],
            ["a.ts", "Tidy.", ["L6"]],
        ],
        "a group is consecutive edits only: the same message later on is its own row",
    );
    assert.equal(groups[1].op, "create");
});

test("the same file with different messages stays apart", () => {
    const groups = groupCommits([c("L2", "x.c", "Second reason."), c("L1", "x.c", "First reason.")]);
    assert.equal(groups.length, 2);
});

test("a message is only stripped of a leading fragment number", () => {
    assert.equal(baseMessage("(12/40) Moved."), "Moved.");
    assert.equal(baseMessage("Moved (1/2) things."), "Moved (1/2) things.");
    assert.deepEqual(splitPath("packages/coboard-vscode/webview/views.tsx"), { name: "views.tsx", dir: "packages/coboard-vscode/webview" });
    assert.deepEqual(splitPath("README.md"), { name: "README.md", dir: "" });
});
