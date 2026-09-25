/* index-ui.md §1: one contribution to the VSCode activity bar, and it is a
 * webview rather than a native `TreeView`.
 *
 * §1 GIVES THE REASON AND IT IS NOT AESTHETIC: "a native `TreeView` cannot
 * carry a search field and per-row metadata". §2's row is a title, a
 * collection, a date, a clamped line, a badge and a set of matched paths, with
 * a search box and a scope picker above it; a `TreeItem` has a label, a
 * description and a tooltip. The cost is that theming and keyboard navigation
 * have to be built rather than inherited, which is what `knowledge.css` and the
 * list's own key handling are.
 *
 * THE RAIL IS NAVIGATION AND A TAB IS A PLACE. Everything this view does with a
 * row is open its URI (§6). It renders no document and knows nothing about how
 * a tab is made.
 */

import * as vscode from "vscode";

import { HostContext, Surface, handleRequest, webviewHtml, webviewOptions } from "./host";
import { Response } from "./protocol";

export const DOCUMENTS_VIEW = "knowledge.documents";

export class Sidebar implements vscode.WebviewViewProvider {
    private view: vscode.WebviewView | null = null;

    constructor(private readonly ctx: HostContext) {}

    static register(ctx: HostContext): { sidebar: Sidebar; disposable: vscode.Disposable } {
        const sidebar = new Sidebar(ctx);
        return {
            sidebar,
            disposable: vscode.window.registerWebviewViewProvider(DOCUMENTS_VIEW, sidebar, {
                /* A rail that rebuilt its list every time the reader looked at
                 * another view would lose the query they had typed — and §2
                 * searches as they type, so what is lost is a question in
                 * progress rather than a scroll position. */
                webviewOptions: { retainContextWhenHidden: true },
            }),
        };
    }

    resolveWebviewView(view: vscode.WebviewView): void {
        this.view = view;
        view.webview.options = webviewOptions(this.ctx);
        view.webview.html = webviewHtml(this.ctx, view.webview, { view: "sidebar" });
        const surface: Surface = { webview: view.webview };
        view.webview.onDidReceiveMessage((m) => handleRequest(this.ctx, surface, m));
        view.onDidDispose(() => {
            this.view = null;
        });
    }

    notify(response: Response): void {
        if (this.view === null) {
            return;
        }
        void this.view.webview.postMessage(response);
    }

    /* §4: "a collection row opens the sidebar scoped to it."
     *
     * THE VIEW IS REVEALED BEFORE IT IS TOLD. A scope message to a sidebar
     * nobody is looking at is a filter applied out of sight, which is exactly
     * the "near-empty list with no reason on screen" a filter must never be.
     * `show` also resolves the view if it has never been opened, which is the
     * case the first time somebody clicks a collection row. */
    async scope(collection: string): Promise<void> {
        await vscode.commands.executeCommand(`${DOCUMENTS_VIEW}.focus`);
        this.notify({ kind: "scope", collection });
    }
}
