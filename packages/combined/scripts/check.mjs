/* Loads the built dist/out/extension.js against a stand-in for the `vscode`
 * module and checks what an install would rely on:
 *   - all three parts activate without an error message
 *   - every command the generated manifest contributes is registered
 *   - the MCP definitions run files that exist in dist/, with the bundled CLIs
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
const emitter = class {
    event = noop;
    fire() {}
    dispose() {}
};
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
                getConfiguration: () => ({ get: (_k, d) => d, update: async () => undefined }),
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
const ctx = { subscriptions: [], extensionPath: dist, extensionUri: { fsPath: dist, path: dist }, workspaceState: { get: () => undefined, update: async () => undefined }, globalState: { get: () => undefined, update: async () => undefined } };
ext.activate(ctx);
await new Promise((r) => setTimeout(r, 200));
Module._load = load;

assert.deepEqual(errors, [], "no part reported an error while starting");
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
    for (const [k, v] of Object.entries(d.env)) if (k.endsWith("_BIN")) assert.ok(fs.existsSync(v), `${k} ${v} exists`);
}

// The Claude Code command writes our servers and keeps the other one.
registered.get("procode.setUpClaudeMcp")();
const written = JSON.parse(fs.readFileSync(path.join(folder, ".mcp.json"), "utf8"));
assert.deepEqual(Object.keys(written.mcpServers).sort(), ["artifacts", "coboard", "kb", "other"]);
assert.equal(written.mcpServers.other.command, "x", "an unrelated server is kept as it was");
assert.equal(written.mcpServers.kb.args[0], defs[1].args[0], "Claude Code runs the same script as VS Code's agent");
assert.equal(written.mcpServers.coboard.env.COBOARD_AUTHOR, "claude", "Claude Code's board comments are signed");

// User scope, against a stand-in `claude` that records its arguments.
const fakeBin = fs.mkdtempSync(path.join(os.tmpdir(), "procode-claude-"));
const calls = path.join(fakeBin, "calls.jsonl");
fs.writeFileSync(
    path.join(fakeBin, "claude"),
    `#!/bin/sh\nexec "${process.execPath}" -e 'require("fs").appendFileSync(process.argv[1], JSON.stringify(process.argv.slice(2)) + "\\n")' "${calls}" "$@"\n`,
    { mode: 0o755 },
);
process.env.PATH = fakeBin + path.delimiter + process.env.PATH;
const state = new Map([["procode.claude.userScope", "a signature from an older install"]]);
const ctx2 = { ...ctx, globalState: { get: (k) => state.get(k), update: async (k, v) => void state.set(k, v) } };
const asked = () => (fs.existsSync(calls) ? fs.readFileSync(calls, "utf8").trim().split("\n").map((l) => JSON.parse(l)) : []);

await ext.refreshClaudeUserScope(ctx2);
const first = asked();
assert.deepEqual(first.map((a) => a.slice(0, 5).join(" ")), [
    "mcp remove --scope user coboard",
    "mcp add-json --scope user coboard",
    "mcp remove --scope user kb",
    "mcp add-json --scope user kb",
    "mcp remove --scope user artifacts",
    "mcp add-json --scope user artifacts",
]);
const coboardEntry = JSON.parse(first[1][5]);
assert.equal(coboardEntry.type, "stdio");
assert.equal(coboardEntry.args[0], defs[0].args[0], "user scope runs the installed script");
assert.equal(coboardEntry.env.COBOARD_AUTHOR, "claude");
assert.ok(fs.existsSync(JSON.parse(first[3][5]).env.KB_BIN), "user scope hands kb the bundled CLI");

await ext.refreshClaudeUserScope(ctx2);
assert.equal(asked().length, 6, "a current registration is left alone");
state.set("procode.claude.userScope", "declined");
await ext.refreshClaudeUserScope(ctx2);
assert.equal(asked().length, 6, "a declined offer is not made again");
assert.equal(
    ext.shellLine(["mcp", "add-json", "kb", `{"a":"it's"}`]),
    `claude mcp add-json kb '{"a":"it'\\''s"}'`,
    "the clipboard line survives a single quote",
);

console.log(`check: ${registered.size} commands registered, ${defs.length} MCP servers, no errors`);
console.log(`check: workspace ${folder}, stand-in claude ${fakeBin}`);
