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
import { cliBin, noCli } from "./cli-bin";

const root = fs.mkdtempSync(path.join(os.tmpdir(), "coboard-host-"));
const commands = new Map<string, (...a: unknown[]) => unknown>();
const posted: unknown[] = [];
const prompts: string[] = [];
const warnings: string[] = [];
let warningAnswer: string | undefined;
const executed: unknown[][] = [];
const threads: {
    uri: { path: string };
    range: { startLine: number; endLine: number };
    comments: { author: { name: string }; body: { value: string; isTrusted?: unknown } }[];
    canReply: boolean;
    label: string;
}[] = [];
let content: { provideTextDocumentContent(uri: { toString(): string }): string } | null = null;
const LAP = cliBin("lap");
let sidebar: { resolveWebviewView(view: unknown): void } | null = null;
let onMessage: ((m: unknown) => void) | null = null;
/* Every tab the host opened, in order, each with what it was sent and how
 * to send it a message from its page. */
interface FakePanel {
    title: string;
    active: boolean;
    iconPath?: { id: string };
    revealed: number;
    disposed: boolean;
    sent: { type: string; id?: string }[];
    send(m: unknown): void;
}
const made: FakePanel[] = [];
/* The editor groups, as vscode.diff fills them: a column's tabs. */
class TabInputTextDiff {
    constructor(public original: { toString(): string }, public modified: { toString(): string }) {}
}
const groups = new Map<number, { input: TabInputTextDiff }[]>();

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
    ViewColumn: { Active: -1, Beside: -2 },
    TabInputTextDiff,
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
        createWebviewPanel: (_type: string, title: string) => {
            let handler: (m: unknown) => void = () => {};
            const gone: (() => void)[] = [];
            const p = {
                title,
                viewColumn: 1,
                active: false,
                revealed: 0,
                disposed: false,
                sent: [] as FakePanel["sent"],
                reveal() {
                    p.revealed++;
                },
                onDidDispose(fn: () => void) {
                    gone.push(fn);
                },
                dispose() {
                    p.disposed = true;
                    for (const fn of gone) fn();
                },
                send: (m: unknown) => handler(m),
                webview: {
                    cspSource: "vscode-resource:",
                    html: "",
                    asWebviewUri: (u: { fsPath: string }) => u.fsPath,
                    postMessage: (m: FakePanel["sent"][number]) => {
                        posted.push(m);
                        p.sent.push(m);
                    },
                    onDidReceiveMessage: (fn: (m: unknown) => void) => {
                        handler = fn;
                        onMessage = fn;
                    },
                },
            };
            made.push(p);
            return p;
        },
        tabGroups: {
            get all() {
                return [...groups].map(([viewColumn, tabs]) => ({ viewColumn, tabs }));
            },
        },
        showErrorMessage: (m: string) => assert.fail(`the host reported an error: ${m}`),
        showWarningMessage: async (m: string) => {
            warnings.push(m);
            return warningAnswer;
        },
        showInputBox: async (o: { title?: string }) => {
            prompts.push(o.title ?? "");
            return "shipped";
        },
    },
    commands: {
        registerCommand: (name: string, fn: (...a: unknown[]) => unknown) => {
            commands.set(name, fn);
            return { dispose() {} };
        },
        executeCommand: (name: string, ...a: unknown[]) => {
            executed.push([name, ...a]);
            if (name === "vscode.diff") {
                const [left, right, , opts] = a as [{ toString(): string }, { toString(): string }, string, { viewColumn: number }];
                const col = opts.viewColumn > 0 ? opts.viewColumn : 99;
                const tabs = groups.get(col) ?? [];
                if (!tabs.some((t) => t.input.modified.toString() === right.toString())) tabs.push({ input: new TabInputTextDiff(left, right) });
                groups.set(col, tabs);
            }
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

    for (const c of [
        "coboard.newEpic",
        "coboard.newMilestone",
        "coboard.newTicket",
        "coboard.open",
        "coboard.back",
        "coboard.forward",
        "coboard.delete",
        "coboard.archive",
        "coboard.archiveWithNote",
        "coboard.unarchive",
        "coboard.close",
        "coboard.reopen",
        "coboard.startSession",
        "coboard.refresh",
        "coboard.collapseAll",
    ]) {
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

    // Archive from a row's right-click archives at once: nothing asked, no note.
    const promptsBefore = prompts.length;
    await commands.get("coboard.archive")!({ webviewSection: "ticket", id: "T-2", coboardArchived: false });
    assert.equal(prompts.length, promptsBefore, "Archive asked for a note");
    const direct = b.get("T-2").archived;
    assert.ok(direct && direct.via === null && direct.reason === undefined, "archived itself, with no note");
    await commands.get("coboard.unarchive")!({ webviewSection: "ticket", id: "T-2" });
    // Archive with Note says what goes with it, asks why, and the sidebar is
    // sent the archived items, marked, to filter itself.
    await commands.get("coboard.archiveWithNote")!({ webviewSection: "milestone", id: "M-1", coboardArchived: false });
    assert.equal(prompts.at(-1), "Archive M-1 and the 2 items under it");
    const archived = b.get("T-1").archived;
    assert.deepEqual(archived && [archived.via, archived.reason], ["M-1", "shipped"]);
    const after = toSidebar.filter((m) => m.type === "items").at(-1) as { items: { id: string; archived?: true }[] };
    assert.deepEqual(after.items.filter((i) => i.archived).map((i) => i.id), ["M-1", "T-1", "T-3"]);
    // And back, from the archived milestone's page.
    onMessage!({ type: "unarchive", id: "M-1" });
    await new Promise((r) => setTimeout(r, 10));
    assert.equal(b.get("T-1").archived, undefined);

    // Close from a row's right-click: with tickets not done it asks, and a
    // dismissed question leaves the milestone open.
    const row = { webviewSection: "milestone", id: "M-1", coboardDone: false };
    warningAnswer = undefined;
    await commands.get("coboard.close")!(row);
    assert.equal(warnings.at(-1), "M-1 has 2 tickets not done. Close it anyway?");
    assert.equal(b.get("M-1").status, "open");
    warningAnswer = "Close";
    await commands.get("coboard.close")!(row);
    assert.equal(b.get("M-1").status, "done");
    // The sidebar is sent the new status, which the menu reads as coboardDone.
    const closed = toSidebar.filter((m) => m.type === "items").at(-1) as unknown as { items: { id: string; status: string }[] };
    assert.equal(closed.items.find((i) => i.id === "M-1")!.status, "done");
    await commands.get("coboard.reopen")!({ ...row, coboardDone: true });
    assert.equal(b.get("M-1").status, "open");
    // With every ticket done there is nothing to ask.
    for (const id of ["T-1", "T-2", "T-3"]) b.update(id, { status: "done" });
    const asked = warnings.length;
    await commands.get("coboard.close")!({ webviewSection: "epic", id: "E-1", coboardDone: false });
    assert.equal(warnings.length, asked);
    assert.equal(b.get("E-1").status, "done");
});

test("a link followed in a tab shows the item there; ⌘-click, the middle button and an open item get tabs of their own", async () => {
    const b = new Board(root);
    const tab = (title: string) => made.find((p) => !p.disposed && p.title === title)!;
    const count = () => made.filter((p) => !p.disposed).length;
    const lastData = (p: FakePanel) => p.sent.filter((m) => m.type === "data").at(-1)!;
    const back = executed.length;

    // A plain click: the milestone's tab becomes the ticket's, with its icon.
    const t = tab("M-1");
    const n = count();
    t.send({ type: "open", id: "t-1", newTab: false });
    assert.equal(count(), n, "no new tab");
    assert.equal(t.title, "T-1");
    assert.equal(t.iconPath!.id, "issues");
    assert.equal(lastData(t).id, "T-1");

    // An item that already has a tab is shown there, whichever way it is clicked.
    const t3 = tab("T-3");
    const seen = t3.revealed;
    t.send({ type: "open", id: "T-3", newTab: false });
    t.send({ type: "open", id: "T-3", newTab: true });
    assert.deepEqual([t.title, t3.revealed - seen, count()], ["T-1", 2, n]);

    // ⌘-click or the middle button: a tab of its own.
    t.send({ type: "open", id: "T-2", newTab: true });
    assert.equal(count(), n + 1);
    assert.equal(t.title, "T-1");
    assert.ok(tab("T-2"));

    // A refresh reaches the tab under its new item.
    b.update("T-1", { title: "Renamed" });
    await commands.get("coboard.refresh")!();
    assert.equal((lastData(t) as unknown as { view: { ticket: { title: string } } }).view.ticket.title, "Renamed");

    // Back from the mouse, Forward from the key, and Back past the start runs
    // VS Code's own Go Back.
    t.send({ type: "history", go: "back" });
    assert.equal(t.title, "M-1");
    t.active = true;
    await commands.get("coboard.forward")!();
    assert.equal(t.title, "T-1");
    await commands.get("coboard.back")!();
    await commands.get("coboard.back")!();
    assert.equal(t.title, "M-1");
    assert.deepEqual(executed.slice(back).map((c) => c[0]), ["workbench.action.navigateBack"]);

    // Forward to an item opened meanwhile in another tab brings that tab
    // forward and uses the entry up; the next press is VS Code's.
    t.send({ type: "open", id: "T-1", newTab: true });
    const t1 = tab("T-1");
    const before = t1.revealed;
    await commands.get("coboard.forward")!();
    assert.deepEqual([t.title, t1.revealed - before], ["M-1", 1]);
    await commands.get("coboard.forward")!();
    assert.equal(executed.at(-1)![0], "workbench.action.navigateForward");
    t.active = false;

    // Deleting the item a tab was moved to closes that tab, and the others
    // are still found by their items.
    b.create({ kind: "ticket", title: "Doomed", milestone: "M-1" });
    t.send({ type: "open", id: "T-4", newTab: false });
    assert.equal(t.title, "T-4");
    warningAnswer = "Delete";
    await commands.get("coboard.delete")!("T-4");
    assert.equal(t.disposed, true);
    const again = t1.revealed;
    await commands.get("coboard.open")!("T-1");
    assert.equal(t1.revealed - again, 1);
});

test("clicking a lap edit on a ticket opens it as a diff at the edited line", { skip: !LAP && noCli("lap") }, async () => {
    const lap = (...args: string[]) => execFileSync(LAP, args, { cwd: root, env: { ...process.env, LAP_USER: "t" } });
    lap("init");
    lap("session", "start", "T-1: work", "--meta", "ticket=T-1");
    fs.writeFileSync(path.join(root, "x.c"), "a\nb\nc\n");
    lap("commit", "x.c", "-i", "Seed the sample source file", "-b", "Three one-letter rows now exist");
    fs.writeFileSync(path.join(root, "x.c"), "a\nB\nc\n");
    lap("commit", "x.c", "-i", "Shout the middle row (follows L1)", "-b", "The second row reads in upper case now");
    lap("session", "end");
    const hash = (JSON.parse(String(lap("show", "L2", "--json"))) as { hash: string }).hash;

    onMessage!({ type: "showEdit", commit: "L2", sessionMsg: "T-1: work" });
    await new Promise((r) => setTimeout(r, 300));
    const diff = executed.find((c) => c[0] === "vscode.diff");
    assert.ok(diff, "vscode.diff was opened");
    type Opts = { selection: { startLine: number }; viewColumn: number; preview: boolean; preserveFocus: boolean };
    const [, left, right, title, opts] = diff as [string, { path: string }, { path: string }, string, Opts];
    // Beside the panel (column 1), as its own tab, the panel keeping focus.
    assert.deepEqual([opts.viewColumn, opts.preview, opts.preserveFocus], [2, false, true]);
    assert.equal(left.path, "/L2/before/x.c");
    assert.equal(right.path, "/L2/after/x.c");
    assert.equal(title, "L2 · x.c");
    assert.equal(opts.selection.startLine, 1, "scrolled to line 2 (0-based 1)");
    assert.equal(content!.provideTextDocumentContent(left as never), "a\nb\nc\n");
    assert.equal(content!.provideTextDocumentContent(right as never), "a\nB\nc\n");

    // The intent and behavior sit on the changed line, as in Lap History,
    // under the id and short hash; the commit the intent names is a link.
    assert.equal(threads.length, 1);
    const t = threads[0];
    assert.equal(t.uri.path, "/L2/after/x.c");
    assert.deepEqual([t.range.startLine, t.range.endLine], [1, 1]);
    assert.equal(t.label, "x.c · line 2");
    assert.equal(t.canReply, false);
    assert.equal(t.comments[0].author.name.startsWith(`L2 · ${hash.slice(0, 7)} @ `), true);
    assert.match(t.comments[0].author.name, / t:$/);
    assert.match(
        t.comments[0].body.value,
        /^\*\*Intent\*\*: Shout the middle row \\\(follows \[L1\]\(command:coboard\.showEdit\?%5B%22L1%22%5D\)\\\)\n\n\*\*Behavior\*\*: The second row reads in upper case now\n\n---\n\n\*session S1: T\\-1: work\*/,
    );
    assert.deepEqual(t.comments[0].body.isTrusted, { enabledCommands: ["coboard.showEdit"] });

    // Opening the same edit again, by a prefix of its hash, reuses its thread.
    onMessage!({ type: "showEdit", commit: "#" + hash.slice(0, 9) });
    await new Promise((r) => setTimeout(r, 300));
    assert.equal(threads.length, 1);
    assert.equal(executed.filter((c) => c[0] === "vscode.diff").length, 2);
    const diffs = () => executed.filter((c) => c[0] === "vscode.diff") as [string, unknown, { path: string }, string, Opts][];
    assert.equal(diffs().at(-1)![4].viewColumn, 2, "the same group again");
    assert.deepEqual([...groups.keys()], [2]);
    assert.equal(groups.get(2)!.length, 1, "the same edit is the tab already there, not a second");

    // Another edit: a second tab in that group, never a third column.
    onMessage!({ type: "showEdit", commit: "L1" });
    await new Promise((r) => setTimeout(r, 300));
    assert.equal(diffs().at(-1)![4].viewColumn, 2);
    assert.deepEqual(groups.get(2)!.map((t) => t.input.modified.toString()), ["coboard-lap:/L2/after/x.c", "coboard-lap:/L1/after/x.c"]);

    // A link inside a diff (no panel) opens in the diff group while it is
    // open, taking focus; once that group is closed, beside the active editor.
    await commands.get("coboard.showEdit")!("L2");
    assert.deepEqual([diffs().at(-1)![4].viewColumn, diffs().at(-1)![4].preserveFocus], [2, false]);
    groups.clear();
    await commands.get("coboard.showEdit")!("L2");
    assert.equal(diffs().at(-1)![4].viewColumn, -2);
});
