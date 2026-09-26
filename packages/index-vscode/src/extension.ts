/* Activation: register the sidebar and the `kb:` editor, wire the commands,
 * and keep both told when the store moves.
 *
 * NO `FileSystemProvider` IS REGISTERED FOR `kb:`, AND THAT IS THE POINT
 * (index-ui.md §6, and `editor.ts`'s header at length). VSCode decides whether
 * a URI is a file by asking whether the file service has a provider for the
 * scheme, and `breadcrumbsControl.ts` hides on exactly
 *
 *     if (!uri || !this._fileService.hasProvider(uri)) { ...this.hide(); }
 *
 * — the editor-type dropdown that offers to reopen the resource with another
 * editor being drawn by the same control behind the same guard. A provider
 * whose every read answered an empty file would therefore buy a breadcrumb
 * naming a path that does not exist and nothing else. `CustomEditorInput.resolve`
 * never asks for one, so tabs still restore, dedupe by URI, and links still
 * resolve. The cost is Quick Open by URI, and §5 replaces it with a command
 * that is better at the job anyway.
 *
 * THERE IS NO SESSION TO HOLD. `kb-js` spawns a process per call and keeps
 * nothing between them, so what this file holds is the settings and a client
 * built from them — and the client is rebuilt when the settings change, because
 * a binary path read once at activation is a binary path that is wrong for the
 * rest of the window.
 *
 * THE STORE IS WATCHED RATHER THAN SUBSCRIBED TO. `.kb/documents.jsonl` is
 * append-only and there is no daemon to push from, so a file watcher over the
 * two logs is what "something changed" means — and the views re-ask rather than
 * being told what changed, which is the same discipline coboard follows for the
 * same reason.
 */

import * as vscode from "vscode";

import { Kb } from "kb-js";

import { HostContext } from "./host";
import { KnowledgeEditor } from "./editor";
import { NO_FOLDER, Settings, makeClient, noFolder, readSettings, workspaceRoot } from "./session";
import { Sidebar } from "./sidebar";
import { addCurrentFile, addFiles, addUrl, refreshStale } from "./commands";
import { quickSearch } from "./quickopen";

export function activate(context: vscode.ExtensionContext): void {
    let settings: Settings = readSettings();
    let client: Kb | undefined = makeClient(settings);

    /* A command with no folder to run in says so and does nothing; see
     * `session.ts`. The webviews get the same sentence as a refusal. */
    const withClient =
        (run: (kb: Kb, ...args: unknown[]) => unknown) =>
        (...args: unknown[]): unknown => {
            if (client === undefined) {
                void vscode.window.showWarningMessage(NO_FOLDER);
                return undefined;
            }
            return run(client, ...args);
        };

    const ctx: HostContext = {
        extensionUri: context.extensionUri,
        client: () => {
            if (client === undefined) {
                throw noFolder();
            }
            return client;
        },
        settings: () => settings,
        announce: () => announce(),
        open: (reference, chunk, preview) => void editors.provider.open(reference, chunk, preview),
        scope: (collection) => void rail.sidebar.scope(collection),
        retitle: (reference, title) => editors.provider.retitle(reference, title),
    };

    const editors = KnowledgeEditor.register(ctx);
    context.subscriptions.push(editors.disposable);

    const rail = Sidebar.register(ctx);
    context.subscriptions.push(rail.disposable);

    const announce = (): void => {
        editors.provider.notify({ kind: "changed" });
        rail.sidebar.notify({ kind: "changed" });
    };

    /* The two append-only logs of §1.6, in the workspace folder's `.kb/`. A
     * store found further up, above the folder, is outside the workspace and
     * cannot be watched by a workspace watcher — a reader whose store changed
     * under them there sees it on the next refresh, which is the same answer
     * they would get from a daemon that did not exist. */
    const root = workspaceRoot();
    if (root !== undefined) {
        const watcher = vscode.workspace.createFileSystemWatcher(
            new vscode.RelativePattern(root, ".kb/{documents,sources}.jsonl"),
        );
        /* The watcher fires from a timer, where nothing is catching. Every call
         * below it answers rather than throws, but a bug above that would
         * otherwise take the extension host down for a refresh — so the
         * boundary is closed here as well. */
        const safely = (): void => {
            try {
                announce();
            } catch (e) {
                void vscode.window.showErrorMessage(
                    `Knowledge could not refresh: ${e instanceof Error ? e.message : String(e)}`,
                );
            }
        };
        watcher.onDidChange(safely);
        watcher.onDidCreate(safely);
        watcher.onDidDelete(safely);
        context.subscriptions.push(watcher);
    }

    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration((e) => {
            if (!e.affectsConfiguration("knowledge")) {
                return;
            }
            settings = readSettings();
            client = makeClient(settings);
            /* The threshold is written into every webview document at build
             * time (`protocol.ts`'s ViewTag), so a changed one needs the pages
             * rebuilt rather than merely re-asked. The views reload themselves
             * on `changed`; what they cannot do is re-read a tag they were
             * handed at load, which is why the rail is re-resolved. */
            announce();
        }),
        vscode.workspace.onDidChangeWorkspaceFolders(() => {
            client = makeClient(settings);
            announce();
        }),
    );

    context.subscriptions.push(
        vscode.commands.registerCommand("knowledge.open", (reference: unknown) => {
            if (typeof reference === "string") {
                return editors.provider.open(reference, null, false);
            }
            return vscode.window
                .showInputBox({
                    title: "Open a Knowledge reference",
                    prompt: "A public identifier: D-241 for a document, S-3 for a source, or collections.",
                    placeHolder: "D-241",
                })
                .then((value) =>
                    value === undefined ? undefined : editors.provider.open(value, null, false),
                );
        }),
        vscode.commands.registerCommand(
            "knowledge.search",
            withClient((kb) =>
                quickSearch(kb, (reference, chunk) =>
                    void editors.provider.open(reference, chunk, false),
                ),
            ),
        ),
        vscode.commands.registerCommand("knowledge.collections", () =>
            editors.provider.open("collections", null, false),
        ),
        vscode.commands.registerCommand("knowledge.graph", () => editors.provider.open("graph", null, false)),
        vscode.commands.registerCommand(
            "knowledge.addCurrentFile",
            withClient((kb) => addCurrentFile(kb, announce)),
        ),
        vscode.commands.registerCommand(
            "knowledge.addFiles",
            withClient((kb, collection?: unknown) =>
                addFiles(kb, announce, typeof collection === "string" && collection ? collection : undefined),
            ),
        ),
        vscode.commands.registerCommand(
            "knowledge.addUrl",
            withClient((kb) => addUrl(kb, announce)),
        ),
        vscode.commands.registerCommand(
            "knowledge.refreshStale",
            withClient((kb) => refreshStale(kb, settings.staleAfterDays, announce)),
        ),
        /* `kb init` creates `.kb/` in the client's directory, which is the
         * workspace folder — there is no other place a store can go. */
        vscode.commands.registerCommand("knowledge.init", withClient(async (kb) => {
            try {
                const created = await kb.init();
                announce();
                void vscode.window.showInformationMessage(
                    `Created ${created.path}. The logs and the blobs are the truth and are worth committing; index/ is derived and is gitignored for you.`,
                );
            } catch (e) {
                void vscode.window.showErrorMessage(
                    e instanceof Error ? e.message : String(e),
                );
            }
        })),
    );
}

export function deactivate(): void {
    /* Every disposable is on the context; nothing here holds a handle of its
     * own. There is no store to close: `kb` is a process that has already
     * exited by the time any call returns. */
}
