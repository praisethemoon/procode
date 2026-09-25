/* The Board: an activity-bar tree of epics, milestones and tickets, and one
 * editor tab per item.
 *
 * The board is the `.coboard/` at or above the first workspace folder (or, if
 * there is none yet, one created in that folder on the first write). Agents
 * write to the same log through the MCP server, so the log is watched and
 * every tree node and open tab re-renders when it changes.
 */

import * as crypto from "node:crypto";
import * as os from "node:os";
import * as vscode from "vscode";

import {
    Board,
    BoardError,
    Item,
    Summary,
    commitDiff,
    findBoard,
    search,
    sessionCommits,
    summarize,
    startSession,
    ticketSessions,
    view,
} from "coboard";

import { commentText, regionLabel, regionLines } from "./lapview";
import type { Choices, ToHost, ToView } from "./protocol";

let board: Board | null = null;

function folder(): string | null {
    const f = vscode.workspace.workspaceFolders?.find((w) => w.uri.scheme === "file");
    return f ? f.uri.fsPath : null;
}

function currentBoard(): Board | null {
    const root = folder();
    if (!root) {
        return null;
    }
    const found = findBoard(root) ?? root;
    if (!board || board.root !== found) {
        board = new Board(found);
    }
    return board;
}

function requireBoard(): Board {
    const b = currentBoard();
    if (!b) {
        throw new BoardError("no_board", "Open a folder to use its board.");
    }
    return b;
}

function items(): Item[] {
    return currentBoard()?.all() ?? [];
}

function author(): string {
    const configured = vscode.workspace.getConfiguration("coboard").get<string>("author", "").trim();
    return configured || os.userInfo().username;
}

function syncLapPath(): void {
    process.env["LAP_BIN"] = vscode.workspace.getConfiguration("coboard").get<string>("lapPath", "lap") || "lap";
}

async function guarded<T>(fn: () => T | Promise<T>): Promise<T | undefined> {
    try {
        return await fn();
    } catch (e) {
        void vscode.window.showErrorMessage((e as Error).message);
        return undefined;
    }
}

/* ------------------------------------------------------------------ tree */

const STATUS_ICON: Record<string, string> = {
    todo: "circle-large-outline",
    doing: "play-circle",
    blocked: "error",
    review: "eye",
    done: "pass-filled",
    open: "circle-large-outline",
};

class Node extends vscode.TreeItem {
    constructor(readonly item: Summary, hasChildren: boolean) {
        super(item.title, hasChildren ? vscode.TreeItemCollapsibleState.Expanded : vscode.TreeItemCollapsibleState.None);
        this.id = item.id;
        this.description = item.kind === "ticket" ? `${item.id} · ${item.status}` : item.id;
        this.contextValue = item.kind;
        this.tooltip = `${item.id} — ${item.title}\n${item.kind}, ${item.status}`;
        const icon = item.kind === "epic" ? "project" : item.kind === "milestone" ? "milestone" : STATUS_ICON[item.status] ?? "circle-large-outline";
        this.iconPath = new vscode.ThemeIcon(item.status === "done" && item.kind !== "ticket" ? "pass" : icon);
        this.command = { command: "coboard.open", title: "Open", arguments: [item.id] };
    }
}

class Tree implements vscode.TreeDataProvider<Node> {
    private readonly changed = new vscode.EventEmitter<void>();
    readonly onDidChangeTreeData = this.changed.event;

    refresh(): void {
        this.changed.fire();
    }

    getTreeItem(n: Node): vscode.TreeItem {
        return n;
    }

    getChildren(parent?: Node): Node[] {
        const all = items(); // epics, then milestones, then tickets
        const childrenOf = (p: Item): Item[] =>
            p.kind === "epic"
                ? all.filter((i) => (i.kind === "milestone" && i.epic === p.id) || (i.kind === "ticket" && i.epic === p.id && i.milestone === null))
                : p.kind === "milestone"
                  ? all.filter((i) => i.kind === "ticket" && i.milestone === p.id)
                  : [];
        const p = parent ? all.find((i) => i.id === parent.item.id) : undefined;
        const list = parent ? (p ? childrenOf(p) : []) : all.filter((i) => i.kind === "epic");
        return list.map((i) => new Node(summarize(i), childrenOf(i).length > 0));
    }
}

/* ------------------------------------------------------------------ tabs */

const panels = new Map<string, vscode.WebviewPanel>();

function choices(all: Item[]): Choices {
    const summaries = search(all, "");
    return {
        epics: summaries.filter((s) => s.kind === "epic"),
        milestones: summaries.filter((s) => s.kind === "milestone"),
    };
}

function push(id: string, panel: vscode.WebviewPanel, all: Item[] = items()): void {
    const v = view(all, id);
    panel.title = v ? `${id} ${all.find((i) => i.id === id)?.title ?? ""}` : `${id} (deleted)`;
    const msg: ToView = { type: "data", id, view: v, choices: choices(all) };
    void panel.webview.postMessage(msg);
}

async function pushSessions(ticket: string, panel: vscode.WebviewPanel): Promise<void> {
    const b = currentBoard();
    if (!b) return;
    syncLapPath();
    const r = await ticketSessions(b.root, ticket);
    const msg: ToView = { type: "sessions", ticket, sessions: { ok: r.ok, sessions: r.value, ...(r.error ? { error: r.error } : {}) } };
    void panel.webview.postMessage(msg);
}

function refreshAll(tree: Tree): void {
    tree.refresh();
    const all = items();
    for (const [id, panel] of panels) {
        push(id, panel, all);
    }
}

function html(panel: vscode.WebviewPanel, media: vscode.Uri, id: string): string {
    const nonce = crypto.randomBytes(16).toString("base64");
    const uri = (f: string) => panel.webview.asWebviewUri(vscode.Uri.joinPath(media, f)).toString();
    const csp = [
        "default-src 'none'",
        `style-src ${panel.webview.cspSource} 'unsafe-inline'`,
        `script-src 'nonce-${nonce}'`,
        `font-src ${panel.webview.cspSource} data:`,
        `img-src ${panel.webview.cspSource} https: data:`,
    ].join("; ");
    const css = ["baukasten-base.css", "baukasten-vscode.css", "codicon.css", "board.css"]
        .map((f) => `<link rel="stylesheet" href="${uri(f)}">`)
        .join("\n");
    return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="${csp}">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
${css}
</head>
<body>
<div id="root" data-id="${id}"></div>
<script nonce="${nonce}" src="${uri("board.js")}"></script>
</body>
</html>`;
}

function open(ctx: vscode.ExtensionContext, tree: Tree, id: string): void {
    const key = id.trim().toUpperCase();
    const existing = panels.get(key);
    if (existing) {
        existing.reveal();
        return;
    }
    const media = vscode.Uri.joinPath(ctx.extensionUri, "out", "media");
    const panel = vscode.window.createWebviewPanel("coboard.item", key, vscode.ViewColumn.Active, {
        enableScripts: true,
        retainContextWhenHidden: true,
        localResourceRoots: [media],
    });
    panel.iconPath = new vscode.ThemeIcon(key.startsWith("E") ? "project" : key.startsWith("M") ? "milestone" : "issues");
    panels.set(key, panel);
    panel.onDidDispose(() => panels.delete(key));
    panel.webview.onDidReceiveMessage((m: ToHost) => void onMessage(ctx, tree, key, panel, m));
    panel.webview.html = html(panel, media, key);
}

async function onMessage(ctx: vscode.ExtensionContext, tree: Tree, id: string, panel: vscode.WebviewPanel, m: ToHost): Promise<void> {
    const fail = (e: unknown) => {
        const msg: ToView = { type: "error", message: (e as Error).message };
        void panel.webview.postMessage(msg);
    };
    try {
        switch (m.type) {
            case "ready":
                push(id, panel);
                if (id.startsWith("T-")) await pushSessions(id, panel);
                return;
            case "open":
                open(ctx, tree, m.id);
                return;
            case "update":
                requireBoard().update(m.id, m.fields);
                break;
            case "create": {
                const created = requireBoard().create({ kind: m.kind, title: m.title, epic: m.epic ?? null, milestone: m.milestone ?? null });
                refreshAll(tree);
                open(ctx, tree, created.id);
                return;
            }
            case "move":
                requireBoard().move(m.id, { ...(m.epic !== undefined ? { epic: m.epic } : {}), ...(m.milestone !== undefined ? { milestone: m.milestone } : {}) });
                break;
            case "comment":
                requireBoard().comment(m.ticket, m.body, author());
                break;
            case "delete":
                await vscode.commands.executeCommand("coboard.delete", m.id);
                return;
            case "startSession":
                await vscode.commands.executeCommand("coboard.startSession", m.ticket);
                await pushSessions(m.ticket, panel);
                return;
            case "showEdit":
                await showEdit(m.commit, m.sessionMsg ?? null);
                return;
            case "commits": {
                const b = requireBoard();
                syncLapPath();
                const r = await sessionCommits(b.root, m.session);
                const msg: ToView = { type: "commits", session: m.session, commits: r.value, ...(r.error ? { error: r.error } : {}) };
                void panel.webview.postMessage(msg);
                return;
            }
        }
        refreshAll(tree);
    } catch (e) {
        fail(e);
        push(id, panel);
    }
}

/* ------------------------------------------------------------ lap edits */

/* One lap commit shown exactly as Lap History shows it: VS Code's own diff of
 * the file just before and just after the edit, scrolled to it, with the
 * commit's reason attached as an inline comment thread on the changed lines.
 * The two sides are read-only documents served from memory under
 * `coboard-lap:`; the path keeps the file's name so the diff gets its syntax
 * highlighting. */
const EDIT_SCHEME = "coboard-lap";
const editText = new Map<string, string>();
const editThreads = new Map<string, vscode.CommentThread>();
let editComments: vscode.CommentController | null = null;

async function showEdit(commit: string, sessionMsg: string | null): Promise<void> {
    const b = requireBoard();
    syncLapPath();
    const d = await commitDiff(b.root, commit);
    const side = (which: string) => vscode.Uri.from({ scheme: EDIT_SCHEME, path: `/${d.id}/${which}/${d.file}` });
    const left = side("before");
    const right = side("after");
    editText.set(left.toString(), d.before);
    editText.set(right.toString(), d.after);
    const lines = regionLines(d);
    const range = new vscode.Range(lines.start, 0, lines.end, 0);
    await vscode.commands.executeCommand("vscode.diff", left, right, `${d.id} · ${d.file}`, { preview: true, selection: range });

    const existing = editThreads.get(d.id);
    if (existing) {
        existing.collapsibleState = vscode.CommentThreadCollapsibleState.Expanded;
        return;
    }
    if (!editComments) return;
    const text = commentText(d, sessionMsg);
    const thread = editComments.createCommentThread(right, range, [
        { author: { name: text.author }, body: new vscode.MarkdownString(text.body), mode: vscode.CommentMode.Preview },
    ]);
    thread.canReply = false;
    thread.collapsibleState = vscode.CommentThreadCollapsibleState.Expanded;
    thread.label = `${d.file} · ${regionLabel(d)}`;
    editThreads.set(d.id, thread);
}

/* -------------------------------------------------------------- commands */

async function ask(prompt: string): Promise<string | undefined> {
    const title = await vscode.window.showInputBox({ prompt, validateInput: (v) => (v.trim() ? null : "A title is required") });
    return title?.trim() || undefined;
}

function idOf(arg: unknown): string | undefined {
    if (typeof arg === "string") return arg;
    if (arg instanceof Node) return arg.item.id;
    return undefined;
}

export function activate(ctx: vscode.ExtensionContext): void {
    const tree = new Tree();
    const treeView = vscode.window.createTreeView("coboard.tree", { treeDataProvider: tree, showCollapseAll: true });
    ctx.subscriptions.push(treeView);

    const reg = (name: string, fn: (...args: unknown[]) => unknown) =>
        ctx.subscriptions.push(vscode.commands.registerCommand(name, (...args: unknown[]) => guarded(() => fn(...args))));

    editComments = vscode.comments.createCommentController("coboard-lap", "lap edit");
    ctx.subscriptions.push(
        editComments,
        vscode.workspace.registerTextDocumentContentProvider(EDIT_SCHEME, {
            provideTextDocumentContent: (uri) => editText.get(uri.toString()) ?? "",
        }),
    );
    reg("coboard.refresh", () => refreshAll(tree));
    reg("coboard.showEdit", (arg) => (typeof arg === "string" ? showEdit(arg, null) : undefined));
    reg("coboard.open", (arg) => {
        const id = idOf(arg);
        if (id) open(ctx, tree, id);
    });
    reg("coboard.newEpic", async () => {
        const title = await ask("New epic: title");
        if (!title) return;
        const e = requireBoard().create({ kind: "epic", title });
        refreshAll(tree);
        open(ctx, tree, e.id);
    });
    reg("coboard.newMilestone", async (arg) => {
        const epic = idOf(arg);
        if (!epic) return;
        const title = await ask(`New milestone in ${epic}: title`);
        if (!title) return;
        const m = requireBoard().create({ kind: "milestone", title, epic });
        refreshAll(tree);
        open(ctx, tree, m.id);
    });
    reg("coboard.newTicket", async (arg) => {
        const parent = idOf(arg);
        if (!parent) return;
        const title = await ask(`New ticket in ${parent}: title`);
        if (!title) return;
        const t = requireBoard().create(
            parent.startsWith("M-") ? { kind: "ticket", title, milestone: parent } : { kind: "ticket", title, epic: parent },
        );
        refreshAll(tree);
        open(ctx, tree, t.id);
    });
    reg("coboard.delete", async (arg) => {
        const id = idOf(arg);
        if (!id) return;
        const b = requireBoard();
        const item = b.get(id);
        const ok = await vscode.window.showWarningMessage(`Delete ${item.id} "${item.title}"?`, { modal: true }, "Delete");
        if (ok !== "Delete") return;
        b.remove(item.id);
        panels.get(item.id)?.dispose();
        refreshAll(tree);
    });
    reg("coboard.startSession", async (arg) => {
        const id = idOf(arg);
        if (!id) return;
        const b = requireBoard();
        const t = b.get(id);
        const purpose = await vscode.window.showInputBox({
            prompt: `lap session for ${t.id}: purpose`,
            value: `${t.id}: ${t.title}`,
        });
        if (!purpose?.trim()) return;
        syncLapPath();
        const s = await startSession(b.root, t.id, purpose.trim());
        void vscode.window.showInformationMessage(`lap session ${s} started for ${t.id}.`);
    });
    reg("coboard.goTo", async () => {
        const pick = await vscode.window.showQuickPick(
            search(items(), "").map((s) => ({
                label: `${s.id}  ${s.title}`,
                description: s.kind === "ticket" ? `${s.status}${s.assignee ? ` · ${s.assignee}` : ""}` : s.kind,
                detail: s.kind === "epic" ? undefined : [s.epic, s.milestone].filter(Boolean).join(" › "),
                id: s.id,
            })),
            { placeHolder: "Go to an epic, milestone or ticket", matchOnDescription: true, matchOnDetail: true },
        );
        if (pick) open(ctx, tree, pick.id);
    });

    // Agents write through the MCP server; the log is the only signal.
    let timer: NodeJS.Timeout | undefined;
    const later = () => {
        clearTimeout(timer);
        timer = setTimeout(() => refreshAll(tree), 150);
    };
    const watcher = vscode.workspace.createFileSystemWatcher("**/.coboard/log.jsonl");
    watcher.onDidChange(later);
    watcher.onDidCreate(later);
    watcher.onDidDelete(later);
    ctx.subscriptions.push(watcher, vscode.workspace.onDidChangeWorkspaceFolders(() => refreshAll(tree)));
}

export function deactivate(): void {
    // nothing to release: every read and write opens and closes the log
}
