/* Does the extension run from the PACKAGED tree?
 *
 * Run with `npm run verify:package`, which packages first.
 *
 * The .vsix is unzipped somewhere this script has never written to, a fake
 * `vscode` module is put in front of the loader, and the extension is activated
 * out of that directory — so what is exercised is the bytes vsce produced, not
 * the working tree. The extension host is bundled with kb-js inlined, so the
 * package must carry no node_modules and must never ask for kb-js at runtime.
 *
 * Then a webview is resolved and driven against a REAL store: a status call, a
 * document read and an §11 refusal all go through the packaged extension host,
 * the kb-js bundled into it, and the `kb` binary.
 *
 * IT NEVER TOUCHES A STORE IT WAS NOT ASKED TO. The workspace folder is a
 * throwaway directory with its own `.kb/`, and `kb` finds the store by walking
 * up from there — so the nearest store is always the temporary one.
 */

import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as Module from "node:module";
import * as os from "node:os";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const pkg = JSON.parse(fs.readFileSync(path.join(ROOT, "package.json"), "utf8"));
const VSIX = path.join(ROOT, `${pkg.name}-${pkg.version}.vsix`);
if (!fs.existsSync(VSIX)) {
    console.error(`${VSIX} is not there. Run \`npm run package\` first.`);
    process.exit(1);
}

const KB = path.resolve(ROOT, "..", "..", "cli", "kb-cli", "bin", "kb");
if (!fs.existsSync(KB)) {
    console.error(`${KB} is not built. The packaged smoke drives the real binary.`);
    process.exit(1);
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "knowledge-vsix-"));
execFileSync("unzip", ["-q", VSIX, "-d", tmp]);
const EXT = path.join(tmp, "extension");
console.log(`unpacked to ${EXT}`);

/* A throwaway workspace with a real store in it. */
const work = fs.mkdtempSync(path.join(os.tmpdir(), "knowledge-ws-"));
const project = path.join(work, "project");
fs.mkdirSync(project);
execFileSync(KB, ["init"], { cwd: project });
execFileSync(KB, ["add", "--title", "IOCP", "--collection", "win32-iocp", "--mime", "text/markdown", "--file", "-"], {
    cwd: project,
    input: "# IOCP\n\nCreateIoCompletionPort binds a handle to a port.\n",
});

/* ------------------------------------------------------- the fake vscode */

const registrations = [];
const messages = [];
const notifications = [];

class Uri {
    constructor(scheme, authority, p, query = "") {
        this.scheme = scheme;
        this.authority = authority;
        this.path = p;
        this.query = query;
    }
    static file(p) {
        return new Uri("file", "", p);
    }
    static from({ scheme, path: p }) {
        return new Uri(scheme, "", p);
    }
    static parse(s) {
        const i = s.indexOf(":");
        return new Uri(s.slice(0, i), "", s.slice(i + 1));
    }
    static joinPath(base, ...parts) {
        return new Uri(base.scheme, base.authority, path.join(base.path, ...parts));
    }
    get fsPath() {
        return this.path;
    }
    toString() {
        return `${this.scheme}://${this.authority}${this.path}`;
    }
}

class Disposable {
    constructor(fn) {
        this.dispose = fn ?? (() => {});
    }
}

class RelativePattern {
    constructor(base, pattern) {
        this.base = base;
        this.pattern = pattern;
    }
}

const vscode = {
    Uri,
    Disposable,
    RelativePattern,
    window: {
        registerCustomEditorProvider(viewType, provider, options) {
            registrations.push({ what: "customEditor", viewType, provider, options });
            return new Disposable();
        },
        registerWebviewViewProvider(id, provider, options) {
            registrations.push({ what: "webviewView", id, provider, options });
            return new Disposable();
        },
        createQuickPick() {
            const picker = {
                onDidChangeValue() {},
                onDidAccept() {},
                onDidHide() {},
                show() {},
                hide() {},
                dispose() {},
            };
            registrations.push({ what: "quickPick", picker });
            return picker;
        },
        showWarningMessage(...args) {
            notifications.push(["warning", args[0]]);
            return Promise.resolve(undefined);
        },
        showErrorMessage(m) {
            notifications.push(["error", m]);
            return Promise.resolve(undefined);
        },
        showInformationMessage(m) {
            notifications.push(["info", m]);
            return Promise.resolve(undefined);
        },
        showInputBox() {
            return Promise.resolve(undefined);
        },
        showQuickPick() {
            return Promise.resolve(undefined);
        },
        activeTextEditor: undefined,
    },
    workspace: {
        workspaceFolders: [{ uri: Uri.file(project) }],
        getConfiguration() {
            return {
                get(key, fallback) {
                    if (key === "cliPath") {
                        return KB;
                    }
                    if (key === "staleAfterDays") {
                        return 90;
                    }
                    return fallback;
                },
            };
        },
        createFileSystemWatcher(pattern) {
            registrations.push({ what: "watcher", pattern });
            return {
                onDidChange() {},
                onDidCreate() {},
                onDidDelete() {},
                dispose() {},
            };
        },
        registerFileSystemProvider(scheme, provider, options) {
            registrations.push({ what: "fileSystem", scheme, provider, options });
            return new Disposable();
        },
        onDidChangeConfiguration() {
            return new Disposable();
        },
        onDidChangeWorkspaceFolders() {
            return new Disposable();
        },
    },
    commands: {
        registerCommand(id, fn) {
            registrations.push({ what: "command", id, fn });
            return new Disposable();
        },
        executeCommand(id, ...args) {
            messages.push(["executeCommand", id, args]);
            return Promise.resolve();
        },
    },
    env: {
        openExternal(u) {
            messages.push(["openExternal", String(u)]);
            return Promise.resolve(true);
        },
    },
};

const originalLoad = Module.default._load;
Module.default._load = function (request, parent, isMain) {
    if (request === "vscode") {
        return vscode;
    }
    return originalLoad.call(this, request, parent, isMain);
};

/* ------------------------------------------------------------- activate */

/* The bundle is self-contained: no node_modules shipped, and nothing in the
 * host asks for kb-js at runtime. Either would mean the installed extension
 * depends on files the .vsix does not carry. */
if (fs.existsSync(path.join(EXT, "node_modules"))) {
    console.error("FAIL: the .vsix ships node_modules; the host bundle should make it unnecessary");
    process.exit(1);
}
if (/require\(["']kb-js["']\)/.test(fs.readFileSync(path.join(EXT, "out", "extension.js"), "utf8"))) {
    console.error("FAIL: out/extension.js still requires kb-js at runtime; it was not bundled");
    process.exit(1);
}
console.log("host bundle is self-contained: no node_modules, no runtime require of kb-js");

const ext = Module.default.createRequire(import.meta.url)(path.join(EXT, "out", "extension.js"));
const subscriptions = [];
ext.activate({ extensionUri: Uri.file(EXT), subscriptions });
console.log(
    "registered:",
    registrations.map((r) => r.what + (r.viewType ?? r.id ?? r.scheme ?? "")).join(", "),
);

const fail = (why) => {
    console.error("FAIL:", why);
    process.exitCode = 1;
};

if (!registrations.some((r) => r.what === "customEditor" && r.viewType === "knowledge.document")) {
    fail("no custom editor");
}
if (!registrations.some((r) => r.what === "webviewView" && r.id === "knowledge.documents")) {
    fail("no webview view");
}
for (const id of [
    "knowledge.open",
    "knowledge.search",
    "knowledge.collections",
    "knowledge.addCurrentFile",
    "knowledge.addUrl",
    "knowledge.refreshStale",
    "knowledge.init",
]) {
    if (!registrations.some((r) => r.what === "command" && r.id === id)) {
        fail(`no command ${id}`);
    }
}

const editor = registrations.find((r) => r.what === "customEditor");
if (editor.options.supportsMultipleEditorsPerDocument !== false) {
    fail("supportsMultipleEditorsPerDocument is not false, so one document can have two tabs");
}

/* And what ships registers NO file system provider (index-ui.md §6). VSCode
 * draws the breadcrumb and the editor-type dropdown on exactly one condition —
 * whether the file service has a provider for the scheme — so a provider here
 * is a header bar on every document tab. Checked in the packaged tree rather
 * than only in source, because this is about what activation actually does. */
if (registrations.some((r) => r.what === "fileSystem")) {
    fail("a FileSystemProvider is registered for kb:, which puts a header above every document tab");
}

/* The watcher is over §1.6's two logs and nothing else. */
const watcher = registrations.find((r) => r.what === "watcher");
if (!watcher || watcher.pattern.pattern !== ".kb/{documents,sources}.jsonl") {
    fail(`the store watcher is ${JSON.stringify(watcher?.pattern)}`);
}

/* ------------------------------------------------- drive a real webview */

let html = "";
const inbox = [];
let onMessage = () => {};
const webview = {
    options: {},
    cspSource: "https://file+.vscode-resource.vscode-cdn.net",
    set html(v) {
        html = v;
    },
    get html() {
        return html;
    },
    asWebviewUri(u) {
        return new Uri("https", "file+.vscode-resource.vscode-cdn.net", u.path.replace(/^\/+/, "/"));
    },
    onDidReceiveMessage(fn) {
        onMessage = fn;
        return new Disposable();
    },
    postMessage(m) {
        inbox.push(m);
        return Promise.resolve(true);
    },
};
const panel = {
    webview,
    title: "",
    onDidDispose() {
        return new Disposable();
    },
};

const provider = editor.provider;
const doc = provider.openCustomDocument(Uri.from({ scheme: "kb", path: "/D-1" }));
provider.resolveCustomEditor(doc, panel);

if (!html.includes("knowledge-webview.js")) {
    fail("the webview document does not load the bundle");
}
if (!/script-src 'nonce-[0-9a-f]{32}'/.test(html)) {
    fail("the webview CSP has no nonce");
}
if ((html.match(/<script nonce="[0-9a-f]{32}"/g) ?? []).length !== 2) {
    fail("a script in the document carries no nonce to match the policy");
}
if (!html.includes('"view":"entity"') || !html.includes('"reference":"D-1"')) {
    fail("the view tag was not written into the page");
}
if (!html.includes("knowledge.css")) {
    fail("the document does not link this package's own stylesheet");
}
if (html.includes("baukasten-web.css")) {
    fail("the document links the web token layer, which ignores the user's theme");
}
if (panel.title !== "D-1") {
    fail(`the tab is called ${JSON.stringify(panel.title)} before the store answers`);
}

/* A refusal for a scheme that is not kb:. */
try {
    provider.openCustomDocument(Uri.from({ scheme: "file", path: "/etc/passwd" }));
    fail("the editor opened a file:// resource");
} catch {
    /* expected */
}

/* ------------------------------------------- real calls, real binary */

const call = async (id, op, input) => {
    onMessage({ kind: "call", id, op, input });
    for (let i = 0; i < 200 && !inbox.some((m) => m.id === id); i++) {
        await new Promise((r) => setTimeout(r, 10));
    }
    return inbox.find((m) => m.id === id);
};

const status = await call(1, "status", {});
if (!status || status.kind !== "result") {
    fail(`status did not come back: ${JSON.stringify(status)}`);
} else if (
    !status.value.present ||
    typeof status.value.path !== "string" ||
    fs.realpathSync(status.value.path) !== fs.realpathSync(path.join(project, ".kb"))
) {
    fail(`the packaged extension read the wrong store: ${JSON.stringify(status.value)}`);
}

const list = await call(2, "ls", { limit: 10 });
if (!list || list.kind !== "result" || list.value.length !== 1) {
    fail(`ls did not come back: ${JSON.stringify(list)}`);
} else if (list.value[0].collection !== "win32-iocp" || list.value[0].mime !== "text/markdown") {
    fail(`the document row lost a field: ${JSON.stringify(list.value[0])}`);
}

const read = await call(3, "get", { id: "D-1" });
if (!read || read.kind !== "result") {
    fail(`get did not come back: ${JSON.stringify(read)}`);
} else {
    if (!read.value.text.includes("CreateIoCompletionPort")) {
        fail("the document's text did not cross the wire");
    }
    if (!Array.isArray(read.value.chunks) || read.value.chunks.length < 1) {
        fail("the document's chunks did not cross the wire");
    }
}

/* An §11 refusal, kept apart from a fault. */
const refused = await call(4, "get", { id: "D-9999" });
if (!refused || refused.kind !== "failed") {
    fail(`the refusal arrived as ${refused?.kind}: ${JSON.stringify(refused)}`);
} else if (refused.error.code !== "not_found" || refused.error.spec !== "not_found") {
    fail(`the refusal lost its §11 code: ${JSON.stringify(refused.error)}`);
}

/* A command the CLI does not have yet answers a refusal a person can read,
 * rather than a fault. index-api.md §5's refresh is the case. */
const refresh = await call(5, "refresh", { olderThan: "90d" });
if (!refresh || (refresh.kind !== "failed" && refresh.kind !== "result")) {
    fail(`refresh arrived as ${refresh?.kind}: ${JSON.stringify(refresh)}`);
} else if (refresh.kind === "failed") {
    console.log(`refresh is not implemented in this kb: ${refresh.error.code} — ${refresh.error.message}`);
}

/* A link out, through the packaged host: the scheme is checked and a
 * confirmation is raised before the system handler ever sees it. */
onMessage({ kind: "link", href: "command:workbench.action.terminal.new" });
await new Promise((r) => setTimeout(r, 10));
if (messages.some(([k]) => k === "openExternal")) {
    fail("a command: URI reached openExternal");
}
if (!notifications.some(([lvl, m]) => lvl === "warning" && /did not open that link/.test(m))) {
    fail("the refused link said nothing");
}

/* Opening a reference goes through vscode.openWith with the canonical URI. */
onMessage({ kind: "open", reference: "d-1", chunk: null, preview: false });
await new Promise((r) => setTimeout(r, 20));
const opened = messages.find(([k, id]) => k === "executeCommand" && id === "vscode.openWith");
if (!opened) {
    fail("open did not reach vscode.openWith");
} else if (String(opened[2][0]) !== "kb:///D-1" || opened[2][1] !== "knowledge.document") {
    fail(`openWith got ${String(opened[2][0])} / ${opened[2][1]}`);
}

/* A chunk is a reference and is not a place: opening one says so rather than
 * building a tab whose whole content is a passage with its provenance cut
 * off. */
const before = messages.filter(([k, id]) => k === "executeCommand" && id === "vscode.openWith").length;
onMessage({ kind: "open", reference: "C-1", chunk: null, preview: false });
await new Promise((r) => setTimeout(r, 20));
const after = messages.filter(([k, id]) => k === "executeCommand" && id === "vscode.openWith").length;
if (after !== before) {
    fail("a chunk opened a tab of its own");
}

fs.rmSync(tmp, { recursive: true, force: true });
fs.rmSync(work, { recursive: true, force: true });
for (const d of subscriptions) {
    if (d && typeof d.dispose === "function") d.dispose();
}
console.log(process.exitCode ? "\nPACKAGED SMOKE: FAILED" : "\nPACKAGED SMOKE: OK");
