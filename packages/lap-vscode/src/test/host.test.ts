/* The History view's wire, driven through the real extension with a
 * stand-in for the `vscode` module: the view asks with a filter and a page,
 * the host answers with that page of the log, and answers again when the log
 * grows or the grouping changes. */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import type { ToView } from "../protocol";

// eslint-disable-next-line @typescript-eslint/no-require-imports
const Module = require("node:module") as { _load: (req: string, parent: unknown, isMain: boolean) => unknown };

const root = fs.mkdtempSync(path.join(os.tmpdir(), "lap-history-"));
fs.mkdirSync(path.join(root, ".lap"));
const logPath = path.join(root, ".lap", "log.jsonl");
const rec = (o: object) => JSON.stringify(o) + "\n";
const commit = (id: string, session: string, msg: string) =>
    rec({ type: "commit", id, session, file: "a.ts", op: "edit", user: "claude", msg, ts: new Date().toISOString(),
        old_start: 1, old_lines: 1, new_start: 1, new_lines: 1, eof_nl: true, old_text: ["a"], new_text: ["b"] });
fs.writeFileSync(
    logPath,
    rec({ type: "session_start", id: "S1", msg: "T-1: parser", ts: new Date().toISOString() }) +
        commit("L1", "S1", "fix precedence") +
        commit("L2", "S1", "a test for it"),
);

const commands = new Map<string, (...a: unknown[]) => unknown>();
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
        createFileSystemWatcher: () => ({
            onDidChange: (fn: () => void) => (onChange = fn),
            onDidCreate() {},
            onDidDelete() {},
            dispose() {},
        }),
        onDidChangeWorkspaceFolders: () => ({ dispose() {} }),
    },
    window: {
        registerWebviewViewProvider: (id: string, p: typeof provider) => {
            assert.equal(id, "lapHistory");
            provider = p;
            return { dispose() {} };
        },
        createStatusBarItem: () => ({ show() {}, hide() {}, dispose() {} }),
        showWarningMessage: () => undefined,
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
    for (const c of ["lap.refresh", "lap.toggleGrouping", "lap.collapseAll", "lap.showCommit"]) {
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

    // The log grows; the watcher fires; the same query is answered again.
    fs.appendFileSync(logPath, commit("L3", "S1", "and another"));
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
});
