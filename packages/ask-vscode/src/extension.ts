/* ask in VS Code (specs/ask.md §5): a tab for each form an agent opens.
 *
 * The host watches every workspace folder for a new `.ask/F-<n>/request.json`
 * and opens a tab for it. The tab sends back the answer, which the host
 * writes with the `ask` store. Closing a tab without submitting cancels its
 * form; an answer that appears from elsewhere (the agent's call was
 * cancelled) closes the tab.
 */

import * as crypto from "node:crypto";
import * as fs from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";

import { Ask, Request, findAsk } from "ask";

import type { PreviewParts } from "./preview";
import type { ToHost, ToView } from "./protocol";

/** On startup, forms this recent are opened: their agent may still be
 *  waiting (ask waits 25 minutes a call). Older ones are left for the
 *  Open Pending Questions command. */
const RECENT_MS = 30 * 60_000;

interface Open {
    readonly panel: vscode.WebviewPanel;
    /** Set once the form is answered, so closing the tab cancels nothing. */
    done: boolean;
}

const open = new Map<string, Open>();

function key(root: string, id: string): string {
    return `${root}\0${id}`;
}

/** `<root>/.ask/F-<n>/<file>` → its root and form id. */
function formOf(file: string): { root: string; id: string } {
    const dir = path.dirname(file);
    return { root: path.dirname(path.dirname(dir)), id: path.basename(dir) };
}

function previewParts(media: vscode.Uri): PreviewParts {
    const read = (f: string) => fs.readFileSync(vscode.Uri.joinPath(media, f).fsPath, "utf8");
    return { tokens: read("baukasten-vscode.css"), defaults: read("page.css") };
}

function html(webview: vscode.Webview, media: vscode.Uri, title: string): string {
    const nonce = crypto.randomBytes(16).toString("base64");
    const uri = (f: string) => webview.asWebviewUri(vscode.Uri.joinPath(media, f)).toString();
    const csp = [
        "default-src 'none'",
        `style-src ${webview.cspSource} 'unsafe-inline'`,
        `script-src 'nonce-${nonce}'`,
        `font-src ${webview.cspSource} data:`,
        "img-src data:",
        "frame-src about: data:",
    ].join("; ");
    const esc = (s: string) => s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c] as string);
    const css = ["baukasten-base.css", "baukasten-vscode.css", "codicon.css", "ask.css"].map((f) => `<link rel="stylesheet" href="${uri(f)}">`).join("\n");
    return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="${csp}">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>${esc(title)}</title>
${css}
</head>
<body>
<div id="root"></div>
<script nonce="${nonce}" src="${uri("ask.js")}"></script>
</body>
</html>`;
}

function show(ctx: vscode.ExtensionContext, root: string, req: Request): void {
    const k = key(root, req.id);
    const existing = open.get(k);
    if (existing) {
        existing.panel.reveal();
        return;
    }
    const ask = new Ask(root);
    if (ask.answer(req.id)) return;
    const media = vscode.Uri.joinPath(ctx.extensionUri, "out", "media");
    const panel = vscode.window.createWebviewPanel("ask.form", req.title, vscode.ViewColumn.Active, {
        enableScripts: true,
        retainContextWhenHidden: true,
        localResourceRoots: [media],
    });
    panel.iconPath = new vscode.ThemeIcon("comment-discussion");
    panel.webview.html = html(panel.webview, media, req.title);
    const entry: Open = { panel, done: false };
    open.set(k, entry);
    const post = (m: ToView) => void panel.webview.postMessage(m);
    panel.webview.onDidReceiveMessage((m: ToHost) => {
        if (m?.type === "ready") {
            post({ type: "load", request: req, preview: previewParts(media) });
        } else if (m?.type === "submit") {
            try {
                ask.submit(req.id, m.answer);
            } catch (e) {
                void vscode.window.showErrorMessage(`ask: the answer to ${req.id} was not saved: ${(e as Error).message}`);
                return;
            }
            entry.done = true;
            panel.dispose();
        } else if (m?.type === "dismiss") {
            panel.dispose();
        }
    });
    panel.onDidDispose(() => {
        open.delete(k);
        if (!entry.done) ask.cancel(req.id);
    });
}

/** The form's answer appeared: written by this tab, or by the server after
 *  its call was cancelled. Either way the tab has nothing left to do. */
function answered(file: string): void {
    const { root, id } = formOf(file);
    const entry = open.get(key(root, id));
    if (!entry || entry.done) return;
    entry.done = true;
    const status = new Ask(root).answer(id)?.status ?? "cancelled";
    void entry.panel.webview.postMessage({ type: "closed", status } satisfies ToView);
}

function pendingForms(): { root: string; req: Request }[] {
    const out: { root: string; req: Request }[] = [];
    for (const f of vscode.workspace.workspaceFolders ?? []) {
        if (f.uri.scheme !== "file") continue;
        const root = findAsk(f.uri.fsPath);
        if (root) for (const req of new Ask(root).pending()) out.push({ root, req });
    }
    return out;
}

export function activate(ctx: vscode.ExtensionContext): void {
    const requests = vscode.workspace.createFileSystemWatcher("**/.ask/F-*/request.json", false, true, true);
    requests.onDidCreate((uri) => {
        const { root, id } = formOf(uri.fsPath);
        try {
            show(ctx, root, new Ask(root).request(id));
        } catch {
            // a request that cannot be read is not a form
        }
    });
    const answers = vscode.workspace.createFileSystemWatcher("**/.ask/F-*/answer.json", false, false, true);
    answers.onDidCreate((uri) => answered(uri.fsPath));
    answers.onDidChange((uri) => answered(uri.fsPath));
    ctx.subscriptions.push(
        requests,
        answers,
        vscode.commands.registerCommand("ask.openPending", () => {
            const forms = pendingForms();
            if (!forms.length) void vscode.window.showInformationMessage("ask: no questions are waiting for an answer.");
            for (const { root, req } of forms) show(ctx, root, req);
        }),
    );
    const now = Date.now();
    for (const { root, req } of pendingForms()) if (now - Date.parse(req.createdAt) < RECENT_MS) show(ctx, root, req);
}

export function deactivate(): void {}
