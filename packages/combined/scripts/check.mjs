/* Loads the built dist/out/extension.js against a stand-in for the `vscode`
 * module and checks what an install would rely on:
 *   - all three parts activate without an error message
 *   - every command the generated manifest contributes is registered
 *   - every icon a view container or view names as a file is in dist/
 *   - the MCP definitions run files that exist in dist/, with the CLIs the
 *     settings name; dist/ ships no native binary, or with --target exactly
 *     that platform's lap and kb, executable
 *   - "Set Up MCP for Claude Code" writes .mcp.json and keeps other servers
 *   - user-scope registration asks a stand-in `claude` on PATH, never the real
 *     one: the first time, after an update, and not again once current
 *
 *   node scripts/check.mjs      (after npm run build --workspace combined)
 *   node scripts/check.mjs --target <vsce target>   (after a platform build)
 *
 * A build made with --with-ask is checked with ask in it: its tab's command
 * registered, and its server listening inside the extension, defined for
 * VS Code over HTTP, answering, and written into .mcp.json as an http entry;
 * any other build must carry nothing of it.
 *
 * Works only in a directory under the system temp dir, and deletes nothing. */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import { createRequire } from "node:module";
import * as os from "node:os";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import { CLIS, exe } from "./binaries.mjs";

const here = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const dist = path.join(here, "dist");
const manifest = JSON.parse(fs.readFileSync(path.join(dist, "package.json"), "utf8"));
const folder = fs.mkdtempSync(path.join(os.tmpdir(), "procode-check-"));
const withAsk = fs.existsSync(path.join(dist, "out", "parts", "ask.js"));
fs.writeFileSync(path.join(folder, ".mcp.json"), JSON.stringify({ mcpServers: { other: { command: "x" } } }));

/* ~/.procode/bin, in the check's own folder so the real one is never
 * touched, and a lap and kb "on PATH" for procode to link into it. */
const procodeBin = path.join(folder, "procode-bin");
process.env.PROCODE_BIN_DIR = procodeBin;
const onPath = path.join(folder, "on-path");
fs.mkdirSync(onPath);
const osExe = (cli) => (process.platform === "win32" ? `${cli}.exe` : cli);
for (const cli of ["lap", "kb"]) {
    fs.writeFileSync(path.join(onPath, osExe(cli)), `#!/bin/sh\necho ${cli}\n`, { mode: 0o755 });
}
process.env.PATH = [onPath, process.env.PATH].join(path.delimiter);

const registered = new Map();
const errors = [];
const warnings = [];
const infos = [];
const executed = [];
/* The answers a scenario gives: to modal questions, in order, and to the
 * pick list (by default, what it offers ticked). */
const answers = [];
let pick = (items) => items.filter((i) => i.picked);
const memento = new Map();
/* Settings a scenario sets, "section.key" to value; the rest are defaults. */
const settings = { "coboard.boardFolder": "/shared/project" };
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
            parse: (u) => ({ toString: () => u }),
        },
        McpStdioServerDefinition: class {
            constructor(label, command, args, env, version) {
                Object.assign(this, { label, command, args, env, version });
            }
        },
        McpHttpServerDefinition: class {
            constructor(label, uri, headers, version) {
                Object.assign(this, { label, uri, headers, version });
            }
        },
        commands: {
            registerCommand: (name, fn) => {
                registered.set(name, fn);
                return { dispose() {} };
            },
            executeCommand: async (...a) => {
                executed.push(a);
                return undefined;
            },
        },
        window: new Proxy(
            {
                showErrorMessage: (m) => {
                    errors.push(m);
                    return Promise.resolve(undefined);
                },
                showWarningMessage: (m) => {
                    warnings.push(m);
                    return Promise.resolve(answers.shift());
                },
                showInformationMessage: (m) => {
                    infos.push(m);
                    return Promise.resolve(undefined);
                },
                showQuickPick: (items) => Promise.resolve(pick(items)),
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
                /* defaults, but for what `settings` sets (a board folder, to
                 * show it is passed on, and the CLI paths the scenarios move) */
                getConfiguration: (section) => ({
                    get: (k, d) => (`${section}.${k}` in settings ? settings[`${section}.${k}`] : d),
                    inspect: (k) => ({ globalValue: settings[`${section}.${k}`] }),
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
const ctx = { subscriptions: [], extension: { packageJSON: manifest }, extensionPath: dist, extensionUri: { fsPath: dist, path: dist }, workspaceState: { get: (k) => memento.get(k), update: async (k, v) => void memento.set(k, v) }, globalState: { get: () => undefined, update: async () => undefined } };
ext.activate(ctx);
await new Promise((r) => setTimeout(r, 200));
Module._load = load;

assert.deepEqual(errors, [], "no part reported an error while starting");

// Activation placed each CLI in ~/.procode/bin: a copy of the package's own
// when it carries one (a platform build), else a link to the one on PATH
// (a copy on Windows); and it put that folder first on PATH for the parts.
for (const cli of ["lap", "kb"]) {
    const placed = path.join(procodeBin, osExe(cli));
    const own = path.join(dist, "bin", osExe(cli));
    if (fs.existsSync(own)) {
        assert.deepEqual(fs.readFileSync(placed), fs.readFileSync(own), `the package's ${cli} is copied into ~/.procode/bin`);
    } else if (process.platform === "win32") {
        assert.deepEqual(fs.readFileSync(placed), fs.readFileSync(path.join(onPath, osExe(cli))), `${cli} is copied into ~/.procode/bin`);
    } else {
        assert.equal(fs.readlinkSync(placed), path.join(onPath, cli), `${cli} in ~/.procode/bin links to the one on PATH`);
    }
}
assert.equal(process.env.PATH.split(path.delimiter)[0], procodeBin, "~/.procode/bin is first on PATH");
for (const c of [...manifest.contributes.viewsContainers.activitybar, ...Object.values(manifest.contributes.views).flat()]) {
    if (typeof c.icon === "string" && !c.icon.startsWith("$(")) {
        assert.ok(fs.existsSync(path.join(dist, c.icon)), `${c.id}'s icon ${c.icon} is in dist/`);
    }
}
for (const c of manifest.contributes.commands) {
    assert.ok(registered.has(c.command), `${c.command} is contributed and registered`);
}
const defs = await mcpProvider.provideMcpServerDefinitions();
assert.deepEqual(defs.map((d) => d.label), [
    "coboard: the board",
    "kb: the knowledge base",
    "techdocs: pages agents publish",
    ...(withAsk ? ["ask: questions as a form"] : []),
]);
if (withAsk) {
    assert.ok(registered.has("ask.openPending"), "ask's tab started with the other parts");
    assert.ok(fs.existsSync(path.join(dist, "out", "media", "ask.js")), "ask's webview is in out/media");
    // ask's server runs inside the extension: VS Code is given its address,
    // and it answers there.
    const askDef = defs.pop();
    const url = askDef.uri.toString();
    assert.match(url, /^http:\/\/127\.0\.0\.1:\d+\/procode\/ask\/mcp$/);
    assert.equal(askDef.version, manifest.version);
    const listed = await fetch(url, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ jsonrpc: "2.0", id: 1, method: "tools/list" }) });
    assert.deepEqual((await listed.json()).result.tools.map((t) => t.name), ["ask", "ask_wait"], "ask's server answers at that address");
} else {
    assert.equal(fs.existsSync(path.join(dist, "out", "parts")), false, "a build without --with-ask carries no optional part");
    assert.doesNotMatch(JSON.stringify(manifest), /"ask\./, "nor anything of ask in its manifest");
}
for (const d of defs) {
    assert.equal(d.command, process.execPath);
    assert.ok(fs.existsSync(d.args[0]), `${d.args[0]} exists`);
    assert.equal(d.env.ELECTRON_RUN_AS_NODE, "1");
    assert.equal(d.cwd.fsPath, folder);
    assert.equal(d.version, manifest.version, "each server's version is the extension's");
}
assert.equal(defs[0].env.LAP_BIN, path.join(procodeBin, osExe("lap")), "coboard is handed ~/.procode/bin's lap when no setting names one");
assert.equal(defs[0].env.COBOARD_DIR, "/shared/project", "coboard is handed Board › Board Folder as COBOARD_DIR");
assert.equal(defs[1].env.COBOARD_DIR, undefined, "only coboard is handed the board folder");
assert.equal(defs[1].env.KB_BIN, path.join(procodeBin, osExe("kb")), "kb is handed ~/.procode/bin's kb when no setting names one");

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
const target = process.argv.includes("--target") ? process.argv[process.argv.indexOf("--target") + 1] : undefined;
if (target === undefined) {
    assert.equal(fs.existsSync(path.join(dist, "bin")), false, "the package carries no CLI");
} else {
    const want = CLIS.map((cli) => exe(cli, target)).sort();
    assert.deepEqual(fs.readdirSync(path.join(dist, "bin")).sort(), want, `the ${target} package carries exactly ${want.join(" and ")}`);
    if (process.platform !== "win32") {
        for (const f of want) assert.ok(fs.statSync(path.join(dist, "bin", f)).mode & 0o111, `bin/${f} is executable`);
    }
}

// Just before a server starts, its CLI is looked for. With both found, every
// server starts as defined and nothing is said.
const fake = (name) => {
    const f = path.join(folder, "clis", name);
    fs.mkdirSync(path.dirname(f), { recursive: true });
    fs.writeFileSync(f, "#!/bin/sh\n");
    return f;
};
const [coboardDef, kbDef, techdocsDef] = defs;
const resolve = (d) => mcpProvider.resolveMcpServerDefinition(d);
settings["knowledge.cliPath"] = fake("kb");
settings["coboard.lapPath"] = fake("lap");
warnings.length = 0;
for (const d of defs) assert.equal(resolve(d), d, `${d.label} starts when its CLI is there`);
assert.deepEqual(warnings, [], "nothing is said when the CLIs are there");
// kb missing: its server is not started, and the person is told why.
settings["knowledge.cliPath"] = "/nowhere/kb";
assert.equal(resolve(kbDef), undefined, "kb's server does not start without kb");
assert.equal(warnings.length, 1);
assert.match(warnings[0], /"\/nowhere\/kb" was not found/);
assert.equal(resolve(coboardDef), coboardDef, "coboard's does not need kb");
assert.equal(resolve(techdocsDef), techdocsDef, "techdocs's needs no CLI");
assert.equal(warnings.length, 1, "only the missing CLI is reported");
// lap missing: coboard's server still starts, with a warning.
settings["knowledge.cliPath"] = fake("kb");
settings["coboard.lapPath"] = "/nowhere/lap";
warnings.length = 0;
assert.equal(resolve(coboardDef), coboardDef, "coboard's server starts without lap");
assert.equal(warnings.length, 1);
assert.match(warnings[0], /"\/nowhere\/lap" was not found/);
assert.equal(resolve(kbDef), kbDef, "kb's does not need lap");
delete settings["knowledge.cliPath"];
delete settings["coboard.lapPath"];
assert.equal(ext.resolveCli("kb", "/nowhere", () => false), null);
assert.equal(ext.resolveCli("kb", ["/a", "/b"].join(path.delimiter), (p) => p === path.join("/b", "kb")), path.join("/b", "kb"));
assert.equal(ext.resolveCli("/opt/kb", "", (p) => p === "/opt/kb"), "/opt/kb");
assert.equal(ext.resolveCli("kb", "/w", (p) => p === path.join("/w", "kb.exe"), "win32"), path.join("/w", "kb.exe"));

// Where ~/.procode/bin's copy comes from: the user's setting, else the
// package's own, else PATH; and the command a CLI is run with.
assert.deepEqual(ext.sourceOf("/mine/lap", "/pkg/lap", "/usr/bin/lap"), { kind: "link", file: "/mine/lap" });
assert.deepEqual(ext.sourceOf(null, "/pkg/lap", "/usr/bin/lap"), { kind: "copy", file: "/pkg/lap" });
assert.deepEqual(ext.sourceOf(null, null, "/usr/bin/lap"), { kind: "link", file: "/usr/bin/lap" });
assert.equal(ext.sourceOf(null, null, null), null);
assert.equal(ext.commandFor("lap", "/mine/lap", () => true), "/mine/lap", "a setting the user set wins");
assert.equal(ext.commandFor("lap", undefined, () => true), path.join(procodeBin, osExe("lap")), "then ~/.procode/bin");
assert.equal(ext.commandFor("lap", undefined, () => false), "lap", "then PATH");
// A copy is written once, again only when the bytes change, and a file that
// cannot be replaced (a running .exe on Windows) is left for next time.
const placing = path.join(folder, "placing");
fs.mkdirSync(placing);
const pkgLap = path.join(placing, "pkg-lap");
const placedLap = path.join(placing, "bin", "lap");
fs.writeFileSync(pkgLap, "v1");
assert.equal(ext.place({ kind: "copy", file: pkgLap }, placedLap), "placed");
assert.equal(fs.readFileSync(placedLap, "utf8"), "v1");
if (process.platform !== "win32") assert.ok(fs.statSync(placedLap).mode & 0o111, "a placed copy is executable");
assert.equal(ext.place({ kind: "copy", file: pkgLap }, placedLap), "current", "the same bytes are not written again");
fs.writeFileSync(pkgLap, "v2");
const locked = () => {
    throw Object.assign(new Error("busy"), { code: "EBUSY" });
};
assert.equal(ext.place({ kind: "copy", file: pkgLap }, placedLap, process.platform, locked), "busy");
assert.equal(fs.readFileSync(placedLap, "utf8"), "v1", "a file that cannot be replaced keeps its old bytes");
assert.deepEqual(fs.readdirSync(path.dirname(placedLap)), ["lap"], "and no temporary file is left beside it");
assert.equal(ext.place({ kind: "copy", file: pkgLap }, placedLap), "placed", "the next try replaces it");
assert.equal(fs.readFileSync(placedLap, "utf8"), "v2");
assert.equal(ext.place({ kind: "link", file: pkgLap }, placedLap, "win32"), "current", "Windows copies instead of linking");

// The Claude Code command writes our servers and keeps the other one; the
// artifacts entry an earlier procode wrote, renamed techdocs, goes.
const earlier = JSON.parse(fs.readFileSync(path.join(folder, ".mcp.json"), "utf8"));
earlier.mcpServers.artifacts = { command: "x", args: ["/old/procode/out/mcp/artifacts.js"] };
fs.writeFileSync(path.join(folder, ".mcp.json"), JSON.stringify(earlier));
await registered.get("procode.setUpClaudeMcp")();
const written = JSON.parse(fs.readFileSync(path.join(folder, ".mcp.json"), "utf8"));
assert.deepEqual(Object.keys(written.mcpServers).sort(), [...(withAsk ? ["ask"] : []), "coboard", "kb", "other", "techdocs"]);
if (withAsk) {
    assert.deepEqual(Object.keys(written.mcpServers.ask), ["type", "url", "timeout"], "ask is an http entry: Claude Code connects to VS Code");
    assert.equal(written.mcpServers.ask.timeout, 1_800_000);
}
assert.equal(written.mcpServers.other.command, "x", "an unrelated server is kept as it was");
assert.equal(written.mcpServers.kb.args[0], defs[1].args[0], "Claude Code runs the same script as VS Code's agent");
assert.equal(written.mcpServers.coboard.env.COBOARD_AUTHOR, "claude", "Claude Code's board comments are signed");

// A retired entry procode wrote is reported for the Update notice; one of the
// same name set up by hand is neither reported nor removed.
const ours = { command: "node", args: ["/old/procode/out/mcp/artifacts.js"] };
const theirs = { command: "my-artifacts", args: [] };
assert.deepEqual(ext.outdated({ mcpServers: { artifacts: ours } }, []), ["artifacts"]);
assert.deepEqual(ext.outdated({ mcpServers: { artifacts: theirs } }, []), []);
assert.deepEqual(ext.withServers({ mcpServers: { artifacts: theirs } }, []).mcpServers, { artifacts: theirs });

assert.equal(registered.has("procode.registerClaudeMcp"), false, "Claude Code is set up per project only");

// ------------------------------------------------------------ Claude skills
// The package carries the skills whole, and the command adds them to the
// project's .claude/skills only.
assert.ok(registered.has("procode.addClaudeSkills"), "Add Skills for Claude Code is registered");
for (const f of ["lap/SKILL.md", "lap/references/branches.md", "tickets/SKILL.md", "techdocs/SKILL.md"]) {
    assert.ok(fs.existsSync(path.join(dist, "skills", f)), `skills/${f} is in the package`);
}
const skills = path.join(folder, ".claude", "skills");
const tree = (d, rel = "") =>
    fs.existsSync(path.join(d, rel))
        ? fs.readdirSync(path.join(d, rel), { withFileTypes: true }).flatMap((e) =>
              e.isDirectory() ? tree(d, path.join(rel, e.name)) : [path.join(rel, e.name)],
          )
        : [];
const read = (f) => fs.readFileSync(f, "utf8");
const outsideBefore = tree(folder).filter((f) => !f.startsWith(path.join(".claude", "skills")) && !f.startsWith(".claude")).map((f) => `${f}:${read(path.join(folder, f))}`);
const addSkills = () => registered.get("procode.addClaudeSkills")();
const same = (name) => read(path.join(skills, name, "SKILL.md")) === read(path.join(dist, "skills", name, "SKILL.md"));

// No .claude/ yet: it is made, and the ticked skills (not tickets) written.
assert.equal(fs.existsSync(path.join(folder, ".claude")), false);
await addSkills();
assert.ok(same("kb") && same("lap") && same("techdocs"), "kb, lap and techdocs are written as shipped");
assert.ok(fs.existsSync(path.join(skills, "lap", "references", "branches.md")), "with every file of the folder");
assert.equal(fs.existsSync(path.join(skills, "tickets")), false, "tickets is not written unless picked");
assert.match(infos.at(-1), /added kb, lap, techdocs/);

// Another skill and settings in .claude/ are left as they are; tickets goes in when picked.
fs.mkdirSync(path.join(skills, "other"), { recursive: true });
fs.writeFileSync(path.join(skills, "other", "SKILL.md"), "mine\n");
fs.writeFileSync(path.join(folder, ".claude", "settings.json"), '{"x":1}\n');
pick = (items) => items;
await addSkills();
assert.ok(same("tickets"), "tickets is written when picked");
assert.equal(read(path.join(skills, "other", "SKILL.md")), "mine\n", "another skill is untouched");
assert.equal(read(path.join(folder, ".claude", "settings.json")), '{"x":1}\n', "settings are untouched");

// An identical copy: nothing asked, nothing done.
const questions = warnings.length;
await addSkills();
assert.equal(warnings.length, questions, "nothing is asked about identical copies");
assert.match(infos.at(-1), /already has procode's skills/);

// A different copy: asked; Show differences opens a diff and asks again; Keep mine leaves it; Update replaces it.
fs.appendFileSync(path.join(skills, "lap", "SKILL.md"), "my own note\n");
pick = (items) => items.filter((i) => i.label === "lap");
answers.push("Show differences", "Keep mine");
await addSkills();
assert.equal(warnings.length, questions + 2, "asked, shown the differences, asked again");
assert.equal(executed.filter((c) => c[0] === "vscode.diff").length, 1, "the differences are a diff");
assert.match(read(path.join(skills, "lap", "SKILL.md")), /my own note/, "Keep mine leaves it");
assert.match(infos.at(-1), /kept yours of lap/);
answers.push("Update");
await addSkills();
assert.ok(same("lap"), "Update replaces it with procode's");
assert.match(infos.at(-1), /updated lap/);

// Nothing outside the project's .claude/ was written.
const outsideAfter = tree(folder).filter((f) => !f.startsWith(".claude")).map((f) => `${f}:${read(path.join(folder, f))}`);
assert.deepEqual(outsideAfter, outsideBefore, "nothing outside .claude/ changed");

// The update notice: a copy procode wrote at an older version is offered an
// update; a copy changed since is only mentioned.
const skillMd = path.join(skills, "lap", "SKILL.md");
fs.writeFileSync(skillMd, read(skillMd).replace(/version: "\d+"/, 'version: "0"'));
memento.set("procode.installedSkills", { ...memento.get("procode.installedSkills"), lap: ext.folderHash(path.join(skills, "lap")) });
infos.length = 0;
ext.checkSkills(ctx);
assert.ok(infos.some((m) => /newer procode skills are available: lap/.test(m)), "an older copy as written is offered an update");
fs.appendFileSync(skillMd, "changed by hand\n");
infos.length = 0;
ext.checkSkills(ctx);
assert.ok(!infos.some((m) => /are available/.test(m)), "a changed copy is not offered one");
assert.ok(infos.some((m) => /lap are shipped; this project's copies were changed/.test(m)), "it is only mentioned");

// Stops what the parts started (ask's server among them), so the check ends.
for (const d of ctx.subscriptions) d.dispose?.();
console.log(`check: ${registered.size} commands registered, ${defs.length + (withAsk ? 1 : 0)} MCP servers${withAsk ? " (ask's over HTTP)" : ""}, no errors`);
console.log(`check: workspace ${folder}`);
