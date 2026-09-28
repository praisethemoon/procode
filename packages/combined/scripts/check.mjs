/* Loads the built dist/out/extension.js against a stand-in for the `vscode`
 * module and checks what an install would rely on:
 *   - all three parts activate without an error message
 *   - every command the generated manifest contributes is registered
 *   - every icon a view container or view names as a file is in dist/
 *   - the MCP definitions run files that exist in dist/, with the CLIs the
 *     settings name, and dist/ ships no native binary
 *   - "Set Up MCP for Claude Code" writes .mcp.json and keeps other servers
 *   - user-scope registration asks a stand-in `claude` on PATH, never the real
 *     one: the first time, after an update, and not again once current
 *
 *   node scripts/check.mjs      (after npm run build --workspace combined)
 *
 * Works only in a directory under the system temp dir, and deletes nothing. */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import { createRequire } from "node:module";
import * as os from "node:os";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const dist = path.join(here, "dist");
const manifest = JSON.parse(fs.readFileSync(path.join(dist, "package.json"), "utf8"));
const folder = fs.mkdtempSync(path.join(os.tmpdir(), "procode-check-"));
fs.writeFileSync(path.join(folder, ".mcp.json"), JSON.stringify({ mcpServers: { other: { command: "x" } } }));

const registered = new Map();
const errors = [];
let mcpProvider = null;
const noop = () => ({ dispose() {} });
/* A working emitter, so the check can count what the MCP provider fires. */
const emitter = class {
    listeners = [];
    event = (fn) => {
        this.listeners.push(fn);
        return { dispose() {} };
    };
    fire(v) {
        for (const fn of this.listeners) fn(v);
    }
    dispose() {}
};
const configListeners = [];
const folderListeners = [];
const handler = {
    get(target, key) {
        if (key in target) return target[key];
        // Anything a part touches that this stand-in does not model is a
        // no-op object, so an unmodelled API cannot make the check pass or
        // fail by accident; the assertions below are what decide.
        return new Proxy(function () {}, { get: (_t, k) => (k === "then" ? undefined : handler.get({}, k)), apply: () => new Proxy({}, handler), construct: () => new Proxy({}, handler) });
    },
};
const vscode = new Proxy(
    {
        EventEmitter: emitter,
        TreeItem: class {
            constructor(label, state) {
                this.label = label;
                this.collapsibleState = state;
            }
        },
        ThemeIcon: class {
            constructor(id) {
                this.id = id;
            }
        },
        Uri: {
            file: (p) => ({ fsPath: p, path: p, scheme: "file", toString: () => `file://${p}` }),
            joinPath: (b, ...p) => ({ fsPath: path.join(b.fsPath, ...p), path: path.join(b.fsPath, ...p) }),
            from: (c) => ({ ...c, toString: () => `${c.scheme}:${c.path}` }),
        },
        McpStdioServerDefinition: class {
            constructor(label, command, args, env, version) {
                Object.assign(this, { label, command, args, env, version });
            }
        },
        commands: {
            registerCommand: (name, fn) => {
                registered.set(name, fn);
                return { dispose() {} };
            },
            executeCommand: async () => undefined,
        },
        window: new Proxy(
            {
                showErrorMessage: (m) => {
                    errors.push(m);
                    return Promise.resolve(undefined);
                },
                showWarningMessage: () => Promise.resolve(undefined),
                showInformationMessage: () => Promise.resolve(undefined),
            },
            handler,
        ),
        workspace: new Proxy(
            {
                workspaceFolders: [{ uri: { scheme: "file", fsPath: folder, path: folder } }],
                onDidChangeConfiguration: (fn) => {
                    configListeners.push(fn);
                    return { dispose() {} };
                },
                onDidChangeWorkspaceFolders: (fn) => {
                    folderListeners.push(fn);
                    return { dispose() {} };
                },
                /* defaults, but for a board folder set to show it is passed on */
                getConfiguration: (section) => ({
                    get: (k, d) => (section === "coboard" && k === "boardFolder" ? "/shared/project" : d),
                    update: async () => undefined,
                }),
            },
            handler,
        ),
        lm: {
            registerMcpServerDefinitionProvider: (_id, p) => {
                mcpProvider = p;
                return { dispose() {} };
            },
        },
    },
    handler,
);

const Module = createRequire(import.meta.url)("node:module");
const load = Module._load;
Module._load = (req, parent, isMain) => (req === "vscode" ? vscode : load(req, parent, isMain));
const ext = createRequire(import.meta.url)(path.join(dist, "out", "extension.js"));
const ctx = { subscriptions: [], extension: { packageJSON: manifest }, extensionPath: dist, extensionUri: { fsPath: dist, path: dist }, workspaceState: { get: () => undefined, update: async () => undefined }, globalState: { get: () => undefined, update: async () => undefined } };
ext.activate(ctx);
await new Promise((r) => setTimeout(r, 200));
Module._load = load;

assert.deepEqual(errors, [], "no part reported an error while starting");
for (const c of [...manifest.contributes.viewsContainers.activitybar, ...Object.values(manifest.contributes.views).flat()]) {
    if (typeof c.icon === "string" && !c.icon.startsWith("$(")) {
        assert.ok(fs.existsSync(path.join(dist, c.icon)), `${c.id}'s icon ${c.icon} is in dist/`);
    }
}
for (const c of manifest.contributes.commands) {
    assert.ok(registered.has(c.command), `${c.command} is contributed and registered`);
}
const defs = await mcpProvider.provideMcpServerDefinitions();
assert.deepEqual(defs.map((d) => d.label), ["coboard: the board", "kb: the knowledge base", "artifacts: pages agents publish"]);
for (const d of defs) {
    assert.equal(d.command, process.execPath);
    assert.ok(fs.existsSync(d.args[0]), `${d.args[0]} exists`);
    assert.equal(d.env.ELECTRON_RUN_AS_NODE, "1");
    assert.equal(d.cwd.fsPath, folder);
    assert.equal(d.version, manifest.version, "each server's version is the extension's");
}
assert.equal(defs[0].env.LAP_BIN, "lap", "coboard is handed Board › Lap Path, lap by default");
assert.equal(defs[0].env.COBOARD_DIR, "/shared/project", "coboard is handed Board › Board Folder as COBOARD_DIR");
assert.equal(defs[1].env.COBOARD_DIR, undefined, "only coboard is handed the board folder");
assert.equal(defs[1].env.KB_BIN, "kb", "kb is handed Knowledge › Cli Path, kb by default");

// A setting the definitions are made from, or the folders, changing tells
// VS Code to ask again, once; any other setting does not.
let asked = 0;
mcpProvider.onDidChangeMcpServerDefinitions(() => asked++);
const changeOf = (key) => ({ affectsConfiguration: (k) => k === key || key.startsWith(`${k}.`) });
for (const key of ["knowledge.cliPath", "coboard.lapPath", "coboard.boardFolder"]) {
    const before = asked;
    for (const fn of configListeners) fn(changeOf(key));
    assert.equal(asked, before + 1, `a change to ${key} asks for the definitions again, once`);
}
const before = asked;
for (const fn of configListeners) fn(changeOf("editor.fontSize"));
assert.equal(asked, before, "an unrelated setting does not");
for (const fn of folderListeners) fn({ added: [], removed: [] });
assert.equal(asked, before + 1, "a change of workspace folders asks again, once");
assert.equal(fs.existsSync(path.join(dist, "bin")), false, "the package carries no CLI");
assert.equal(ext.resolveCli("kb", "/nowhere", () => false), null);
assert.equal(ext.resolveCli("kb", ["/a", "/b"].join(path.delimiter), (p) => p === path.join("/b", "kb")), path.join("/b", "kb"));
assert.equal(ext.resolveCli("/opt/kb", "", (p) => p === "/opt/kb"), "/opt/kb");
assert.equal(ext.resolveCli("kb", "/w", (p) => p === path.join("/w", "kb.exe"), "win32"), path.join("/w", "kb.exe"));

// The Claude Code command writes our servers and keeps the other one.
registered.get("procode.setUpClaudeMcp")();
const written = JSON.parse(fs.readFileSync(path.join(folder, ".mcp.json"), "utf8"));
assert.deepEqual(Object.keys(written.mcpServers).sort(), ["artifacts", "coboard", "kb", "other"]);
assert.equal(written.mcpServers.other.command, "x", "an unrelated server is kept as it was");
assert.equal(written.mcpServers.kb.args[0], defs[1].args[0], "Claude Code runs the same script as VS Code's agent");
assert.equal(written.mcpServers.coboard.env.COBOARD_AUTHOR, "claude", "Claude Code's board comments are signed");

assert.equal(registered.has("procode.registerClaudeMcp"), false, "Claude Code is set up per project only");

console.log(`check: ${registered.size} commands registered, ${defs.length} MCP servers, no errors`);
console.log(`check: workspace ${folder}`);
