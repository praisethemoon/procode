/* Artifacts in VS Code (specs/artifacts.md §5): the list of what agents have
 * published into the workspace — a webview with the Board's filter bar —
 * and a tab per artifact that renders its page in the editor's theme.
 *
 * The store is the `artifacts` package, read fresh on every refresh: the
 * files are small, and an agent writing through MCP is a different process,
 * so there is no cache here that could disagree with the disk.
 */

import * as crypto from "node:crypto";
import * as fs from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";

import { Artifacts, findArtifacts } from "artifacts";

import { FrameParts, frameDocument } from "./frame";
import { sortForList } from "./list";
import type { ToHost, ToView } from "./protocol";
import { viewerHtml } from "./viewer";

const panels = new Map<string, vscode.WebviewPanel>();

function store(): Artifacts | null {
    const folder = vscode.workspace.workspaceFolders?.find((f) => f.uri.scheme === "file");
    if (!folder) return null;
    const root = findArtifacts(folder.uri.fsPath);
    return new Artifacts(root ?? folder.uri.fsPath);
}

/* The list: a webview view (a native tree has no room for a filter bar).
 * The host reads the pages and sends them; the view filters and draws. */
class Pages implements vscode.WebviewViewProvider {
    private view: vscode.WebviewView | null = null;

    constructor(private readonly ctx: vscode.ExtensionContext) {}

    resolveWebviewView(view: vscode.WebviewView): void {
        this.view = view;
        const media = vscode.Uri.joinPath(this.ctx.extensionUri, "out", "media");
        view.webview.options = { enableScripts: true, localResourceRoots: [media] };
        view.webview.html = listHtml(view.webview, media);
        view.webview.onDidReceiveMessage((m: ToHost) => {
            if (m?.type === "ready") this.refresh();
            else if (m?.type === "open") open(this.ctx, m.id);
            else if (m?.type === "source") void vscode.commands.executeCommand("artifacts.openSource", m.id);
        });
        view.onDidDispose(() => {
            this.view = null;
        });
    }

    refresh(): void {
        const s = store();
        const pages = sortForList(s?.list() ?? []).map(({ bytes: _bytes, ...row }) => row);
        const m: ToView = { type: "pages", pages, hasFolder: s !== null };
        void this.view?.webview.postMessage(m);
    }
}

function listHtml(webview: vscode.Webview, media: vscode.Uri): string {
    const nonce = crypto.randomBytes(16).toString("base64");
    const uri = (f: string) => webview.asWebviewUri(vscode.Uri.joinPath(media, f)).toString();
    const csp = [
        "default-src 'none'",
        `style-src ${webview.cspSource}`,
        `script-src 'nonce-${nonce}'`,
        `font-src ${webview.cspSource}`,
    ].join("; ");
    const css = ["baukasten-vscode.css", "codicon.css", "pages.css"].map((f) => `<link rel="stylesheet" href="${uri(f)}">`).join("\n");
    return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="${csp}">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
${css}
</head>
<body>
<div id="root"></div>
<script nonce="${nonce}" src="${uri("pages.js")}"></script>
</body>
</html>`;
}

function parts(ctx: vscode.ExtensionContext): FrameParts {
    const media = path.join(ctx.extensionPath, "out", "media");
    return {
        tokens: fs.readFileSync(path.join(media, "baukasten-vscode.css"), "utf8"),
        defaults: fs.readFileSync(path.join(media, "artifact.css"), "utf8"),
    };
}

function load(ctx: vscode.ExtensionContext, id: string, panel: vscode.WebviewPanel): void {
    const s = store();
    try {
        const { artifact, html } = s!.get(id);
        panel.title = artifact.title;
        void panel.webview.postMessage({ type: "load", doc: frameDocument(html, parts(ctx)) });
    } catch {
        // Gone since it was opened: the tab has nothing left to show.
        panel.dispose();
    }
}

function open(ctx: vscode.ExtensionContext, id: string): void {
    const existing = panels.get(id);
    if (existing) {
        existing.reveal();
        return;
    }
    const s = store();
    let title = id;
    try {
        title = s!.get(id).artifact.title;
    } catch {
        void vscode.window.showWarningMessage(`Artifacts: there is no ${id} any more.`);
        return;
    }
    const panel = vscode.window.createWebviewPanel("artifacts.view", title, vscode.ViewColumn.Active, {
        enableScripts: true,
        localResourceRoots: [],
        retainContextWhenHidden: true,
    });
    panel.iconPath = new vscode.ThemeIcon("preview");
    panel.webview.html = viewerHtml(title);
    panel.webview.onDidReceiveMessage((m: { type?: string }) => {
        if (m?.type === "ready") load(ctx, id, panel);
    });
    panel.onDidDispose(() => panels.delete(id));
    panels.set(id, panel);
}

/* A command's page: its id, or a row's context (right-click in the list). */
function idOf(arg: unknown): string | undefined {
    if (typeof arg === "string") return arg;
    const id = (arg as { id?: unknown } | null)?.id;
    return typeof id === "string" ? id : undefined;
}

async function pick(): Promise<string | undefined> {
    const all = sortForList(store()?.list() ?? []);
    const chosen = await vscode.window.showQuickPick(
        all.map((a) => ({ label: a.title, description: a.id, detail: a.description, id: a.id })),
        { placeHolder: "Artifact" },
    );
    return chosen?.id;
}

async function remove(id: string): Promise<void> {
    const s = store();
    if (!s) return;
    let title: string;
    try {
        title = s.get(id).artifact.title;
    } catch {
        return;
    }
    const answer = await vscode.window.showWarningMessage(
        `Delete the artifact “${title}” (${id})?`,
        { modal: true, detail: "Its page and metadata are removed from .artifact/. The id is not reused." },
        "Delete",
    );
    if (answer !== "Delete") return;
    // The directory of a validated id, and nothing else: pagePath refuses
    // anything that is not A-<n>.
    fs.rmSync(path.dirname(s.pagePath(id)), { recursive: true, force: true });
    panels.get(id)?.dispose();
}

export function activate(ctx: vscode.ExtensionContext): void {
    const list = new Pages(ctx);
    ctx.subscriptions.push(vscode.window.registerWebviewViewProvider("artifacts.list", list));

    let pending: NodeJS.Timeout | undefined;
    const changed = () => {
        if (pending) clearTimeout(pending);
        pending = setTimeout(() => {
            list.refresh();
            for (const [id, panel] of panels) load(ctx, id, panel);
        }, 150);
    };
    const watcher = vscode.workspace.createFileSystemWatcher("**/.artifact/**");
    ctx.subscriptions.push(watcher, watcher.onDidCreate(changed), watcher.onDidChange(changed), watcher.onDidDelete(changed));

    ctx.subscriptions.push(
        vscode.commands.registerCommand("artifacts.open", async (arg?: unknown) => {
            const id = idOf(arg) ?? (await pick());
            if (id) open(ctx, id);
        }),
        vscode.commands.registerCommand("artifacts.openSource", async (arg?: unknown) => {
            const id = idOf(arg) ?? (await pick());
            const s = store();
            if (!id || !s) return;
            await vscode.window.showTextDocument(vscode.Uri.file(s.pagePath(id)));
        }),
        vscode.commands.registerCommand("artifacts.delete", async (arg?: unknown) => {
            const id = idOf(arg);
            if (id) await remove(id);
        }),
        vscode.commands.registerCommand("artifacts.refresh", () => changed()),
    );
}

export function deactivate(): void {
    for (const panel of panels.values()) panel.dispose();
}
