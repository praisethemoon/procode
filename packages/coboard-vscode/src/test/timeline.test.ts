import * as assert from "node:assert/strict";
import { test } from "node:test";

import { TimelineStep, areaOf, gapLabel, linesOf, timeline } from "../timeline";

const at = "2026-09-26T08:43:55Z";
const step = (id: string, file: string, msg: string, extra: Partial<TimelineStep> = {}): TimelineStep => ({
    id, ts: at, file, op: "edit", new_start: 10, new_lines: 3, msg, ...extra,
});
const shape = (items: ReturnType<typeof timeline>) =>
    items.map((i) => (i.kind === "node" ? `${i.op}:${i.file.split("/").pop()}×${i.steps.length}` : i.kind === "move" ? `→${i.area}` : `gap`));

test("fragments of one change are one node; the message is shown once", () => {
    const items = timeline([
        step("L999", "packages/index-vscode/src/pdf.ts", "A paper from its PDF", { op: "create" }),
        step("L1000", "packages/index-vscode/src/commands.ts", "(1/4) Add Files takes PDFs"),
        step("L1001", "packages/index-vscode/src/commands.ts", "(2/4) Add Files takes PDFs"),
        step("L1002", "packages/index-vscode/src/commands.ts", "(3/4) Add Files takes PDFs"),
        step("L1003", "packages/index-vscode/src/commands.ts", "(4/4) Add Files takes PDFs"),
        step("L1004", "packages/index-vscode/scripts/assets.mjs", "The build copies pdf.js"),
    ]);
    assert.deepEqual(shape(items), ["create:pdf.ts×1", "edit:commands.ts×4", "edit:assets.mjs×1"]);
    const n = items[1];
    assert.ok(n.kind === "node" && n.msg === "Add Files takes PDFs");
});

test("a marker where the work moves to another area, and not before the first node", () => {
    const items = timeline([
        step("L1", "packages/index-vscode/src/a.ts", "a"),
        step("L2", "packages/index-vscode/package.json", "b"),
        step("L3", "packages/combined/scripts/build.mjs", "c"),
        step("L4", "specs/index-ui.md", "d"),
        step("L5", "README.md", "e"),
    ]);
    assert.deepEqual(shape(items), ["edit:a.ts×1", "edit:package.json×1", "→packages/combined", "edit:build.mjs×1", "→specs", "edit:index-ui.md×1", "→", "edit:README.md×1"]);
    assert.equal(areaOf("cli/kb-cli/src/main.c"), "cli/kb-cli");
    assert.equal(areaOf("packages/x"), "packages");
});

test("time appears only as a gap longer than a few minutes, and a gap ends a node", () => {
    const burst = timeline([step("L1", "a/x.ts", "(1/2) m"), step("L2", "a/x.ts", "(2/2) m")]);
    assert.deepEqual(shape(burst), ["edit:x.ts×2"], "a session recorded in one burst has no times");
    const slow = timeline([
        step("L1", "a/x.ts", "(1/2) m", { ts: "2026-09-26T08:00:00Z" }),
        step("L2", "a/x.ts", "(2/2) m", { ts: "2026-09-26T08:12:00Z" }),
        step("L3", "a/y.ts", "n", { ts: "2026-09-26T08:13:00Z" }),
    ]);
    assert.deepEqual(shape(slow), ["edit:x.ts×1", "gap", "edit:x.ts×1", "edit:y.ts×1"]);
    const g = slow[1];
    assert.ok(g.kind === "gap" && gapLabel(g.ms) === "12 min later");
    assert.equal(gapLabel(3 * 3_600_000), "3 h later");
    assert.equal(gapLabel(3 * 86_400_000), "3 days later");
});

test("a node that ends by deleting its file reads as a deletion; lines read plainly", () => {
    const items = timeline([step("L1", "a/x.ts", "retire"), step("L2", "a/x.ts", "retire", { op: "delete", new_lines: 0 })]);
    assert.deepEqual(shape(items), ["delete:x.ts×2"]);
    assert.equal(linesOf(step("L", "f", "m", { new_start: 29, new_lines: 1 })), "29");
    assert.equal(linesOf(step("L", "f", "m", { new_start: 232, new_lines: 18 })), "232–249");
    assert.equal(linesOf(step("L", "f", "m", { op: "delete", new_lines: 0 })), "deleted");
});
