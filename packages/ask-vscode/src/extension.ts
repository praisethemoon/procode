/* ask in VS Code (specs/ask.md §5): the MCP server agents ask through, and
 * a tab for each form they open.
 *
 * THE SERVER IS IN HERE. The extension serves ask's MCP tools over HTTP on
 * 127.0.0.1 (ask's http.ts), at a port that comes from the workspace folder,
 * so the address written into the folder's .mcp.json stays right across
 * restarts. A form an agent opens is an object in this process: the tab
 * opens at once, its answer goes back as the call's result, and nothing is
 * written to disk. Closing a tab without submitting cancels its form; a call
 * its agent cancels shows the tab closed.
 *
 * The server runs while the procode › MCP › ask setting is on (the default),
 * and stops and starts again as it changes. Stopping cancels the forms its
 * calls were waiting on.
 */

import * as crypto from "node:crypto";
import * as fs from "node:fs";
import * as vscode from "vscode";

import { Forms, Request, Served, portFor, serve } from "ask";

import type { PreviewParts } from "./preview";
import type { ToHost, ToView } from "./protocol";

interface Open {
    readonly panel: vscode.WebviewPanel;
    /** Set once the form is answered, so closing the tab cancels nothing. */
    done: boolean;
}

const forms = new Forms();
const open = new Map<string, Open>();
/** The server as last asked for: starting or running, or null while off. */
let server: Promise<Served | null> | null = null;
const serving = new Set<(url: string | undefined) => void>();

/** The address of this window's ask server once it is listening; undefined
 *  while it is off or when it could not start. What procode writes into
 *  .mcp.json and hands VS Code's own agent. */
export function whenServed(): Promise<string | undefined> {
    return (server ?? Promise.resolve(null)).then((s) => s?.url);
}

/** Called with the address each time the server starts, and with undefined
 *  each time it stops. */
export function onServing(fn: (url: string | undefined) => void): () => void {
    serving.add(fn);
    return () => serving.delete(fn);
}

function announce(url: string | undefined): void {
    for (const fn of serving) fn(url);
}

function enabled(): boolean {
    return vscode.workspace.getConfiguration("procode.mcp").get<boolean>("ask", true) !== false;
}

/* A stop still closing, which a start waits for so it gets the same port. */
let stopping: Promise<void> = Promise.resolve();

function start(): void {
    if (server) return;
    const folder = vscode.workspace.workspaceFolders?.find((f) => f.uri.scheme === "file")?.uri.fsPath ?? "";
    server = stopping.then(() => serve({ ctx: { forms }, port: portFor(folder) })).then(
        (s) => {
            announce(s.url);
            return s;
        },
        (e) => {
            void vscode.window.showErrorMessage(`ask: its MCP server could not start: ${(e as Error).message}`);
            return null;
        },
    );
}

function stop(): Promise<void> {
    const was = server;
    server = null;
    if (!was) return stopping;
    stopping = was.then(async (s) => {
        if (!s) return;
        await s.close();
        announce(undefined);
    });
    return stopping;
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

function show(ctx: vscode.ExtensionContext, req: Request): void {
    const existing = open.get(req.id);
    if (existing) {
        existing.panel.reveal();
        return;
    }
    if (forms.answer(req.id)) return;
    const media = vscode.Uri.joinPath(ctx.extensionUri, "out", "media");
    const panel = vscode.window.createWebviewPanel("ask.form", req.title, vscode.ViewColumn.Active, {
        enableScripts: true,
        retainContextWhenHidden: true,
        localResourceRoots: [media],
    });
    panel.iconPath = new vscode.ThemeIcon("comment-discussion");
    panel.webview.html = html(panel.webview, media, req.title);
    const entry: Open = { panel, done: false };
    open.set(req.id, entry);
    const post = (m: ToView) => void panel.webview.postMessage(m);
    panel.webview.onDidReceiveMessage((m: ToHost) => {
        if (m?.type === "ready") {
            post({ type: "load", request: req, preview: previewParts(media) });
        } else if (m?.type === "submit") {
            try {
                forms.submit(req.id, m.answer);
            } catch (e) {
                void vscode.window.showErrorMessage(`ask: the answer to ${req.id} was not taken: ${(e as Error).message}`);
                return;
            }
            entry.done = true;
            panel.dispose();
        } else if (m?.type === "dismiss") {
            panel.dispose();
        }
    });
    panel.onDidDispose(() => {
        open.delete(req.id);
        if (!entry.done) forms.cancel(req.id);
    });
}

export function activate(ctx: vscode.ExtensionContext): void {
    const offOpen = forms.on("open", (req) => show(ctx, req));
    // Answered elsewhere: the agent's call was cancelled, or it hung up.
    const offAnswer = forms.on("answer", (id, answer) => {
        const entry = open.get(id);
        if (!entry || entry.done) return;
        entry.done = true;
        void entry.panel.webview.postMessage({ type: "closed", status: answer.status } satisfies ToView);
    });
    if (enabled()) start();
    ctx.subscriptions.push(
        { dispose: offOpen },
        { dispose: offAnswer },
        { dispose: () => void stop() },
        vscode.workspace.onDidChangeConfiguration((e) => {
            if (!e.affectsConfiguration("procode.mcp.ask")) return;
            if (enabled()) start();
            else void stop();
        }),
        vscode.commands.registerCommand("ask.openPending", () => {
            const pending = forms.pending();
            if (!pending.length) void vscode.window.showInformationMessage("ask: no questions are waiting for an answer.");
            for (const req of pending) show(ctx, req);
        }),
    );
}

export function deactivate(): Promise<void> {
    return stop();
}
