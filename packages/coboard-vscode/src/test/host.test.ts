/* The bundled extension host, activated against a minimal stand-in for the
 * `vscode` module: enough to prove it registers its commands, builds the tree
 * from a real board, and answers a tab's first message with the item's view. */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

// eslint-disable-next-line @typescript-eslint/no-require-imports
const Module = require("node:module") as { _load: (req: string, parent: unknown, isMain: boolean) => unknown };

import { Board } from "coboard";

const root = fs.mkdtempSync(path.join(os.tmpdir(), "coboard-host-"));
const commands = new Map<string, (...a: unknown[]) => unknown>();
const posted: unknown[] = [];
let provider: { getChildren(n?: unknown): { item: { id: string }; label: string; description: string }[] } | null = null;
let onMessage: ((m: unknown) => void) | null = null;

class EventEmitter {
    event = () => ({ dispose() {} });
    fire() {}
}
const fake = {
    TreeItem: class {
        constructor(public label: string, public collapsibleState: number) {}
    },
    ThemeIcon: class {
        constructor(public id: string) {}
    },
    EventEmitter,
    TreeItemCollapsibleState: { None: 0, Collapsed: 1, Expanded: 2 },
    ViewColumn: { Active: -1 },
    Uri: { joinPath: (...p: { fsPath?: string }[]) => ({ fsPath: p.map((x) => x.fsPath ?? String(x)).join("/") }) },
    window: {
        createTreeView: (_id: string, o: { treeDataProvider: typeof provider }) => {
            provider = o.treeDataProvider;
            return { dispose() {} };
        },
        createWebviewPanel: () => ({
            title: "",
            reveal() {},
            onDidDispose() {},
            webview: {
                cspSource: "vscode-resource:",
                html: "",
                asWebviewUri: (u: { fsPath: string }) => u.fsPath,
                postMessage: (m: unknown) => posted.push(m),
                onDidReceiveMessage: (fn: (m: unknown) => void) => (onMessage = fn),
            },
        }),
        showErrorMessage: (m: string) => assert.fail(`the host reported an error: ${m}`),
    },
    commands: {
        registerCommand: (name: string, fn: (...a: unknown[]) => unknown) => {
            commands.set(name, fn);
            return { dispose() {} };
        },
        executeCommand: (name: string, ...a: unknown[]) => commands.get(name)?.(...a),
    },
    workspace: {
        workspaceFolders: [{ uri: { scheme: "file", fsPath: root } }],
        getConfiguration: () => ({ get: (_k: string, d: unknown) => d }),
        createFileSystemWatcher: () => ({ onDidChange() {}, onDidCreate() {}, onDidDelete() {}, dispose() {} }),
        onDidChangeWorkspaceFolders: () => ({ dispose() {} }),
    },
};

test("the bundled host activates, draws the tree and serves a tab", async () => {
    const b = new Board(root);
    b.create({ kind: "epic", title: "Epic" });
    b.create({ kind: "milestone", title: "Mile", epic: "E-1" });
    b.create({ kind: "ticket", title: "In milestone", milestone: "M-1" });
    b.create({ kind: "ticket", title: "Dangling", epic: "E-1" });

    const load = Module._load;
    Module._load = (req, parent, isMain) => (req === "vscode" ? fake : load(req, parent, isMain));
    try {
        const ext = require(path.resolve(__dirname, "..", "extension.js")) as { activate(ctx: unknown): void };
        ext.activate({ subscriptions: [], extensionUri: { fsPath: "/ext" } });
    } finally {
        Module._load = load;
    }

    for (const c of ["coboard.newEpic", "coboard.newMilestone", "coboard.newTicket", "coboard.open", "coboard.goTo", "coboard.delete", "coboard.startSession", "coboard.refresh"]) {
        assert.ok(commands.has(c), `${c} is registered`);
    }
    const epics = provider!.getChildren();
    assert.deepEqual(epics.map((n) => n.item.id), ["E-1"]);
    const under = provider!.getChildren(epics[0]);
    assert.deepEqual(under.map((n) => n.item.id), ["M-1", "T-2"], "milestones, then the epic's tickets in no milestone");
    assert.deepEqual(provider!.getChildren(under[0]).map((n) => n.item.id), ["T-1"]);

    await commands.get("coboard.open")!("M-1");
    onMessage!({ type: "ready" });
    const data = posted.find((m) => (m as { type: string }).type === "data") as { view: { kind: string; tickets: { id: string }[] } };
    assert.equal(data.view.kind, "milestone");
    assert.deepEqual(data.view.tickets.map((t) => t.id), ["T-1"]);

    onMessage!({ type: "create", kind: "ticket", title: "From the tab", milestone: "M-1" });
    await new Promise((r) => setTimeout(r, 10));
    assert.equal(b.get("T-3").title, "From the tab");
    onMessage!({ type: "comment", ticket: "T-3", body: "hi" });
    await new Promise((r) => setTimeout(r, 10));
    const t = b.get("T-3");
    assert.ok(t.kind === "ticket" && t.comments.length === 1);
});
