/* procode: Lap History, Knowledge, the Board and Artifacts as one extension.
 *
 * Each part is the extension it always was — its own activate(), its own
 * views and commands (the manifest is merged from theirs at build time) — and
 * this file only starts them in turn. The lap and kb CLIs are not in the
 * package: the user builds them, and each part finds its CLI through its own
 * setting (clis.ts). That keeps the package free of native code, so one .vsix
 * installs on every platform.
 */

import * as fs from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";

import { forClaude } from "./claude";
import { CLIS, missingMessage, resolveCli } from "./clis";
import { outdated, withServers } from "./mcpjson";

export { forClaude, resolveCli };

interface Part {
    activate(ctx: vscode.ExtensionContext): unknown;
    deactivate?(): unknown;
}

/* The built extensions, bundled into this one by esbuild. */
const PARTS: readonly [string, Part][] = [
    ["Lap History", require("../../lap-vscode/out/extension.js") as Part],
    ["Knowledge", require("../../index-vscode/out/extension.js") as Part],
    ["Board", require("../../coboard-vscode/out/extension.js") as Part],
    ["Artifacts", require("../../artifacts-vscode/out/extension.js") as Part],
];

/* The command a CLI's setting names, "kb" or "lap" when it names none. */
function cliCommand(settingId: string, fallback: string): string {
    const [section, key] = settingId.split(".");
    return vscode.workspace.getConfiguration(section).get<string>(key, fallback)?.trim() || fallback;
}

/* Board › Board Folder, handed to coboard's server as COBOARD_DIR so agents
 * work the board this window does; nothing when it is empty. */
function boardFolder(): Record<string, string> {
    const dir = vscode.workspace.getConfiguration("coboard").get<string>("boardFolder", "")?.trim();
    return dir ? { COBOARD_DIR: dir } : {};
}

/* ------------------------------------------------------------ MCP servers
 *
 * coboard's, kb's and artifacts' MCP servers, each bundled into one script under
 * out/mcp. They run on VS Code's own runtime (the extension host's
 * executable with ELECTRON_RUN_AS_NODE=1), so no separate Node is needed,
 * and they are handed the CLIs the settings name. */

const VERSION = "0.1.0";

export interface Server {
    readonly name: string;
    readonly label: string;
    readonly command: string;
    readonly args: string[];
    readonly env: Record<string, string>;
}

export function servers(ctx: vscode.ExtensionContext): Server[] {
    const script = (name: string) => path.join(ctx.extensionPath, "out", "mcp", `${name}.js`);
    const node = { ELECTRON_RUN_AS_NODE: "1" };
    return [
        {
            name: "coboard",
            label: "coboard: the board",
            command: process.execPath,
            args: [script("coboard")],
            env: { ...node, LAP_BIN: cliCommand("coboard.lapPath", "lap"), ...boardFolder() },
        },
        {
            name: "kb",
            label: "kb: the knowledge base",
            command: process.execPath,
            args: [script("kb")],
            env: { ...node, KB_BIN: cliCommand("knowledge.cliPath", "kb") },
        },
        {
            name: "artifacts",
            label: "artifacts: pages agents publish",
            command: process.execPath,
            args: [script("artifacts")],
            env: { ...node },
        },
    ];
}

/* VS Code's own agent finds the servers without any configuration. Each runs
 * in the workspace folder, which is where it finds that workspace's .coboard/
 * and .kb/. */
function registerWithVsCode(ctx: vscode.ExtensionContext): void {
    if (typeof vscode.lm?.registerMcpServerDefinitionProvider !== "function") {
        return; // a VS Code older than the MCP API: the Claude Code command still works
    }
    ctx.subscriptions.push(
        vscode.lm.registerMcpServerDefinitionProvider("procode.mcp", {
            provideMcpServerDefinitions: () => {
                const folder = vscode.workspace.workspaceFolders?.find((f) => f.uri.scheme === "file")?.uri;
                return servers(ctx).map((s) => {
                    const d = new vscode.McpStdioServerDefinition(s.label, s.command, s.args, s.env, VERSION);
                    if (folder) {
                        d.cwd = folder;
                    }
                    return d;
                });
            },
        }),
    );
}

/* ------------------------------------------------------------ Claude Code
 *
 * Claude Code reads a project's MCP servers from .mcp.json at its root, which
 * VS Code cannot fill in. This writes our two entries there — only when asked,
 * because it is a file in the user's project, and one with this machine's
 * paths in it — and keeps every other server in the file. After an update moves the
 * extension's folder, the entries it wrote point at a copy that is gone;
 * activation notices and offers to fix them. */

function mcpJsonPath(): string | null {
    const folder = vscode.workspace.workspaceFolders?.find((f) => f.uri.scheme === "file");
    return folder ? path.join(folder.uri.fsPath, ".mcp.json") : null;
}

/* The parsed file, {} when there is none, or null when it is there and is not
 * a JSON object — which is never overwritten. */
function readMcpJson(file: string): Record<string, unknown> | null {
    let text: string;
    try {
        text = fs.readFileSync(file, "utf8");
    } catch {
        return {};
    }
    try {
        const v = JSON.parse(text) as unknown;
        return typeof v === "object" && v !== null && !Array.isArray(v) ? (v as Record<string, unknown>) : null;
    } catch {
        return null;
    }
}

function setUpClaudeMcp(ctx: vscode.ExtensionContext): void {
    const file = mcpJsonPath();
    if (!file) {
        void vscode.window.showWarningMessage("procode: open a folder first; Claude Code's MCP servers are set per project.");
        return;
    }
    const current = readMcpJson(file);
    if (current === null) {
        void vscode.window.showErrorMessage(`procode: ${file} is not a JSON object, so it was left alone. Fix it and run this again.`);
        return;
    }
    fs.writeFileSync(file, JSON.stringify(withServers(current, servers(ctx).map(forClaude)), null, 2) + "\n");
    void vscode.window.showInformationMessage(
        "procode: coboard, kb and artifacts are in .mcp.json. Restart Claude Code in this project (or check /mcp) to pick them up.",
    );
}

function checkClaudeMcp(ctx: vscode.ExtensionContext): void {
    const file = mcpJsonPath();
    const current = file ? readMcpJson(file) : null;
    if (!current) {
        return;
    }
    const stale = outdated(current, servers(ctx));
    if (stale.length === 0) {
        return;
    }
    void vscode.window
        .showWarningMessage(`procode: .mcp.json runs ${stale.join(" and ")} from an older procode that is no longer installed.`, "Update")
        .then((choice) => {
            if (choice === "Update") {
                setUpClaudeMcp(ctx);
            }
        });
}

/* Says once per window, for each CLI its setting cannot reach, what cannot
 * run and how to fix it. The parts report their own failures when used; this
 * is the one place that says it up front, with the setting one click away. */
function checkClis(): void {
    const isFile = (p: string) => {
        try {
            return fs.statSync(p).isFile();
        } catch {
            return false;
        }
    };
    for (const cli of CLIS) {
        const command = cliCommand(cli.settingId, cli.name);
        if (resolveCli(command, process.env["PATH"] ?? "", isFile)) continue;
        void vscode.window.showWarningMessage(missingMessage(cli, command), "Open Setting").then((choice) => {
            if (choice === "Open Setting") {
                void vscode.commands.executeCommand("workbench.action.openSettings", cli.settingId);
            }
        });
    }
}

export function activate(ctx: vscode.ExtensionContext): void {
    for (const [name, part] of PARTS) {
        try {
            void part.activate(ctx);
        } catch (e) {
            // One part failing to start must not take the other two with it.
            void vscode.window.showErrorMessage(`procode: ${name} failed to start: ${(e as Error).message}`);
        }
    }
    registerWithVsCode(ctx);
    ctx.subscriptions.push(
        vscode.commands.registerCommand("procode.setUpClaudeMcp", () => setUpClaudeMcp(ctx)),
    );
    checkClaudeMcp(ctx);
    checkClis();
}

export function deactivate(): void {
    for (const [, part] of PARTS) {
        part.deactivate?.();
    }
}
