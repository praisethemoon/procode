/* Where the store is, and the one client that reads it.
 *
 * ONE CLIENT FOR THE WINDOW. `kb-js` spawns a process per call and holds
 * nothing between them, so a client is only a binary path, a working directory
 * and an environment — but those three are exactly what decides WHICH STORE is
 * being read (index-api.md §1.4 finds the project tier by walking up from the
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
 * NOTHING HERE WRITES. Reading is every method but three, and those three are
 * the explicit actions §2's title bar and §4's rows offer — which go through
 * the CLI, because §10 makes the CLI the only code that writes.
 */

import * as vscode from "vscode";

import { Kb, KbOptions } from "kb-js";

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

export function clientOptions(settings: Settings): KbOptions {
    return { bin: settings.cliPath, cwd: workspaceRoot() };
}

export function makeClient(settings: Settings): Kb {
    return new Kb(clientOptions(settings));
}
