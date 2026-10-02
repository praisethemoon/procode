/* The History view's wire, driven through the real extension with a
 * stand-in for the `vscode` module: the view asks with a filter and a page,
 * the host answers with that page of the log, and answers again when the log
 * grows or the grouping changes. A reference followed in the view is resolved
 * by lap, or by the log where lap cannot answer, and answered with the page
 * that shows its commit. */

import * as assert from "node:assert/strict";
import { createHash } from "node:crypto";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import type { ToView } from "../protocol";

// eslint-disable-next-line @typescript-eslint/no-require-imports
const Module = require("node:module") as { _load: (req: string, parent: unknown, isMain: boolean) => unknown };

const root = fs.mkdtempSync(path.join(os.tmpdir(), "lap-history-"));
// the history as lap keeps it: chunk files, read in order as one stream
fs.mkdirSync(path.join(root, ".lap", "log"), { recursive: true });
const logPath = path.join(root, ".lap", "log", "main.000001.jsonl");
const nextChunk = path.join(root, ".lap", "log", "main.000002.jsonl");
const rec = (o: object) => JSON.stringify(o) + "\n";
const commit = (id: string, session: string, intent: string) =>
    rec({ type: "commit", id, session, file: "a.ts", op: "edit", user: "claude",
        old_start: 1, old_lines: 1, new_start: 1, new_lines: 1, eof_nl: true, old_text: ["a"], new_text: ["b"],
        intent, behavior: `step ${id}`, ts: new Date().toISOString() });
fs.writeFileSync(
    logPath,
    rec({ type: "session_start", id: "S1", msg: "T-1: parser", ts: new Date().toISOString() }) +
        commit("L1", "S1", "fix precedence") +
        commit("L2", "S1", "a test for it"),
);

const commands = new Map<string, (...a: unknown[]) => unknown>();
const warnings: string[] = [];
const state = new Map<string, unknown>();
let provider: { resolveWebviewView(view: unknown): void } | null = null;
let onChange: (() => void) | null = null;

class EventEmitter {
    event = () => ({ dispose() {} });
    fire() {}
}
const fake = {
    EventEmitter,
    StatusBarAlignment: { Left: 1 },
    Uri: {
        joinPath: (...p: { fsPath?: string }[]) => ({ fsPath: p.map((x) => x.fsPath ?? String(x)).join("/") }),
        from: (c: object) => c,
    },
    workspace: {
        workspaceFolders: [{ uri: { fsPath: root } }],
        registerTextDocumentContentProvider: () => ({ dispose() {} }),
        getConfiguration: () => ({ get: () => undefined }),
        createFileSystemWatcher: () => ({
            onDidChange: (fn: () => void) => (onChange = fn),
            onDidCreate() {},
            onDidDelete() {},
            dispose() {},
        }),
        onDidChangeWorkspaceFolders: () => ({ dispose() {} }),
        onDidChangeConfiguration: () => ({ dispose() {} }),
    },
    window: {
        registerWebviewViewProvider: (id: string, p: typeof provider) => {
            assert.equal(id, "lapHistory");
            provider = p;
            return { dispose() {} };
        },
        createStatusBarItem: () => ({ show() {}, hide() {}, dispose() {} }),
        onDidChangeWindowState: () => ({ dispose() {} }),
        showWarningMessage: (m: string) => void warnings.push(m),
    },
    comments: { createCommentController: () => ({ dispose() {} }) },
    commands: {
        registerCommand: (name: string, fn: (...a: unknown[]) => unknown) => {
            commands.set(name, fn);
            return { dispose() {} };
        },
        executeCommand: async () => undefined,
    },
};

test("the view asks for a page, and is sent it again when the log grows or the grouping changes", async () => {
    const load = Module._load;
    Module._load = (req, parent, isMain) => (req === "vscode" ? fake : load(req, parent, isMain));
    try {
        // eslint-disable-next-line @typescript-eslint/no-require-imports
        const ext = require("../extension") as { activate(ctx: unknown): void };
        ext.activate({
            subscriptions: [],
            extensionUri: { fsPath: "/ext" },
            workspaceState: { get: (k: string, d: unknown) => (state.has(k) ? state.get(k) : d), update: async (k: string, v: unknown) => void state.set(k, v) },
        });
    } finally {
        Module._load = load;
    }
    for (const c of ["lap.refresh", "lap.toggleGrouping", "lap.collapseAll", "lap.showCommit", "lap.revealCommit"]) {
        assert.ok(commands.has(c), `${c} is registered`);
    }

    const posted: ToView[] = [];
    let fromView: ((m: unknown) => void) | null = null;
    provider!.resolveWebviewView({
        webview: {
            options: {},
            html: "",
            cspSource: "vscode-resource:",
            asWebviewUri: (u: { fsPath: string }) => u.fsPath,
            postMessage: async (m: ToView) => void posted.push(m),
            onDidReceiveMessage: (fn: (m: unknown) => void) => (fromView = fn),
        },
        onDidDispose() {},
    });

    fromView!({ type: "query", filter: { text: "", range: "recent", ops: [], users: [], states: [] }, page: 0 });
    const first = posted.at(-1)!;
    assert.ok(first.type === "page" && first.page);
    assert.equal(first.active, "S1");
    assert.deepEqual(first.page.sessions.map((s) => `${s.id}:${s.commits.map((c) => c.id).join(",")}`), ["S1:L2,L1"]);

    // The log grows into a new chunk (the first one sealed at the limit);
    // the watcher fires; the same query is answered again.
    fs.appendFileSync(nextChunk, commit("L3", "S1", "and another"));
    onChange!();
    await new Promise((r) => setTimeout(r, 300));
    const grown = posted.at(-1)!;
    assert.ok(grown.type === "page" && grown.page);
    assert.deepEqual(grown.page.sessions[0].commits.map((c) => c.id), ["L3", "L2", "L1"]);

    // A filter narrows; the raw list is one toggle away.
    fromView!({ type: "query", filter: { text: "test", range: "recent", ops: [], users: [], states: [] }, page: 0 });
    const narrowed = posted.at(-1)!;
    assert.ok(narrowed.type === "page" && narrowed.page);
    assert.deepEqual(narrowed.page.sessions[0].commits.map((c) => c.id), ["L2"]);
    await commands.get("lap.toggleGrouping")!();
    const raw = posted.at(-1)!;
    assert.ok(raw.type === "page" && raw.page && !raw.page.grouped);
    assert.deepEqual(raw.page.commits.map((c) => c.id), ["L2"]);

    commands.get("lap.collapseAll")!();
    assert.equal(posted.at(-1)!.type, "collapseAll");

    // A reference: lap names the commit. The filter ("test") hides L1, so
    // the answer is All with nothing else set, on L1's page.
    const bin = fs.mkdtempSync(path.join(os.tmpdir(), "lap-bin-"));
    const args = path.join(bin, "args");
    fs.writeFileSync(path.join(bin, "lap"), `#!/bin/sh\nprintf '%s\\n' "$@" > '${args}'\necho '{"ok":true,"id":"L1"}'\n`, { mode: 0o755 });
    const nolap = fs.mkdtempSync(path.join(os.tmpdir(), "lap-nobin-"));
    const PATH = process.env.PATH;
    const revealed = async (ref: string) => {
        const before = posted.length;
        fromView!({ type: "reveal", ref });
        for (let i = 0; i < 100 && posted.length === before && warnings.length === 0; i++) await new Promise((r) => setTimeout(r, 20));
        const m = posted.at(-1)!;
        return posted.length > before && m.type === "page" ? m : null;
    };
    try {
        // The stand-in lap is a #! script, which Windows cannot start.
        if (process.platform !== "win32") {
            process.env.PATH = bin;
            const byLap = await revealed("#abcdef0");
            assert.deepEqual(fs.readFileSync(args, "utf8").split("\n"), ["show", "#abcdef0", "--json", ""]);
            assert.ok(byLap && byLap.page && byLap.reveal);
            assert.equal(byLap.reveal.id, "L1");
            assert.deepEqual(byLap.reveal.filter, { text: "", range: "all", ops: [], users: [], states: [] });
            assert.deepEqual(byLap.page.commits.map((c) => c.id), ["L3", "L2", "L1"]);
        }

        // No lap: the log's own hashes resolve a prefix.
        process.env.PATH = nolap;
        const l3 = createHash("sha256").update(fs.readFileSync(nextChunk, "utf8").trimEnd().split("\n").at(-1)!).digest("hex");
        const byLog = await revealed("#" + l3.slice(0, 7).toUpperCase());
        assert.ok(byLog && byLog.page && byLog.reveal);
        assert.equal(byLog.reveal.id, "L3");
        assert.equal(byLog.page.commits[0].hash, l3);

        // A reference to nothing is a warning, and the view stays as it is.
        assert.equal(await revealed("L99"), null);
        assert.deepEqual(warnings, ["lap: no commit is named L99"]);
    } finally {
        process.env.PATH = PATH;
    }
});
