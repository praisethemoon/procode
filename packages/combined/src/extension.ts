/* procode: Lap History, Knowledge, the Board and techdocs as one extension.
 *
 * Each part is the extension it always was — its own activate(), its own
 * views and commands (the manifest is merged from theirs at build time) — and
 * this file only starts them in turn. A platform package carries the lap and
 * kb CLIs in its bin/, and they are placed in ~/.procode/bin before any part
 * starts (procodebin.ts); the package for every platform carries none, and
 * the user builds them. Either way each part finds its CLI through its own
 * setting (clis.ts), else ~/.procode/bin, else PATH.
 */

import * as fs from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";

import { forClaude } from "./claude";
import { CLIS, Cli, missingMessage, resolveCli, serverNeeds } from "./clis";
import { outdated, withServers, writtenEntries } from "./mcpjson";
import { commandFor, exeName, place, procodeBinDir, sourceOf } from "./procodebin";
import { ShippedSkill, behind, folderHash, install, shippedSkills, stateOf } from "./skills";

export { commandFor, folderHash, forClaude, outdated, place, resolveCli, sourceOf, withServers, writtenEntries };

interface Part {
    activate(ctx: vscode.ExtensionContext): unknown;
    deactivate?(): unknown;
    /** ask's: the address its MCP server listens on, once it does. */
    whenServed?(): Promise<string | undefined>;
    /** ask's: told each time its server starts (with the address) or stops. */
    onServing?(fn: (url: string | undefined) => void): () => void;
}

/* Parts a build adds only when asked to (scripts/build.mjs --with-ask). Each
 * is its own bundle in out/parts/, found when the extension loads, so a build
 * without one carries nothing of it and this file names it only here. */
const OPTIONAL = ["ask"] as const;

function optionalParts(): [string, Part][] {
    return OPTIONAL.map((name) => [name, path.join(__dirname, "parts", `${name}.js`)] as const)
        .filter(([, file]) => fs.existsSync(file))
        .map(([name, file]) => [name, require(file) as Part]);
}

/* The built extensions, bundled into this one by esbuild, then any optional
 * part this build carries. */
const PARTS: readonly [string, Part][] = [
    ["Lap History", require("../../lap-vscode/out/extension.js") as Part],
    ["Knowledge", require("../../index-vscode/out/extension.js") as Part],
    ["Board", require("../../coboard-vscode/out/extension.js") as Part],
    ["techdocs", require("../../techdocs-vscode/out/extension.js") as Part],
    ...optionalParts(),
];

/* ask's part, in a build that carries it. Its MCP server runs inside it. */
const ASK = PARTS.find(([name]) => name === "ask")?.[1];
/* Its address once listening; read by servers(). */
let askUrl: string | undefined;

/* The settings that can name each CLI, first match wins: lap's own view has
 * lap.path besides the Board's setting. */
const CLI_SETTINGS: Record<Cli["name"], readonly string[]> = {
    kb: ["knowledge.cliPath"],
    lap: ["coboard.lapPath", "lap.path"],
};

/* The command a setting the user set names for a CLI; undefined when every
 * one is at its default. */
function setByUser(cli: Cli["name"]): string | undefined {
    for (const id of CLI_SETTINGS[cli]) {
        const [section, key] = id.split(".");
        const i = vscode.workspace.getConfiguration(section).inspect<string>(key);
        const value = (i?.workspaceFolderValue ?? i?.workspaceValue ?? i?.globalValue)?.trim();
        if (value) return value;
    }
    return undefined;
}

/* The command to run a CLI: the user's setting, else ~/.procode/bin's, else
 * the bare name for PATH. */
function cliCommand(cli: Cli["name"]): string {
    return commandFor(cli, setByUser(cli), isFile);
}

/* Board › Board Folder, handed to coboard's server as COBOARD_DIR so agents
 * work the board this window does; nothing when it is empty. */
function boardFolder(): Record<string, string> {
    const dir = vscode.workspace.getConfiguration("coboard").get<string>("boardFolder", "")?.trim();
    return dir ? { COBOARD_DIR: dir } : {};
}

/* ------------------------------------------------------------ MCP servers
 *
 * coboard's, kb's and techdocs's MCP servers, each bundled into one script under
 * out/mcp. They run on VS Code's own runtime (the extension host's
 * executable with ELECTRON_RUN_AS_NODE=1), so no separate Node is needed,
 * and they are handed the CLIs the settings name. */

export type Server =
    | { readonly name: string; readonly label: string; readonly command: string; readonly args: string[]; readonly env: Record<string, string> }
    /* ask's, which runs inside this extension and is reached over HTTP. */
    | { readonly name: string; readonly label: string; readonly url: string; readonly timeout?: number };

/* ------------------------------------------------------- on and off
 *
 * Each server has a switch, procode › MCP › <name>, on by default. A server
 * that is off is left out of VS Code's agent and out of what Set Up MCP for
 * Claude Code writes, which also takes out the entry it once wrote for it;
 * ask's server stops listening (ask-vscode follows its own switch). */

/* The servers this build can run, in the order they are listed. */
export function mcpNames(): string[] {
    return ["coboard", "kb", "techdocs", ...(ASK ? ["ask"] : [])];
}

export function mcpEnabled(name: string): boolean {
    return vscode.workspace.getConfiguration("procode.mcp").get<boolean>(name, true) !== false;
}

/* The servers that are on. */
export function servers(ctx: vscode.ExtensionContext): Server[] {
    return allServers(ctx).filter((s) => mcpEnabled(s.name));
}

/* Lets the person tick the servers they want, and sets the switches: for the
 * workspace when a folder is open (servers are a per-project choice), else
 * for the user. */
async function chooseMcpServers(): Promise<void> {
    const picked = await vscode.window.showQuickPick(
        mcpNames().map((name) => ({ label: name, picked: mcpEnabled(name) })),
        { canPickMany: true, title: "procode: MCP servers to run", placeHolder: "Ticked servers run; the others are off" },
    );
    if (!picked) return;
    const on = new Set(picked.map((p) => p.label));
    const target = vscode.workspace.workspaceFolders?.length ? vscode.ConfigurationTarget.Workspace : vscode.ConfigurationTarget.Global;
    const config = vscode.workspace.getConfiguration("procode.mcp");
    for (const name of mcpNames()) {
        if (on.has(name) !== mcpEnabled(name)) await config.update(name, on.has(name), target);
    }
}

function allServers(ctx: vscode.ExtensionContext): Server[] {
    const script = (name: string) => path.join(ctx.extensionPath, "out", "mcp", `${name}.js`);
    const node = { ELECTRON_RUN_AS_NODE: "1" };
    return [
        {
            name: "coboard",
            label: "coboard: the board",
            command: process.execPath,
            args: [script("coboard")],
            env: { ...node, LAP_BIN: cliCommand("lap"), ...boardFolder() },
        },
        {
            name: "kb",
            label: "kb: the knowledge base",
            command: process.execPath,
            args: [script("kb")],
            env: { ...node, KB_BIN: cliCommand("kb") },
        },
        {
            name: "techdocs",
            label: "techdocs: pages agents publish",
            command: process.execPath,
            args: [script("techdocs")],
            env: { ...node },
        },
        // Only in a build made with --with-ask, once its server listens.
        // Claude Code drops an HTTP call silent for 5 minutes; ask's sends
        // progress while it waits, and the timeout (which also floors that
        // idle limit) is a second guard, above ask's own 25-minute wait.
        ...(askUrl ? [{ name: "ask", label: "ask: questions as a form", url: askUrl, timeout: 30 * 60_000 }] : []),
    ];
}

/* The settings the definitions are made from: the CLIs handed to the
 * servers, and the board folder handed to coboard's. */
export const MCP_SETTINGS = ["knowledge.cliPath", "coboard.lapPath", "coboard.boardFolder", "procode.mcp"] as const;

/* VS Code's own agent finds the servers without any configuration. Each runs
 * in the workspace folder, which is where it finds that workspace's .coboard/
 * and .kb/. When a setting they are made from or the folders change, VS Code
 * is told to ask for them again, so the change reaches the next server it
 * starts without a reload. */
function registerWithVsCode(ctx: vscode.ExtensionContext): void {
    if (typeof vscode.lm?.registerMcpServerDefinitionProvider !== "function") {
        return; // a VS Code older than the MCP API: the Claude Code command still works
    }
    const changed = new vscode.EventEmitter<void>();
    // ask's server comes up after activation: VS Code is told when it does.
    ASK?.onServing?.(() => changed.fire());
    ctx.subscriptions.push(
        changed,
        vscode.workspace.onDidChangeConfiguration((e) => {
            if (MCP_SETTINGS.some((k) => e.affectsConfiguration(k))) changed.fire();
        }),
        vscode.workspace.onDidChangeWorkspaceFolders(() => changed.fire()),
        vscode.lm.registerMcpServerDefinitionProvider("procode.mcp", {
            onDidChangeMcpServerDefinitions: changed.event,
            provideMcpServerDefinitions: () => {
                const folder = vscode.workspace.workspaceFolders?.find((f) => f.uri.scheme === "file")?.uri;
                /* The extension's own version: VS Code compares it to notice
                 * that a server's tools may have changed, so every release
                 * that ships new servers says so without a constant to bump. */
                const version = String(ctx.extension.packageJSON.version);
                return servers(ctx).map((s) => {
                    if ("url" in s) return new vscode.McpHttpServerDefinition(s.label, vscode.Uri.parse(s.url), {}, version);
                    const d = new vscode.McpStdioServerDefinition(s.label, s.command, s.args, s.env, version);
                    if (folder) {
                        d.cwd = folder;
                    }
                    return d;
                });
            },
            /* Just before VS Code starts a server: the CLI it runs is looked
             * for, and when it is missing the person is told as checkClis
             * tells them. kb's server is then not started (every tool needs
             * kb); coboard's still is (only its sessions need lap). */
            resolveMcpServerDefinition: (d: vscode.McpServerDefinition) => {
                const name = servers(ctx).find((s) => s.label === d.label)?.name;
                const need = name ? serverNeeds(name) : null;
                const cli = need ? CLIS.find((c) => c.name === need.cli) : undefined;
                const command = cli ? missingCli(cli) : null;
                if (!cli || command === null) return d;
                warnMissing(cli, command);
                return need!.required ? undefined : d;
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

async function setUpClaudeMcp(ctx: vscode.ExtensionContext): Promise<void> {
    await ASK?.whenServed?.(); // so ask's address is there to write
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
    const on = servers(ctx);
    const off = mcpNames().filter((n) => !mcpEnabled(n));
    const removed = writtenEntries(current, off);
    fs.writeFileSync(file, JSON.stringify(withServers(current, on.map(forClaude), off), null, 2) + "\n");
    const list = (names: string[]) => (names.length < 2 ? names.join("") : `${names.slice(0, -1).join(", ")} and ${names.at(-1)}`);
    const said = [
        ...(on.length ? [`${list(on.map((s) => s.name))} ${on.length === 1 ? "is" : "are"} in .mcp.json`] : []),
        ...(removed.length ? [`${list(removed)} (turned off) ${removed.length === 1 ? "was" : "were"} taken out`] : []),
    ];
    void vscode.window.showInformationMessage(
        `procode: ${said.join("; ") || "no MCP server is on, and .mcp.json has none of procode's"}. Restart Claude Code in this project (or check /mcp) to pick this up.`,
    );
}

/* After the switches change: when .mcp.json holds procode's entries, offer
 * to bring it in line, so Claude Code follows what VS Code's agent does. */
function offerMcpUpdate(ctx: vscode.ExtensionContext): void {
    const file = mcpJsonPath();
    const current = file ? readMcpJson(file) : null;
    if (!current || writtenEntries(current, mcpNames()).length === 0) return;
    void vscode.window
        .showInformationMessage("procode: the MCP servers that are on changed. Update .mcp.json for Claude Code too?", "Update .mcp.json")
        .then((choice) => {
            if (choice === "Update .mcp.json") void setUpClaudeMcp(ctx);
        });
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
                void setUpClaudeMcp(ctx);
            }
        });
}

/* ------------------------------------------------------------ Claude skills
 *
 * The skills that teach Claude Code to use the tools, shipped with the
 * extension (skills/, the repository's .claude/skills as of this build) and
 * added to a project's .claude/skills on request — never to ~/.claude, and
 * never over a folder of the person's own without asking. The hash of each
 * folder procode writes is kept, so a later version can replace a copy only
 * while it is still exactly as procode wrote it. */

const INSTALLED_SKILLS = "procode.installedSkills";

function projectSkillsDir(): string | null {
    const folder = vscode.workspace.workspaceFolders?.find((f) => f.uri.scheme === "file");
    return folder ? path.join(folder.uri.fsPath, ".claude", "skills") : null;
}

function shipped(ctx: vscode.ExtensionContext): ShippedSkill[] {
    return shippedSkills(path.join(ctx.extensionPath, "skills"));
}

function installAndRecord(ctx: vscode.ExtensionContext, skill: ShippedSkill, dir: string): void {
    const hash = install(skill, dir);
    const record = { ...(ctx.workspaceState.get<Record<string, string>>(INSTALLED_SKILLS) ?? {}), [skill.name]: hash };
    void ctx.workspaceState.update(INSTALLED_SKILLS, record);
}

async function addClaudeSkills(ctx: vscode.ExtensionContext): Promise<void> {
    const dir = projectSkillsDir();
    if (!dir) {
        void vscode.window.showWarningMessage("procode: open a folder first; Claude Code's skills are added per project.");
        return;
    }
    const picked = await vscode.window.showQuickPick(
        shipped(ctx).map((skill) => ({
            label: skill.name,
            description: `version ${skill.version}`,
            detail: skill.name === "tickets" ? "This repository's own workflow (board, lap and git): adapt it before relying on it." : undefined,
            picked: skill.name !== "tickets",
            skill,
        })),
        { canPickMany: true, title: "procode: Add Skills for Claude Code", placeHolder: "The skills to add to this project's .claude/skills" },
    );
    if (!picked || picked.length === 0) return;
    const added: string[] = [];
    const updated: string[] = [];
    const kept: string[] = [];
    for (const { skill } of picked) {
        const state = stateOf(skill, dir);
        if (state === "identical") continue;
        if (state === "different") {
            let choice: string | undefined;
            for (;;) {
                choice = await vscode.window.showWarningMessage(
                    `.claude/skills/${skill.name} is not procode's ${skill.name} skill (version ${skill.version}).`,
                    { modal: true, detail: "Update replaces that folder with procode's; Keep mine leaves it as it is." },
                    "Update",
                    "Keep mine",
                    "Show differences",
                );
                if (choice !== "Show differences") break;
                await vscode.commands.executeCommand(
                    "vscode.diff",
                    vscode.Uri.file(path.join(dir, skill.name, "SKILL.md")),
                    vscode.Uri.file(path.join(skill.dir, "SKILL.md")),
                    `${skill.name}: yours ↔ procode's`,
                );
            }
            if (choice !== "Update") {
                kept.push(skill.name);
                continue;
            }
            installAndRecord(ctx, skill, dir);
            updated.push(skill.name);
            continue;
        }
        installAndRecord(ctx, skill, dir);
        added.push(skill.name);
    }
    const said = [
        added.length ? `added ${added.join(", ")}` : "",
        updated.length ? `updated ${updated.join(", ")}` : "",
        kept.length ? `kept yours of ${kept.join(", ")}` : "",
    ].filter((x) => x);
    void vscode.window.showInformationMessage(
        said.length
            ? `procode: ${said.join("; ")} in .claude/skills. Restart Claude Code in this project (or run /skills) to pick them up.`
            : "procode: this project's .claude/skills already has procode's skills as shipped.",
    );
}

/* On opening a project: says when its copies of procode's skills are behind
 * the shipped ones, offering to update those still as procode wrote them. */
export function checkSkills(ctx: vscode.ExtensionContext): void {
    const dir = projectSkillsDir();
    if (!dir) return;
    const { updatable, edited } = behind(shipped(ctx), dir, ctx.workspaceState.get<Record<string, string>>(INSTALLED_SKILLS) ?? {});
    if (updatable.length > 0) {
        void vscode.window
            .showInformationMessage(`procode: newer procode skills are available: ${updatable.map((s) => s.name).join(", ")}.`, "Update")
            .then((choice) => {
                if (choice === "Update") for (const s of updatable) installAndRecord(ctx, s, dir);
            });
    }
    if (edited.length > 0) {
        void vscode.window.showInformationMessage(
            `procode: newer versions of ${edited.map((s) => s.name).join(", ")} are shipped; this project's copies were changed, so they are left as they are. procode: Add Skills for Claude Code compares them.`,
        );
    }
}

function isFile(p: string): boolean {
    try {
        return fs.statSync(p).isFile();
    } catch {
        return false;
    }
}

/* The command a CLI's setting names, when it cannot be found; null when it
 * can. */
function missingCli(cli: Cli): string | null {
    const command = cliCommand(cli.name);
    return resolveCli(command, process.env["PATH"] ?? "", isFile) ? null : command;
}

/* What cannot run and how to fix it, with the setting one click away. */
function warnMissing(cli: Cli, command: string): void {
    void vscode.window.showWarningMessage(missingMessage(cli, command), "Open Setting").then((choice) => {
        if (choice === "Open Setting") {
            void vscode.commands.executeCommand("workbench.action.openSettings", cli.settingId);
        }
    });
}

/* Says once per window, for each CLI its setting cannot reach, what cannot
 * run and how to fix it. The parts report their own failures when used; this
 * is the one place that says it up front. */
function checkClis(): void {
    for (const cli of CLIS) {
        const command = missingCli(cli);
        if (command !== null) warnMissing(cli, command);
    }
}

/* Places each CLI in ~/.procode/bin (procodebin.ts), and puts that folder
 * first on the extension host's PATH, where the parts look for a bare "lap"
 * or "kb". PATH is searched without the folder itself, so a link never
 * points at its own copy. */
function installClis(ctx: vscode.ExtensionContext): void {
    const dir = procodeBinDir();
    const pathVar = (process.env["PATH"] ?? "")
        .split(path.delimiter)
        .filter((d) => path.resolve(d) !== path.resolve(dir))
        .join(path.delimiter);
    let any = false;
    for (const cli of CLIS) {
        const setting = setByUser(cli.name);
        const bundled = path.join(ctx.extensionPath, "bin", exeName(cli.name));
        const source = sourceOf(
            setting ? resolveCli(setting, pathVar, isFile) : null,
            isFile(bundled) ? bundled : null,
            resolveCli(cli.name, pathVar, isFile),
        );
        const to = path.join(dir, exeName(cli.name));
        if (source) {
            try {
                place(source, to);
            } catch (e) {
                console.error(`procode: could not place ${cli.name} in ${dir}: ${(e as Error).message}`);
            }
        }
        any ||= isFile(to);
    }
    if (any) process.env["PATH"] = [dir, pathVar].join(path.delimiter);
}

export function activate(ctx: vscode.ExtensionContext): void {
    installClis(ctx);
    ASK?.onServing?.((url) => {
        askUrl = url;
    });
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
        vscode.commands.registerCommand("procode.chooseMcpServers", () => chooseMcpServers()),
        vscode.workspace.onDidChangeConfiguration((e) => {
            if (e.affectsConfiguration("procode.mcp")) offerMcpUpdate(ctx);
        }),
        vscode.commands.registerCommand("procode.addClaudeSkills", () => addClaudeSkills(ctx)),
    );
    // After ask's server listens, so its entry is checked too.
    void Promise.resolve(ASK?.whenServed?.()).then(() => checkClaudeMcp(ctx));
    checkSkills(ctx);
    checkClis();
}

export function deactivate(): void {
    for (const [, part] of PARTS) {
        part.deactivate?.();
    }
}
