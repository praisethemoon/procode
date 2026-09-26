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
    commitDiff,
    findBoard,
    search,
    sessionCommits,
    startSession,
    ticketSessions,
    view,
} from "coboard";

import { commentText, regionLabel, regionLines } from "./lapview";
import type { Choices, SidebarToHost, SidebarToView, ToHost, ToView } from "./protocol";

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

/* --------------------------------------------------------------- sidebar
 *
 * A webview rather than a native tree, because the filter bar above the tree
 * is an input with buttons inside it, which a native tree cannot hold. The
 * tree is drawn by webview/sidebar.tsx from the board's summaries; its
 * right-click menu is still VS Code's own, through `webview/context` menus
 * keyed on each row's `data-vscode-context`. */

class Sidebar implements vscode.WebviewViewProvider {
    private view: vscode.WebviewView | null = null;

    constructor(
        private readonly ctx: vscode.ExtensionContext,
        private readonly onOpen: (id: string) => void,
    ) {}

    resolveWebviewView(view: vscode.WebviewView): void {
        this.view = view;
        const media = vscode.Uri.joinPath(this.ctx.extensionUri, "out", "media");
        view.webview.options = { enableScripts: true, localResourceRoots: [media] };
        view.webview.html = html(view.webview, media, "sidebar.js", "");
        view.webview.onDidReceiveMessage((m: SidebarToHost) => {
            if (m.type === "ready") this.refresh();
            else if (m.type === "open") this.onOpen(m.id);
            else if (m.type === "command") void vscode.commands.executeCommand(m.command, ...(m.id ? [m.id] : []));
        });
        view.onDidDispose(() => {
            this.view = null;
        });
    }

    refresh(): void {
        this.post({ type: "items", items: search(items(), ""), hasFolder: folder() !== null });
    }

    post(m: SidebarToView): void {
        void this.view?.webview.postMessage(m);
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
    // The id alone: the page's header carries the title, and a tab strip of
    // full titles leaves no room for anything else.
    panel.title = v ? id : `${id} (deleted)`;
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

function refreshAll(tree: Sidebar): void {
    tree.refresh();
    const all = items();
    for (const [id, panel] of panels) {
        push(id, panel, all);
    }
}

function html(webview: vscode.Webview, media: vscode.Uri, script: string, id: string): string {
    const nonce = crypto.randomBytes(16).toString("base64");
    const uri = (f: string) => webview.asWebviewUri(vscode.Uri.joinPath(media, f)).toString();
    const csp = [
        "default-src 'none'",
        `style-src ${webview.cspSource} 'unsafe-inline'`,
        `script-src 'nonce-${nonce}'`,
        `font-src ${webview.cspSource} data:`,
        `img-src ${webview.cspSource} https: data:`,
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
<script nonce="${nonce}" src="${uri(script)}"></script>
</body>
</html>`;
}

function open(ctx: vscode.ExtensionContext, tree: Sidebar, id: string): void {
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
    panel.webview.html = html(panel.webview, media, "board.js", key);
}

async function onMessage(ctx: vscode.ExtensionContext, tree: Sidebar, id: string, panel: vscode.WebviewPanel, m: ToHost): Promise<void> {
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

/* A command's subject: an id passed directly, or the `data-vscode-context`
 * of the sidebar row it was invoked on. */
function idOf(arg: unknown): string | undefined {
    if (typeof arg === "string") return arg;
    if (typeof arg === "object" && arg !== null && typeof (arg as { id?: unknown }).id === "string") {
        return (arg as { id: string }).id;
    }
    return undefined;
}

export function activate(ctx: vscode.ExtensionContext): void {
    const tree: Sidebar = new Sidebar(ctx, (id) => open(ctx, tree, id));
    ctx.subscriptions.push(
        vscode.window.registerWebviewViewProvider("coboard.tree", tree, { webviewOptions: { retainContextWhenHidden: true } }),
    );

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
    reg("coboard.collapseAll", () => tree.post({ type: "collapseAll" }));
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
