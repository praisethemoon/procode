/* The bundled extension host, activated against a minimal stand-in for the
 * `vscode` module: enough to prove it registers its commands, builds the tree
 * from a real board, and answers a tab's first message with the item's view. */

import * as assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
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
const executed: unknown[][] = [];
let content: { provideTextDocumentContent(uri: { toString(): string }): string } | null = null;
const LAP = path.resolve(__dirname, "../../../../cli/lap-cli/bin/lap");
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
    Range: class {
        constructor(public startLine: number, public startCharacter: number, public endLine: number, public endCharacter: number) {}
    },
    Uri: {
        joinPath: (...p: { fsPath?: string }[]) => ({ fsPath: p.map((x) => x.fsPath ?? String(x)).join("/") }),
        from: (c: { scheme: string; path: string }) => ({ ...c, toString: () => `${c.scheme}:${c.path}` }),
    },
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
        executeCommand: (name: string, ...a: unknown[]) => {
            executed.push([name, ...a]);
            return commands.get(name)?.(...a);
        },
    },
    workspace: {
        workspaceFolders: [{ uri: { scheme: "file", fsPath: root } }],
        getConfiguration: () => ({ get: (k: string, d: unknown) => (k === "lapPath" ? LAP : d) }),
        registerTextDocumentContentProvider: (_scheme: string, p: typeof content) => {
            content = p;
            return { dispose() {} };
        },
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

test("clicking a lap edit on a ticket opens it as a diff at the edited line", { skip: !fs.existsSync(LAP) && "lap is not built" }, async () => {
    const lap = (...args: string[]) => execFileSync(LAP, args, { cwd: root, env: { ...process.env, LAP_USER: "t" } });
    lap("init");
    lap("session", "start", "T-1: work", "--meta", "ticket=T-1");
    fs.writeFileSync(path.join(root, "x.c"), "a\nb\nc\n");
    lap("commit", "x.c", "-m", "first");
    fs.writeFileSync(path.join(root, "x.c"), "a\nB\nc\n");
    lap("commit", "x.c", "-m", "capital b");
    lap("session", "end");

    onMessage!({ type: "showEdit", commit: "L2" });
    await new Promise((r) => setTimeout(r, 300));
    const diff = executed.find((c) => c[0] === "vscode.diff");
    assert.ok(diff, "vscode.diff was opened");
    const [, left, right, title, opts] = diff as [string, { path: string }, { path: string }, string, { selection: { startLine: number } }];
    assert.equal(left.path, "/L2/before/x.c");
    assert.equal(right.path, "/L2/after/x.c");
    assert.match(title, /^L2 x\.c — capital b$/);
    assert.equal(opts.selection.startLine, 1, "scrolled to line 2 (0-based 1)");
    assert.equal(content!.provideTextDocumentContent(left as never), "a\nb\nc\n");
    assert.equal(content!.provideTextDocumentContent(right as never), "a\nB\nc\n");
});
