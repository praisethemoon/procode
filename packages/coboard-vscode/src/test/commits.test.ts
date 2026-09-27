import * as assert from "node:assert/strict";
import { test } from "node:test";

import type { LapCommit } from "coboard/lap";

import { groupCommits, shortHash, splitPath } from "../commits";

const c = (id: string, file: string, intent: string, behavior: string, extra: Partial<LapCommit> = {}): LapCommit => ({
    id, hash: id.slice(1).padStart(64, "a"), ts: "", user: "claude", session: "S1", file, op: "edit", intent, behavior, ...extra,
});

test("consecutive edits to one file with one intent are one group, oldest first, each with its behavior", () => {
    // lap lists newest first.
    const newestFirst = [
        c("L6", "a.ts", "Tidy.", "The helper loses its unused argument."),
        c("L5", "views.tsx", "The toggle.", "The toggle's label follows the mode."),
        c("L4", "views.tsx", "The toggle.", "Clicking the toggle switches the mode."),
        c("L3", "views.tsx", "The toggle.", "A toggle is drawn beside the heading."),
        c("L2", "kanban.ts", "Columns.", "Tickets are sorted into a column per status.", { op: "create" }),
        c("L1", "a.ts", "Tidy.", "The helper's import is sorted."),
    ];
    const groups = groupCommits(newestFirst);
    assert.deepEqual(
        groups.map((g) => [g.file, g.intent, g.commits.map((x) => `${x.id}: ${x.behavior}`)]),
        [
            ["a.ts", "Tidy.", ["L1: The helper's import is sorted."]],
            ["kanban.ts", "Columns.", ["L2: Tickets are sorted into a column per status."]],
            [
                "views.tsx",
                "The toggle.",
                ["L3: A toggle is drawn beside the heading.", "L4: Clicking the toggle switches the mode.", "L5: The toggle's label follows the mode."],
            ],
            ["a.ts", "Tidy.", ["L6: The helper loses its unused argument."]],
        ],
        "a group is consecutive edits only: the same intent later on is its own row",
    );
    assert.equal(groups[1].op, "create");
});

test("the same file with different intents stays apart, and one intent across files is a group per file", () => {
    assert.equal(groupCommits([c("L2", "x.c", "Second reason.", "b"), c("L1", "x.c", "First reason.", "a")]).length, 2);
    const across = groupCommits([c("L3", "x.c", "One goal.", "c"), c("L2", "y.c", "One goal.", "b"), c("L1", "x.c", "One goal.", "a")]);
    assert.deepEqual(across.map((g) => g.commits.length), [1, 1, 1]);
});

test("a numbered prefix is part of the intent: nothing is stripped", () => {
    const groups = groupCommits([c("L2", "x.c", "(2/2) Moved.", "b"), c("L1", "x.c", "(1/2) Moved.", "a")]);
    assert.deepEqual(groups.map((g) => g.intent), ["(1/2) Moved.", "(2/2) Moved."]);
});

test("a forced commit keeps its mark inside its group", () => {
    const groups = groupCommits([c("L2", "x.c", "Goal.", "b", { forced: true }), c("L1", "x.c", "Goal.", "a")]);
    assert.deepEqual(groups[0].commits.map((x) => x.forced === true), [false, true]);
});

test("short hashes and paths", () => {
    assert.equal(shortHash("085576b2d5a664f2b60955c82e335bc2e94f029086b5f6a811a69cf8d750d630"), "085576b");
    assert.deepEqual(splitPath("packages/coboard-vscode/webview/views.tsx"), { name: "views.tsx", dir: "packages/coboard-vscode/webview" });
    assert.deepEqual(splitPath("README.md"), { name: "README.md", dir: "" });
});
