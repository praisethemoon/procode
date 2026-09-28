/* Artifacts in VS Code (specs/artifacts.md §5): the list of what agents have
 * published into the workspace, and a tab per artifact that renders its page
 * in the editor's theme.
 *
 * The store is the `artifacts` package, read fresh on every refresh: the
 * files are small, and an agent writing through MCP is a different process,
 * so there is no cache here that could disagree with the disk.
 */

import * as fs from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";

import { Artifact, Artifacts, findArtifacts } from "artifacts";

import { FrameParts, frameDocument } from "./frame";
import { describeRow, sortForList } from "./list";
import { viewerHtml } from "./viewer";

const panels = new Map<string, vscode.WebviewPanel>();

function store(): Artifacts | null {
    const folder = vscode.workspace.workspaceFolders?.find((f) => f.uri.scheme === "file");
    if (!folder) return null;
    const root = findArtifacts(folder.uri.fsPath);
    return new Artifacts(root ?? folder.uri.fsPath);
}

class Item extends vscode.TreeItem {
    constructor(readonly artifact: Artifact) {
        super(artifact.title, vscode.TreeItemCollapsibleState.None);
        this.id = artifact.id;
        this.description = describeRow(artifact, Date.now());
        const tip = new vscode.MarkdownString();
        tip.appendMarkdown(`**${artifact.title.replace(/[\\`*_[\]]/g, "\\$&")}**\n\n`);
        if (artifact.description) tip.appendText(`${artifact.description}\n\n`);
        if (artifact.keywords.length) tip.appendText(`keywords: ${artifact.keywords.join(", ")}\n\n`);
        tip.appendText(`${artifact.id} · created ${artifact.createdAt} · updated ${artifact.updatedAt}`);
        this.tooltip = tip;
        this.iconPath = new vscode.ThemeIcon("preview");
        this.contextValue = "artifact";
        this.command = { command: "artifacts.open", title: "Open", arguments: [artifact.id] };
    }
}

class Tree implements vscode.TreeDataProvider<Item> {
    private readonly changed = new vscode.EventEmitter<void>();
    readonly onDidChangeTreeData = this.changed.event;

    refresh(): void {
        this.changed.fire();
    }

    getTreeItem(item: Item): vscode.TreeItem {
        return item;
    }

    getChildren(): Item[] {
        return sortForList(store()?.list() ?? []).map((a) => new Item(a));
    }
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

function idOf(arg: unknown): string | undefined {
    if (arg instanceof Item) return arg.artifact.id;
    return typeof arg === "string" ? arg : undefined;
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
    const tree = new Tree();
    ctx.subscriptions.push(vscode.window.registerTreeDataProvider("artifacts.list", tree));

    let pending: NodeJS.Timeout | undefined;
    const changed = () => {
        if (pending) clearTimeout(pending);
        pending = setTimeout(() => {
            tree.refresh();
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
