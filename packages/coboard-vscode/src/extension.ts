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
    locateBoard,
    staleParentMessage,
    search,
    sessionCommits,
    sessionReview,
    startSession,
    ticketSessions,
    view,
} from "coboard";

import { SHOW_EDIT, commentText, regionLabel, regionLines } from "./lapview";
import type { ViewMode } from "./kanban";
import { BESIDE, diffColumn } from "./placement";
import { sessionKey } from "./protocol";
import type { Choices, SidebarToHost, SidebarToView, ToHost, ToView } from "./protocol";

let board: Board | null = null;
/* List or Kanban for epic and milestone tabs, remembered per workspace. */
let viewMode: ViewMode = "list";

function folder(): string | null {
    const f = vscode.workspace.workspaceFolders?.find((w) => w.uri.scheme === "file");
    return f ? f.uri.fsPath : null;
}

/* How this window found its board, for the view to say when it is not the
 * folder's own: set in Board Folder, or a lap branch folder's parent's. */
let boardVia: "override" | "lap-parent" | "found" | "stale-parent" = "found";
/* A lap branch folder whose parent is gone: why there is no board. */
let staleBoard: string | null = null;

function currentBoard(): Board | null {
    const root = folder();
    if (!root) {
        return null;
    }
    const setting = vscode.workspace.getConfiguration("coboard").get<string>("boardFolder", "");
    const at = locateBoard(root, setting || process.env["COBOARD_DIR"]);
    boardVia = at.via;
    /* never the branch's own copy of the board, which is stale */
    staleBoard = at.via === "stale-parent" ? staleParentMessage(at.stale!) : null;
    if (staleBoard) {
        return null;
    }
    const found = at.root ?? at.home ?? root;
    if (!board || board.root !== found) {
        board = new Board(found);
    }
    return board;
}

/* The tree's description: whose board it is, when it is not this folder's. */
function boardNote(): string {
    const b = currentBoard();
    if (staleBoard) {
        return "no board: this lap branch's parent folder is gone";
    }
    if (!b || boardVia === "found") {
        return "";
    }
    const name = b.root.split(/[\\/]/).filter(Boolean).pop() ?? b.root;
    return boardVia === "lap-parent" ? `${name}'s board (this folder is a lap branch)` : `${name}'s board`;
}

function requireBoard(): Board {
    const b = currentBoard();
    if (staleBoard) {
        throw new BoardError("stale_parent", staleBoard);
    }
    if (!b) {
        throw new BoardError("no_board", "Open a folder to use its board.");
    }
    return b;
}

/* Every item, archived ones included: the sidebar filters them itself, and
 * an archived item's tab still opens. */
function items(): Item[] {
    return currentBoard()?.all({ archived: "include" }) ?? [];
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
        this.post({ type: "items", items: search(items(), "", { archived: "include" }), hasFolder: folder() !== null });
        if (this.view) this.view.description = boardNote();
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
    const msg: ToView = { type: "data", id, view: v, choices: choices(all), mode: viewMode };
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
        // A review shows a finished session; the board changing does not change it.
        if (!id.startsWith(REVIEW)) push(id, panel, all);
    }
}

/* ---------------------------------------------------------------- reviews
 *
 * A lap session as a review (`lap rr`): its purpose, every edit in order with
 * its reason — each opens as the same diff-with-comment a ticket's commits
 * open — and every file's net change. One tab per session, keyed REVIEW+id. */
const REVIEW = "review:";
/* What a review tab was opened for, by its key: the ticket, and for a branch
 * session or an adopted one, its branch and what that branch's merge
 * stopped. */
interface ReviewOf {
    readonly session: string;
    readonly ticket: string | null;
    readonly branch?: string;
    readonly adoptedFrom?: string;
    readonly stops?: readonly { readonly file: string; readonly at: string }[];
}
const reviews = new Map<string, ReviewOf>();

async function pushReview(key: string, panel: vscode.WebviewPanel): Promise<void> {
    const of = reviews.get(key) ?? { session: key.slice(REVIEW.length), ticket: null };
    const b = currentBoard();
    if (!b) return;
    syncLapPath();
    const r = await sessionReview(b.root, of.session, of.branch);
    const msg: ToView = {
        type: "review",
        session: of.session,
        ticket: of.ticket,
        review: r.value,
        ...(r.error ? { error: r.error } : {}),
        ...(of.branch ? { branch: of.branch } : {}),
        ...(of.adoptedFrom ? { adoptedFrom: of.adoptedFrom, stops: of.stops ?? [] } : {}),
    };
    void panel.webview.postMessage(msg);
}

function openReview(ctx: vscode.ExtensionContext, tree: Sidebar, of: ReviewOf): void {
    const key = REVIEW + sessionKey({ id: of.session.trim().toUpperCase(), branch: of.branch });
    reviews.set(key, of);
    const existing = panels.get(key);
    if (existing) {
        existing.reveal();
        return;
    }
    const media = vscode.Uri.joinPath(ctx.extensionUri, "out", "media");
    const panel = vscode.window.createWebviewPanel("coboard.review", `${key.slice(REVIEW.length)} review`, vscode.ViewColumn.Active, {
        enableScripts: true,
        retainContextWhenHidden: true,
        localResourceRoots: [media],
    });
    panel.iconPath = new vscode.ThemeIcon("git-pull-request");
    panels.set(key, panel);
    panel.onDidDispose(() => panels.delete(key));
    panel.webview.onDidReceiveMessage((m: ToHost) => void onMessage(ctx, tree, key, panel, m));
    panel.webview.html = html(panel.webview, media, "board.js", key);
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
                /* A new tab is focused before its page loads, which leaves
                 * keyboard shortcuts dead until it is left and come back to;
                 * focusing it again once loaded is what coming back does. */
                if (panel.active) panel.reveal(undefined, false);
                if (id.startsWith(REVIEW)) {
                    await pushReview(id, panel);
                    return;
                }
                push(id, panel);
                if (id.startsWith("T-")) await pushSessions(id, panel);
                return;
            case "open":
                open(ctx, tree, m.id);
                return;
            case "review":
                openReview(ctx, tree, {
                    session: m.session,
                    ticket: m.ticket ?? null,
                    ...(m.branch ? { branch: m.branch } : {}),
                    ...(m.adoptedFrom ? { adoptedFrom: m.adoptedFrom, stops: m.stops ?? [] } : {}),
                });
                return;
            case "mode":
                viewMode = m.mode === "kanban" ? "kanban" : "list";
                await ctx.workspaceState.update("coboard.viewMode", viewMode);
                refreshAll(tree);
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
            case "archive":
                await vscode.commands.executeCommand("coboard.archive", m.id);
                return;
            case "unarchive":
                await vscode.commands.executeCommand("coboard.unarchive", m.id);
                return;
            case "startSession":
                await vscode.commands.executeCommand("coboard.startSession", m.ticket);
                await pushSessions(m.ticket, panel);
                return;
            case "showEdit":
                await showEdit(m.commit, m.sessionMsg ?? null, panel.viewColumn);
                return;
            case "commits": {
                const b = requireBoard();
                syncLapPath();
                const r = await sessionCommits(b.root, m.session, m.branch);
                const msg: ToView = { type: "commits", session: sessionKey({ id: m.session, branch: m.branch }), commits: r.value, ...(r.error ? { error: r.error } : {}) };
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
 * commit's intent and behavior attached as an inline comment thread on the
 * changed lines. `commit` is an id or a hash (a prefix will do); lap resolves
 * it, and the commits the comment names open the same way.
 * The two sides are read-only documents served from memory under
 * `coboard-lap:`; the path keeps the file's name so the diff gets its syntax
 * highlighting. */
const EDIT_SCHEME = "coboard-lap";
const editText = new Map<string, string>();
const editThreads = new Map<string, vscode.CommentThread>();
let editComments: vscode.CommentController | null = null;
/* The editor group the last diff opened in (see placement.ts). */
let diffGroup: number | undefined;

/* `from`: the column of the panel that asked, so the diff opens beside it;
 * undefined for a link inside a diff. */
async function showEdit(commit: string, sessionMsg: string | null, from?: number): Promise<void> {
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
    /* Not a preview: each edit keeps its own tab. The same edit opened again
     * in that group is the tab already there, which VS Code brings forward. */
    const col = diffColumn(from, diffGroup, vscode.window.tabGroups.all.map((g) => g.viewColumn));
    await vscode.commands.executeCommand("vscode.diff", left, right, `${d.id} · ${d.file}`, {
        preview: false,
        preserveFocus: from !== undefined,
        viewColumn: col === BESIDE ? vscode.ViewColumn.Beside : col,
        selection: range,
    });
    const shown = vscode.window.tabGroups.all.find((g) =>
        g.tabs.some((t) => t.input instanceof vscode.TabInputTextDiff && t.input.modified.toString() === right.toString()),
    );
    diffGroup = shown?.viewColumn ?? (col === BESIDE ? undefined : col);

    const existing = editThreads.get(d.id);
    if (existing) {
        existing.collapsibleState = vscode.CommentThreadCollapsibleState.Expanded;
        return;
    }
    if (!editComments) return;
    const text = commentText(d, sessionMsg);
    const body = new vscode.MarkdownString(text.body);
    body.isTrusted = { enabledCommands: [SHOW_EDIT] };
    const thread = editComments.createCommentThread(right, range, [
        { author: { name: text.author }, body, mode: vscode.CommentMode.Preview },
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
    viewMode = ctx.workspaceState.get<ViewMode>("coboard.viewMode") === "kanban" ? "kanban" : "list";
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
    reg(SHOW_EDIT, (arg) => (typeof arg === "string" ? showEdit(arg, null) : undefined));
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
    /* Archive archives at once, with no note; Archive with Note asks for
     * one first, saying what goes with the item, and Escape cancels. */
    const archive = async (arg: unknown, withNote: boolean) => {
        const id = idOf(arg);
        if (!id) return;
        const b = requireBoard();
        const item = b.get(id);
        let reason: string | undefined;
        if (withNote) {
            const under = b
                .all({ archived: "include" })
                .filter((c) => !c.archived && ((c.kind !== "epic" && c.epic === item.id) || (c.kind === "ticket" && c.milestone === item.id)));
            reason = await vscode.window.showInputBox({
                title: `Archive ${item.id}${under.length > 0 ? ` and the ${under.length} item${under.length === 1 ? "" : "s"} under it` : ""}`,
                prompt: "Why, optionally. Archived items leave the board's lists and search; Unarchive brings them back.",
                placeHolder: "e.g. shipped in 0.1",
            });
            if (reason === undefined) return;
        }
        b.archive(item.id, { by: author(), ...(reason ? { reason } : {}) });
        refreshAll(tree);
    };
    reg("coboard.archive", (arg) => archive(arg, false));
    reg("coboard.archiveWithNote", (arg) => archive(arg, true));
    reg("coboard.unarchive", (arg) => {
        const id = idOf(arg);
        if (!id) return;
        requireBoard().unarchive(id);
        refreshAll(tree);
    });
    /* Close is the Status field's "done" from a row's right-click. A closed
     * epic or milestone reads as finished, so tickets under it that are not
     * done are counted and the close is confirmed first. */
    reg("coboard.close", async (arg) => {
        const id = idOf(arg);
        if (!id) return;
        const b = requireBoard();
        const item = b.get(id);
        const open = b
            .all()
            .filter((c) => c.kind === "ticket" && c.status !== "done" && (c.epic === item.id || c.milestone === item.id)).length;
        if (open > 0) {
            const go = await vscode.window.showWarningMessage(
                `${item.id} has ${open} ticket${open === 1 ? "" : "s"} not done. Close it anyway?`,
                { modal: true },
                "Close",
            );
            if (go !== "Close") return;
        }
        b.update(item.id, { status: "done" });
        refreshAll(tree);
    });
    reg("coboard.reopen", (arg) => {
        const id = idOf(arg);
        if (!id) return;
        requireBoard().update(id, { status: "open" });
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
        /* in this folder's lap history, even when the board is another's */
        const s = await startSession(folder() ?? b.root, t.id, purpose.trim());
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
