/* One board for a project's folders: which board a folder works (an
 * override, a lap branch folder's parent, else the board above), the MCP
 * server writing to it from another folder, two processes writing through
 * the override at once, and the --branch a session start needs. */

import * as assert from "node:assert/strict";
import { spawn } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { branchArgs } from "../lap";
import { handle } from "../mcp";
import { Board, locateBoard } from "../store";

function tmp(): string {
    return fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "coboard-locate-")));
}

/* A parent folder with a board, and a branch folder beside it holding a
 * copy of the board and a lap branch that names the parent. */
function project(): { parent: string; branch: string } {
    const root = tmp();
    const parent = path.join(root, "proj");
    const branch = path.join(root, "proj-feat");
    new Board(parent).create({ kind: "epic", title: "The parent's epic" });
    fs.mkdirSync(path.join(branch, ".coboard"), { recursive: true });
    fs.copyFileSync(path.join(parent, ".coboard", "log.jsonl"), path.join(branch, ".coboard", "log.jsonl"));
    fs.mkdirSync(path.join(branch, ".lap"), { recursive: true });
    fs.writeFileSync(path.join(branch, ".lap", "lineage"), "0123456789ab\n");
    fs.writeFileSync(path.join(branch, ".lap", "parent"), parent + "\n");
    fs.mkdirSync(path.join(branch, "src"));
    return { parent, branch };
}

async function call(cwd: string, name: string, args: object) {
    const out = await handle({ jsonrpc: "2.0", id: 1, method: "tools/call", params: { name, arguments: args } }, { cwd, author: "agent" });
    const result = (out as { result: { content: { text: string }[]; isError?: boolean } }).result;
    return { error: result.isError === true, text: result.content[0].text };
}

test("locateBoard: an override first, then a lap branch's parent, then the board above", () => {
    const { parent, branch } = project();
    const other = tmp();
    new Board(other).create({ kind: "epic", title: "Elsewhere" });
    assert.deepEqual(locateBoard(path.join(branch, "src"), other), { root: other, via: "override" });
    assert.deepEqual(locateBoard(branch, path.join(other, ".coboard")), { root: other, via: "override" });
    assert.deepEqual(locateBoard(path.join(branch, "src"), ""), { root: parent, via: "lap-parent" });
    assert.deepEqual(locateBoard(parent, ""), { root: parent, via: "found" });
    // the branch's own copy is never the board: a parent with none yet is
    // where the first one goes, and a parent that is gone is stale
    const bare = tmp();
    fs.writeFileSync(path.join(branch, ".lap", "parent"), bare + "\n");
    assert.deepEqual(locateBoard(branch, ""), { root: null, via: "lap-parent", home: bare });
    const gone = path.join(other, "nothing-here");
    fs.writeFileSync(path.join(branch, ".lap", "parent"), gone + "\n");
    assert.deepEqual(locateBoard(branch, ""), { root: null, via: "stale-parent", stale: gone });
    assert.deepEqual(locateBoard(tmp(), ""), { root: null, via: "found" });
});

test("mcp: a branch folder whose parent is gone refuses reads and writes, naming the stale path; COBOARD_DIR still works", async () => {
    const { parent, branch } = project();
    const gone = path.join(path.dirname(parent), "moved-away");
    fs.writeFileSync(path.join(branch, ".lap", "parent"), gone + "\n");
    const saved = process.env["COBOARD_DIR"];
    delete process.env["COBOARD_DIR"];
    try {
        for (const [name, args] of [
            ["board_list", {}],
            ["board_get", { id: "E-1" }],
            ["board_create", { kind: "ticket", title: "Lost", epic: "E-1" }],
            ["board_update", { id: "E-1", title: "Renamed" }],
        ] as const) {
            const r = await call(branch, name, args);
            assert.equal(r.error, true, `${name} answered from the stale copy: ${r.text}`);
            assert.match(r.text, /stale_parent/);
            assert.ok(r.text.includes(gone), `${name} does not name the stale path`);
            assert.match(r.text, /COBOARD_DIR/);
        }
        assert.equal(new Board(branch).all().length, 1, "the branch's copy is untouched");
        process.env["COBOARD_DIR"] = parent;
        const listed = await call(branch, "board_list", {});
        assert.equal(listed.error, false, listed.text);
        assert.match(listed.text, /The parent's epic/);
        const created = await call(branch, "board_create", { kind: "ticket", title: "Through the override", epic: "E-1" });
        assert.equal(created.error, false, created.text);
        assert.ok(new Board(parent).all().some((i) => i.title === "Through the override"));
    } finally {
        if (saved === undefined) delete process.env["COBOARD_DIR"];
        else process.env["COBOARD_DIR"] = saved;
    }
});

test("mcp: a branch whose parent has no board yet creates the first one there", async () => {
    const { parent, branch } = project();
    fs.rmSync(path.join(parent, ".coboard"), { recursive: true });
    const saved = process.env["COBOARD_DIR"];
    delete process.env["COBOARD_DIR"];
    try {
        const created = await call(branch, "board_create", { kind: "epic", title: "First in the parent" });
        assert.equal(created.error, false, created.text);
        assert.deepEqual(new Board(parent).all().map((i) => i.title), ["First in the parent"]);
    } finally {
        if (saved === undefined) delete process.env["COBOARD_DIR"];
        else process.env["COBOARD_DIR"] = saved;
    }
});

test("locateBoard: a relative override is taken from the folder worked in, whatever the process's cwd; ~ is the home folder", () => {
    const { parent, branch } = project();
    const up = path.relative(branch, parent); /* "../proj" */
    const saved = process.cwd();
    try {
        process.chdir(os.tmpdir());
        assert.deepEqual(locateBoard(branch, up), { root: parent, via: "override" });
        assert.deepEqual(locateBoard(branch, path.join(up, ".coboard")), { root: parent, via: "override" });
    } finally {
        process.chdir(saved);
    }
    assert.deepEqual(locateBoard(branch, "~/somewhere"), { root: path.join(os.homedir(), "somewhere"), via: "override" });
    assert.deepEqual(locateBoard(branch, "~"), { root: os.homedir(), via: "override" });
});

test("locateBoard: a branch of a branch goes on up to main's board, past the branch between", () => {
    const { parent, branch } = project();
    const nested = path.join(path.dirname(branch), "proj-feat-sub");
    fs.mkdirSync(path.join(nested, ".coboard"), { recursive: true });
    fs.copyFileSync(path.join(branch, ".coboard", "log.jsonl"), path.join(nested, ".coboard", "log.jsonl"));
    fs.mkdirSync(path.join(nested, ".lap"), { recursive: true });
    fs.writeFileSync(path.join(nested, ".lap", "lineage"), "ba9876543210\n");
    fs.writeFileSync(path.join(nested, ".lap", "parent"), branch + "\n");
    assert.deepEqual(locateBoard(nested, ""), { root: parent, via: "lap-parent" });
    // parents that name each other are not followed for ever
    fs.writeFileSync(path.join(branch, ".lap", "parent"), nested + "\n");
    assert.equal(locateBoard(nested, "").via, "lap-parent");
});

test("mcp: in a branch folder, the tools read and write the parent's board", async () => {
    const { parent, branch } = project();
    const saved = process.env["COBOARD_DIR"];
    delete process.env["COBOARD_DIR"];
    try {
        const created = await call(path.join(branch, "src"), "board_create", { kind: "ticket", title: "Made in the branch", epic: "E-1" });
        assert.equal(created.error, false, created.text);
        assert.ok(new Board(parent).all().some((i) => i.title === "Made in the branch"), "the ticket is on the parent's board");
        assert.equal(new Board(branch).all().length, 1, "the branch's copy is untouched");
        const listed = await call(branch, "board_list", {});
        assert.ok(listed.text.includes("Made in the branch"));
    } finally {
        if (saved !== undefined) process.env["COBOARD_DIR"] = saved;
    }
});

test("mcp: COBOARD_DIR points the tools at another folder's board", async () => {
    const { parent } = project();
    const elsewhere = tmp();
    fs.mkdirSync(path.join(elsewhere, ".git"));
    const saved = process.env["COBOARD_DIR"];
    process.env["COBOARD_DIR"] = parent;
    try {
        const created = await call(elsewhere, "board_create", { kind: "ticket", title: "Pointed at the parent", epic: "E-1" });
        assert.equal(created.error, false, created.text);
        assert.ok(new Board(parent).all().some((i) => i.title === "Pointed at the parent"));
        assert.equal(fs.existsSync(path.join(elsewhere, ".coboard")), false, "no board was made where the agent runs");
    } finally {
        if (saved === undefined) delete process.env["COBOARD_DIR"];
        else process.env["COBOARD_DIR"] = saved;
    }
});

test("two processes writing one board through COBOARD_DIR lose nothing", async () => {
    const { parent } = project();
    const store = path.join(__dirname, "..", "store.js");
    const writer = (tag: string) =>
        new Promise<number>((resolve) => {
            const script = `const s = require(${JSON.stringify(store)});
const b = new s.Board(s.locateBoard(process.cwd()).root);
for (let i = 0; i < 25; i++) b.create({ kind: "ticket", title: "${tag} " + i, epic: "E-1" });`;
            const p = spawn(process.execPath, ["-e", script], { cwd: tmp(), env: { ...process.env, COBOARD_DIR: parent }, stdio: "ignore" });
            p.on("exit", (c) => resolve(c ?? 1));
        });
    const codes = await Promise.all([writer("a"), writer("b")]);
    assert.deepEqual(codes, [0, 0]);
    const tickets = new Board(parent).all().filter((i) => i.kind === "ticket");
    assert.equal(tickets.length, 50);
    assert.equal(new Set(tickets.map((t) => t.id)).size, 50, "no id was handed out twice");
});

test("branchArgs: the branch id in a branch folder, main where branches exist, else nothing", () => {
    const { parent, branch } = project();
    const saved = process.env["LAP_BRANCH"];
    delete process.env["LAP_BRANCH"];
    try {
        assert.deepEqual(branchArgs(path.join(branch, "src")), ["--branch", "0123456789ab"]);
        fs.mkdirSync(path.join(parent, ".lap"), { recursive: true });
        assert.deepEqual(branchArgs(parent), []);
        fs.writeFileSync(path.join(parent, ".lap", "branches.json"), "[]\n");
        assert.deepEqual(branchArgs(parent), []);
        fs.writeFileSync(path.join(parent, ".lap", "branches.json"), '[{"id":"0123456789ab"}]\n');
        assert.deepEqual(branchArgs(parent), ["--branch", "main"]);
        assert.deepEqual(branchArgs(tmp()), []);
        process.env["LAP_BRANCH"] = "feat";
        assert.deepEqual(branchArgs(branch), [], "LAP_BRANCH already says it");
    } finally {
        if (saved === undefined) delete process.env["LAP_BRANCH"];
        else process.env["LAP_BRANCH"] = saved;
    }
});

test("a board whose folder cannot be written fails at once, as a refusal, never waiting", { skip: process.getuid?.() === 0 }, async () => {
    const dir = tmp();
    new Board(dir).create({ kind: "epic", title: "Read-only" });
    fs.chmodSync(path.join(dir, ".coboard"), 0o555);
    try {
        const t0 = Date.now();
        assert.throws(() => new Board(dir).create({ kind: "epic", title: "Nope" }), (e: unknown) => (e as { code?: string }).code === "unwritable");
        assert.ok(Date.now() - t0 < 1000, "failed promptly, not after the lock's wait");
        const saved = process.env["COBOARD_DIR"];
        process.env["COBOARD_DIR"] = dir;
        try {
            const r = await call(tmp(), "board_create", { kind: "epic", title: "Nope" });
            assert.equal(r.error, true);
            assert.match(r.text, /is the folder writable/);
        } finally {
            if (saved === undefined) delete process.env["COBOARD_DIR"];
            else process.env["COBOARD_DIR"] = saved;
        }
    } finally {
        fs.chmodSync(path.join(dir, ".coboard"), 0o755);
    }
});
