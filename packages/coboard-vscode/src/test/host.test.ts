/* The bundled extension host, activated against a minimal stand-in for the
 * `vscode` module: enough to prove it registers its commands, sends the
 * sidebar the board it draws its tree from, and answers a tab's first message
 * with the item's view. */

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
const threads: { uri: { path: string }; range: { startLine: number; endLine: number }; comments: { author: { name: string }; body: { value: string } }[]; canReply: boolean; label: string }[] = [];
let content: { provideTextDocumentContent(uri: { toString(): string }): string } | null = null;
const LAP = path.resolve(__dirname, "../../../../cli/lap-cli/bin/lap");
let sidebar: { resolveWebviewView(view: unknown): void } | null = null;
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
    CommentMode: { Editing: 0, Preview: 1 },
    CommentThreadCollapsibleState: { Collapsed: 0, Expanded: 1 },
    MarkdownString: class {
        constructor(public value: string) {}
    },
    comments: {
        createCommentController: () => ({
            dispose() {},
            createCommentThread: (uri: unknown, range: unknown, comments: unknown[]) => {
                const t = { uri, range, comments, canReply: true, collapsibleState: 0, label: "" } as unknown as (typeof threads)[number];
                threads.push(t);
                return t;
            },
        }),
    },
    Range: class {
        constructor(public startLine: number, public startCharacter: number, public endLine: number, public endCharacter: number) {}
    },
    Uri: {
        joinPath: (...p: { fsPath?: string }[]) => ({ fsPath: p.map((x) => x.fsPath ?? String(x)).join("/") }),
        from: (c: { scheme: string; path: string }) => ({ ...c, toString: () => `${c.scheme}:${c.path}` }),
    },
    window: {
        registerWebviewViewProvider: (id: string, p: typeof sidebar) => {
            assert.equal(id, "coboard.tree");
            sidebar = p;
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
        ext.activate({ subscriptions: [], extensionUri: { fsPath: "/ext" }, workspaceState: { get: () => undefined, update: async () => undefined } });
    } finally {
        Module._load = load;
    }

    for (const c of ["coboard.newEpic", "coboard.newMilestone", "coboard.newTicket", "coboard.open", "coboard.delete", "coboard.startSession", "coboard.refresh", "coboard.collapseAll"]) {
        assert.ok(commands.has(c), `${c} is registered`);
    }
    assert.equal(commands.has("coboard.goTo"), false, "the filter bar replaces Go to Item");

    // The sidebar: on its first message it is sent the whole board.
    const toSidebar: { type: string; items?: { id: string }[]; hasFolder?: boolean }[] = [];
    let fromSidebar: ((m: unknown) => void) | null = null;
    sidebar!.resolveWebviewView({
        webview: {
            options: {},
            html: "",
            cspSource: "vscode-resource:",
            asWebviewUri: (u: { fsPath: string }) => u.fsPath,
            postMessage: (m: (typeof toSidebar)[number]) => toSidebar.push(m),
            onDidReceiveMessage: (fn: (m: unknown) => void) => (fromSidebar = fn),
        },
        onDidDispose() {},
    });
    fromSidebar!({ type: "ready" });
    const tree = toSidebar.find((m) => m.type === "items")!;
    assert.equal(tree.hasFolder, true);
    assert.deepEqual(tree.items!.map((i) => i.id), ["E-1", "M-1", "T-1", "T-2"]);
    commands.get("coboard.collapseAll")!();
    assert.equal(toSidebar.at(-1)!.type, "collapseAll");

    // A right-click command gets the row's data-vscode-context.
    await commands.get("coboard.open")!({ webviewSection: "milestone", id: "M-1", preventDefaultContextMenuItems: true });
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

    onMessage!({ type: "showEdit", commit: "L2", sessionMsg: "T-1: work" });
    await new Promise((r) => setTimeout(r, 300));
    const diff = executed.find((c) => c[0] === "vscode.diff");
    assert.ok(diff, "vscode.diff was opened");
    const [, left, right, title, opts] = diff as [string, { path: string }, { path: string }, string, { selection: { startLine: number } }];
    assert.equal(left.path, "/L2/before/x.c");
    assert.equal(right.path, "/L2/after/x.c");
    assert.equal(title, "L2 · x.c");
    assert.equal(opts.selection.startLine, 1, "scrolled to line 2 (0-based 1)");
    assert.equal(content!.provideTextDocumentContent(left as never), "a\nb\nc\n");
    assert.equal(content!.provideTextDocumentContent(right as never), "a\nB\nc\n");

    // The reason for the edit sits on the changed line, as in Lap History.
    assert.equal(threads.length, 1);
    const t = threads[0];
    assert.equal(t.uri.path, "/L2/after/x.c");
    assert.deepEqual([t.range.startLine, t.range.endLine], [1, 1]);
    assert.equal(t.label, "x.c · line 2");
    assert.equal(t.canReply, false);
    assert.match(t.comments[0].author.name, /^L2 @ .+ t:$/);
    assert.match(t.comments[0].body.value, /^capital b\n\n---\n\n\*session S1: T\\-1: work\*/);

    // Opening the same edit again reuses its thread.
    onMessage!({ type: "showEdit", commit: "L2", sessionMsg: "T-1: work" });
    await new Promise((r) => setTimeout(r, 300));
    assert.equal(threads.length, 1);
});
