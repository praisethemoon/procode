/* procode: Lap History, Knowledge and the Board as one extension, with the
 * lap and kb CLIs inside it.
 *
 * Each part is the extension it always was — its own activate(), its own
 * views and commands (the manifest is merged from theirs at build time) — and
 * this file only starts them in turn. The CLIs ship in <extension>/bin and go
 * first on PATH, so Knowledge's `kb` and the Board's `lap` are the ones built
 * with this package unless a setting names another.
 */

import { execFile } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import * as vscode from "vscode";

import { addArgv, findClaude, forClaude, removeArgv, shellLine, signature } from "./claude";
import { outdated, withServers } from "./mcpjson";

export { addArgv, findClaude, forClaude, removeArgv, shellLine, signature };

interface Part {
    activate(ctx: vscode.ExtensionContext): unknown;
    deactivate?(): unknown;
}

/* The built extensions, bundled into this one by esbuild. */
const PARTS: readonly [string, Part][] = [
    ["Lap History", require("../../lap-vscode/out/extension.js") as Part],
    ["Knowledge", require("../../index-vscode/out/extension.js") as Part],
    ["Board", require("../../coboard-vscode/out/extension.js") as Part],
];

export function binDir(ctx: vscode.ExtensionContext): string {
    return path.join(ctx.extensionPath, "bin");
}

/* ------------------------------------------------------------ MCP servers
 *
 * coboard's and kb's MCP servers, each bundled into one script under
 * out/mcp. They run on VS Code's own runtime (the extension host's
 * executable with ELECTRON_RUN_AS_NODE=1), so no separate Node is needed,
 * and they are handed the bundled CLIs by path. */

const VERSION = "0.1.0";

export interface Server {
    readonly name: string;
    readonly label: string;
    readonly command: string;
    readonly args: string[];
    readonly env: Record<string, string>;
}

export function servers(ctx: vscode.ExtensionContext): Server[] {
    const bin = binDir(ctx);
    const script = (name: string) => path.join(ctx.extensionPath, "out", "mcp", `${name}.js`);
    const node = { ELECTRON_RUN_AS_NODE: "1" };
    return [
        {
            name: "coboard",
            label: "coboard: the board",
            command: process.execPath,
            args: [script("coboard")],
            env: { ...node, LAP_BIN: path.join(bin, "lap") },
        },
        {
            name: "kb",
            label: "kb: the knowledge base",
            command: process.execPath,
            args: [script("kb")],
            env: { ...node, KB_BIN: path.join(bin, "kb") },
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
 * paths in it — and keeps every other server in the file. Registering at user
 * scope (below) is the default; this is for anyone who wants it per project. After an update moves the extension's folder, the entries it
 * wrote point at a copy that is gone; activation notices and offers to fix
 * them. */

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
        "procode: coboard and kb are in .mcp.json. Restart Claude Code in this project (or check /mcp) to pick them up.",
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

/* ------------------------------------------------- Claude Code, user scope
 *
 * Registered once per machine rather than written into each project (see
 * claude.ts). The first start that finds the `claude` CLI offers it; after
 * that, an update that moved the extension's folder re-registers silently,
 * because the entries point at a folder the update removed. The state is the
 * signature of what was registered, or "declined". */

const USER_SCOPE = "procode.claude.userScope";

function claudeCli(): string | null {
    return findClaude(process.env["PATH"] ?? "", os.homedir(), (p) => {
        try {
            return fs.statSync(p).isFile();
        } catch {
            return false;
        }
    });
}

function run(cli: string, argv: string[]): Promise<void> {
    return new Promise((resolve, reject) => {
        execFile(cli, argv, { cwd: os.homedir(), timeout: 30_000 }, (err, _out, stderr) =>
            err ? reject(new Error(stderr.trim() || err.message)) : resolve(),
        );
    });
}

export async function registerUserScope(ctx: vscode.ExtensionContext, cli: string): Promise<void> {
    const entries = servers(ctx).map(forClaude);
    for (const s of entries) {
        await run(cli, removeArgv(s.name)).catch(() => undefined); // not there yet
        await run(cli, addArgv(s));
    }
    await ctx.globalState.update(USER_SCOPE, signature(entries));
}

async function registerClaudeMcp(ctx: vscode.ExtensionContext): Promise<void> {
    const cli = claudeCli();
    if (cli === null) {
        const lines = servers(ctx)
            .map(forClaude)
            .map((s) => shellLine(addArgv(s)))
            .join("\n");
        await vscode.env.clipboard.writeText(lines);
        void vscode.window.showInformationMessage(
            "procode: the claude CLI was not found. The two commands that register coboard and kb for every project are on the clipboard; run them in a terminal.",
        );
        return;
    }
    try {
        await registerUserScope(ctx, cli);
        void vscode.window.showInformationMessage(
            "procode: coboard and kb are registered with Claude Code for every project. Start a new Claude Code session (or check /mcp) to pick them up.",
        );
    } catch (e) {
        void vscode.window.showErrorMessage(`procode: registering with Claude Code failed: ${(e as Error).message}`);
    }
}

export async function refreshClaudeUserScope(ctx: vscode.ExtensionContext): Promise<void> {
    const state = ctx.globalState.get<string>(USER_SCOPE);
    const cli = claudeCli();
    if (cli === null || state === "declined") {
        return;
    }
    if (state === undefined) {
        const choice = await vscode.window.showInformationMessage(
            "procode: register the coboard and kb MCP servers with Claude Code, for every project on this machine?",
            "Register",
            "No thanks",
        );
        if (choice === "Register") {
            await registerClaudeMcp(ctx);
        } else {
            // Offered once; the command stays in the palette.
            await ctx.globalState.update(USER_SCOPE, "declined");
        }
        return;
    }
    if (state === signature(servers(ctx).map(forClaude))) {
        return;
    }
    try {
        await registerUserScope(ctx, cli);
    } catch (e) {
        void vscode.window.showWarningMessage(
            `procode: Claude Code still runs coboard and kb from an older procode, and updating them failed: ${(e as Error).message}`,
        );
    }
}

export function activate(ctx: vscode.ExtensionContext): void {
    const bin = binDir(ctx);
    const current = process.env["PATH"] ?? "";
    if (!current.split(path.delimiter).includes(bin)) {
        process.env["PATH"] = bin + path.delimiter + current;
    }
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
        vscode.commands.registerCommand("procode.registerClaudeMcp", () => registerClaudeMcp(ctx)),
    );
    checkClaudeMcp(ctx);
    void refreshClaudeUserScope(ctx);
}

export function deactivate(): void {
    for (const [, part] of PARTS) {
        part.deactivate?.();
    }
}
