/* Where the store is, and the one client that reads it.
 *
 * ONE CLIENT FOR THE WINDOW. `kb-js` spawns a process per call and holds
 * nothing between them, so a client is only a binary path, a working directory
 * and an environment — but those three are exactly what decides WHICH STORE is
 * being read (index-api.md §1.4 finds the one `.kb/` by walking up from the
 * working directory, like `.git`), and a second client built somewhere else
 * would quietly answer about a different store.
 *
 * THE WORKING DIRECTORY IS THE FIRST FILE-SCHEME WORKSPACE FOLDER. A folder on
 * a remote or virtual scheme has no path for a child process to run in, and a
 * multi-root workspace has no single answer — §1.4's walk is defined from one
 * directory. Taking the first file-scheme folder is the rule `coboard-vscode`
 * uses to find its board, and stating it here is what keeps two extensions in
 * one window from disagreeing about which project they are in.
 *
 * NO FOLDER, NO CLIENT. Without a working directory to give it, `kb-js` would
 * spawn in whatever directory the extension host happened to start in, and the
 * walk up from there would find — or fail to find — a store that has nothing
 * to do with this window. `makeClient` answers `undefined` instead, and the
 * callers say that no folder is open rather than running `kb` somewhere
 * arbitrary.
 *
 * NOTHING HERE WRITES. Reading is every method but three, and those three are
 * the explicit actions §2's title bar and §4's rows offer — which go through
 * the CLI, because §10 makes the CLI the only code that writes.
 */

import * as vscode from "vscode";

import { Kb, KbError, KbOptions } from "kb-js";

export const CONFIG_SECTION = "knowledge";

export interface Settings {
    readonly cliPath: string;
    readonly staleAfterDays: number;
}

export function readSettings(): Settings {
    const config = vscode.workspace.getConfiguration(CONFIG_SECTION);
    const days = config.get<number>("staleAfterDays", 90);
    return {
        cliPath: config.get<string>("cliPath", "kb").trim() || "kb",
        /* A negative or unreadable threshold badges nothing rather than
         * everything: `kb-js`'s `isStale` refuses it, and this keeps the
         * number that reaches the webview honest about what it is. */
        staleAfterDays: Number.isFinite(days) && days >= 0 ? days : 90,
    };
}

export function workspaceRoot(): string | undefined {
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        if (folder.uri.scheme === "file") {
            return folder.uri.fsPath;
        }
    }
    return undefined;
}

export function clientOptions(settings: Settings, cwd: string): KbOptions {
    return { bin: settings.cliPath, cwd };
}

export function makeClient(settings: Settings): Kb | undefined {
    const root = workspaceRoot();
    return root === undefined ? undefined : new Kb(clientOptions(settings, root));
}

/* What a reader is told when there is no folder to find a store from. */
export const NO_FOLDER =
    "No folder is open. Knowledge reads the .kb directory found by walking up from the workspace folder; open a folder first.";

/* The same sentence as §11's `not_found`, which is what the CLI itself answers
 * outside any store — so the webviews draw it as the refusal it is, with no
 * branch of their own for a window without a folder. */
export function noFolder(): KbError {
    return new KbError("not_found", NO_FOLDER, []);
}
