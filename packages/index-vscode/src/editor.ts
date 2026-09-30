/* index-ui.md §3 and §4 as URI-addressed read-only custom editors on the `kb:`
 * scheme.
 *
 * READ-ONLY, AND THAT IS NOT A DEFAULT. §3 gives the reason: "editing an
 * indexed copy of someone else's documentation would make the content hash
 * meaningless and the provenance a lie". `CustomReadonlyEditorProvider` —
 * rather than the editable one — keeps VSCode from offering a save that would
 * mean nothing and from prompting on close. There is no model, no buffer and
 * no backing file; the document is an identity handle and nothing else.
 *
 * WHY NOT `createWebviewPanel`. A bare panel would also produce tabs, but three
 * things that come free with a URI would each have to be rebuilt: tab restore
 * across a window reload (VSCode reopens editors by URI; bare panels need a
 * serialiser and quietly vanish without one), tab identity (§6: "opening a
 * document that already has a tab focuses it"), and link resolution — one
 * string is the reference and the editor URI, so resolving one is opening the
 * other.
 *
 * NO `FileSystemProvider` IS REGISTERED FOR `kb:`, AND THAT IS THE POINT.
 * VSCode decides whether a URI is a file by asking whether the file service has
 * a provider for the scheme, and hangs a header on the answer —
 * `breadcrumbsControl.ts` hides on exactly
 *
 *     if (!uri || !this._fileService.hasProvider(uri)) { ...this.hide(); }
 *
 * — and the editor-type dropdown that offers to reopen the resource with a
 * different editor is drawn by that same control behind that same guard. So a
 * provider, even one answering every read with an empty file, buys a breadcrumb
 * naming a path that does not exist and an editor picker that cannot open it,
 * and buys nothing else. `CustomEditorInput.resolve` never asks for one: tabs
 * still restore, still dedupe by URI, and links still resolve. What is given up
 * is Quick Open by URI — its history picker drops entries whose scheme has no
 * provider — and §5 replaces that with a command of its own.
 *
 * THE CHUNK TRAVELS AS A MESSAGE AND NEVER AS PART OF THE URI. §3.2 scrolls to
 * the matching chunk's heading when a search result is opened; putting it in
 * the URI would make `kb:/D-241` and `kb:/D-241?chunk=C-1` two resources and
 * therefore two tabs for one document, which is the failure §6 names by hand.
 * A tab that is already open is told; a tab that is about to open is told when
 * it resolves.
 */

import * as vscode from "vscode";

import { HostContext, Surface, broadcast, handleRequest, webviewHtml, webviewOptions } from "./host";
import { Response } from "./protocol";
import { KB_SCHEME, Target, fallbackTitle, isPlaceable, parseTarget, targetPath, targetRef } from "./uri";

export const VIEW_TYPE = "knowledge.document";

class KnowledgeDocument implements vscode.CustomDocument {
    constructor(readonly uri: vscode.Uri) {}
    dispose(): void {
        /* Nothing to release: there is no model, no buffer and no file. */
    }
}

export class KnowledgeEditor implements vscode.CustomReadonlyEditorProvider<KnowledgeDocument> {
    /* Every live webview and what it is a place for, so the watcher can tell
     * all of them at once that the store moved. This is the one piece of
     * bookkeeping the URI does not do for us, and it is about notification
     * rather than about identity — nothing here decides whether two opens are
     * one tab. */
    private readonly live = new Map<vscode.WebviewPanel, Target | null>();

    /* §3.2's scroll target, held between the `openWith` that starts a tab and
     * the `resolveCustomEditor` that finishes it. Keyed by canonical URI
     * because that is the only thing the two halves share. */
    private readonly pendingChunk = new Map<string, string>();

    constructor(private readonly ctx: HostContext) {}

    static register(ctx: HostContext): {
        provider: KnowledgeEditor;
        disposable: vscode.Disposable;
    } {
        const provider = new KnowledgeEditor(ctx);
        const disposable = vscode.window.registerCustomEditorProvider(VIEW_TYPE, provider, {
            /* One editor per document: the whole of §6's "opening a document
             * that already has a tab focuses it". */
            supportsMultipleEditorsPerDocument: false,
            webviewOptions: {
                /* A tab the reader comes back to should be where they left it.
                 * Rebuilding the React tree on every tab switch would also
                 * re-read the whole document from the store. */
                retainContextWhenHidden: true,
                /* ⌘F / Ctrl+F: VS Code's find widget, with its highlighting,
                 * next and previous, and match count, over the rendered page. */
                enableFindWidget: true,
            },
        });
        return { provider, disposable };
    }

    openCustomDocument(uri: vscode.Uri): KnowledgeDocument {
        if (uri.scheme !== KB_SCHEME) {
            /* The selector is `kb:/*`, matched against `scheme:path`, so
             * nothing else should arrive. Said plainly rather than rendered as
             * an empty tab, because the one way to get here is a file this
             * editor should never have been offered. */
            throw new Error(
                `Knowledge opens ${KB_SCHEME}: references and not ${uri.scheme}: resources. Reopen this file with the text editor.`,
            );
        }
        return new KnowledgeDocument(uri);
    }

    resolveCustomEditor(document: KnowledgeDocument, panel: vscode.WebviewPanel): void {
        const target = parseTarget(document.uri.path);
        panel.webview.options = webviewOptions(this.ctx);
        panel.webview.html = webviewHtml(this.ctx, panel.webview, {
            view: "entity",
            reference: target === null ? document.uri.path : targetRef(target),
        });

        const surface: Surface = { webview: panel.webview };
        panel.webview.onDidReceiveMessage((m) => handleRequest(this.ctx, surface, m));

        this.live.set(panel, target);
        panel.onDidDispose(() => this.live.delete(panel));

        if (target !== null) {
            panel.title = fallbackTitle(target);
            const chunk = this.pendingChunk.get(document.uri.toString());
            if (chunk !== undefined) {
                this.pendingChunk.delete(document.uri.toString());
                /* The document has to mount before it can scroll, and the
                 * webview's own script has not started yet — so the message is
                 * queued rather than sent. `postMessage` on a resolved webview
                 * is delivered once the page is listening, which is exactly the
                 * ordering this needs and is why there is no handshake. */
                void panel.webview.postMessage({ kind: "reveal", chunk } satisfies Response);
            }
        }
    }

    /* A tab's title is read from the store rather than stored on the tab, so a
     * document whose title changes renames its tab instead of leaving a stale
     * name sitting there until it is closed. The webview knows the title —
     * it has just read the document — so it says so rather than this file
     * making a second call for one string. */
    retitle(reference: string, title: string): void {
        for (const [panel, target] of this.live) {
            if (target !== null && targetRef(target) === reference) {
                panel.title = title.trim().length > 0 ? title : fallbackTitle(target);
            }
        }
    }

    notify(response: Response): void {
        broadcast([...this.live.keys()].map((p) => p.webview), response);
    }

    /* The one door to a tab.
     *
     * `vscode.openWith` names the view type explicitly and the resource is the
     * canonical URI — so a second open of the same document is the same
     * resource and the same view type, which is what makes VSCode reveal the
     * existing tab instead of building another. */
    async open(reference: string, chunk: string | null, preview: boolean): Promise<void> {
        const target = parseTarget(reference);
        if (target === null || !isPlaceable(target)) {
            void vscode.window.showWarningMessage(
                `"${reference}" is not a Knowledge place. References are D-241 for a document, S-3 for a source, or collections.`,
            );
            return;
        }
        const uri = vscode.Uri.from({ scheme: KB_SCHEME, path: targetPath(target) });
        if (chunk !== null && chunk.length > 0) {
            /* Told twice on purpose. A tab that is already open never resolves
             * again, so the queued value would sit unread; a tab that is about
             * to open is not listening yet, so the direct post would be lost.
             * Neither path alone covers both, and a reveal that silently did
             * nothing half the time is worse than one that fires twice. */
            this.pendingChunk.set(uri.toString(), chunk);
            for (const [panel, open] of this.live) {
                if (open !== null && targetRef(open) === targetRef(target)) {
                    this.pendingChunk.delete(uri.toString());
                    void panel.webview.postMessage({ kind: "reveal", chunk } satisfies Response);
                }
            }
        }
        await vscode.commands.executeCommand("vscode.openWith", uri, VIEW_TYPE, {
            preview,
            preserveFocus: preview,
        } satisfies vscode.TextDocumentShowOptions);
    }
}
