/* Builds the combined extension into dist/, and with --package turns dist/
 * into one .vsix for every platform: nothing in it is native. The lap and kb
 * CLIs are not packaged; users build them, and the parts find them through
 * their settings.
 *
 *   npm run build   --workspace combined     dist/ only
 *   npm run package --workspace combined     dist/ and procode-<version>.vsix
 *
 * dist/ holds:
 *   package.json   generated: the four extensions' contributions merged, plus
 *                  procode's own command and MCP provider
 *   out/extension.js   the entry, with Lap History, Knowledge, the Board and
 *                      techdocs bundled in
 *   out/media/         the webview assets of Lap History, Knowledge, the Board and techdocs
 *   out/pdfjs/         pdf.js, which Knowledge imports at runtime to read PDFs
 *   out/mcp/coboard.js, out/mcp/kb.js, out/mcp/techdocs.js
 *                      the MCP servers, one file each
 *   media/             the parts' view icons, and procode-views.svg for the
 *                      one activity-bar container that holds their views
 *   skills/            the repository's .claude/skills, every file, for
 *                      procode: Add Skills for Claude Code
 *   icon.png           procode's own icon, from packages/combined/media
 */

import { execFileSync } from "node:child_process";
import * as esbuild from "esbuild";
import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repo = path.resolve(here, "..", "..");
const dist = path.join(here, "dist");
const VERSION = JSON.parse(fs.readFileSync(path.join(here, "package.json"), "utf8")).version;
const PARTS = ["lap-vscode", "index-vscode", "coboard-vscode", "techdocs-vscode"];

/* npm, tsc and vsce all run as JavaScript on this Node. Their commands on
 * PATH are .cmd wrappers on Windows, which execFileSync cannot start without
 * a shell. npm names its own entry in npm_execpath for every script it runs;
 * run directly, this script falls back to npm on PATH through a shell. */
const node = (script, args, cwd = repo) =>
    execFileSync(process.execPath, [script, ...args], { cwd, stdio: "inherit" });
const npm = (args) =>
    process.env.npm_execpath
        ? node(process.env.npm_execpath, args)
        : execFileSync("npm", args, { cwd: repo, stdio: "inherit", shell: process.platform === "win32" });
const bin = (...p) => path.join(repo, "node_modules", ...p);
const readJson = (p) => JSON.parse(fs.readFileSync(p, "utf8"));

/* ---------------------------------------------------------- build the parts */

for (const w of [...PARTS, "kb-mcp"]) {
    npm(["run", "compile", "--workspace", w]);
}
node(bin("typescript", "bin", "tsc"), ["-p", path.join(here, "tsconfig.json")]);

/* --------------------------------------------------------------- fresh dist
 * dist/ is rebuilt from nothing so no file from an earlier build can ride
 * along into the package. The one recursive delete in this script, and it
 * refuses any path but this package's own dist/. */

if (path.basename(dist) !== "dist" || path.dirname(dist) !== here || !here.endsWith(path.join("packages", "combined"))) {
    throw new Error(`refusing to clear ${dist}`);
}
fs.rmSync(dist, { recursive: true, force: true });
fs.mkdirSync(path.join(dist, "out", "media"), { recursive: true });

/* ------------------------------------------------------------- the manifest
 * Arrays concatenate; objects of arrays (views, menus) merge key by key;
 * `configuration` becomes a list of the parts' sections. A command, view or
 * container id that two parts both claim is an error, not a silent winner. */

const contributes = {};
const activation = new Set(["onStartupFinished"]);
for (const part of PARTS) {
    const m = readJson(path.join(repo, "packages", part, "package.json"));
    for (const e of m.activationEvents ?? []) activation.add(e);
    for (const [key, value] of Object.entries(m.contributes ?? {})) {
        if (key === "configuration") {
            contributes.configuration = [...(contributes.configuration ?? []), ...(Array.isArray(value) ? value : [value])];
        } else if (Array.isArray(value)) {
            contributes[key] = [...(contributes[key] ?? []), ...value];
        } else {
            contributes[key] ??= {};
            for (const [k, v] of Object.entries(value)) {
                contributes[key][k] = [...(contributes[key][k] ?? []), ...v];
            }
        }
    }
}
contributes.commands.push(
    { command: "procode.setUpClaudeMcp", title: "Set Up MCP for Claude Code (This Project's .mcp.json)", category: "procode" },
    { command: "procode.addClaudeSkills", title: "Add Skills for Claude Code (This Project's .claude/skills)", category: "procode" },
);
contributes.mcpServerDefinitionProviders = [{ id: "procode.mcp", label: "procode" }];

/* One activity-bar icon for the whole extension. Each part brings its own
 * container for when it runs alone; here their views move into procode's,
 * stacked in STACK's order, and each keeps its part's icon for when someone
 * drags it out into a container of its own. A part's container missing from
 * STACK is an error, so a new part is placed on purpose. */
const STACK = ["coboard", "lap-explorer", "techdocs", "knowledge"];
const partContainers = contributes.viewsContainers.activitybar;
for (const c of partContainers) {
    if (!STACK.includes(c.id)) throw new Error(`container ${c.id} has no place in procode's STACK`);
}
partContainers.sort((a, b) => STACK.indexOf(a.id) - STACK.indexOf(b.id));
contributes.viewsContainers.activitybar = [{ id: "procode", title: "procode", icon: "media/procode-views.svg" }];
contributes.views = {
    ...Object.fromEntries(Object.entries(contributes.views).filter(([k]) => !partContainers.some((c) => c.id === k))),
    procode: partContainers.flatMap((c) => (contributes.views[c.id] ?? []).map((v) => ({ ...v, icon: v.icon ?? c.icon }))),
};

const ids = [
    ...contributes.commands.map((c) => `command ${c.command}`),
    ...Object.values(contributes.views ?? {}).flat().map((v) => `view ${v.id}`),
    ...(contributes.viewsContainers?.activitybar ?? []).map((c) => `container ${c.id}`),
];
const dup = ids.find((id, i) => ids.indexOf(id) !== i);
if (dup) {
    throw new Error(`two parts both contribute ${dup}`);
}

const manifest = {
    name: "procode",
    displayName: "procode",
    description: "Lap History, Knowledge, the Board and techdocs in one extension, with the lap and kb CLIs and the kb, coboard and techdocs MCP servers.",
    version: VERSION,
    publisher: "praisethemoon",
    author: { name: "Soulaymen Chouri", email: "doit@praisethemoon.org" },
    license: "MIT",
    engines: { vscode: "^1.101.0" },
    categories: ["Other"],
    icon: "icon.png",
    main: "./out/extension.js",
    activationEvents: [...activation],
    contributes,
};
fs.writeFileSync(path.join(dist, "package.json"), JSON.stringify(manifest, null, 2) + "\n");

/* ------------------------------------------------------------------ assets */

for (const part of PARTS) {
    const media = path.join(repo, "packages", part, "out", "media");
    for (const f of fs.readdirSync(media)) {
        if (!f.endsWith(".map")) fs.copyFileSync(path.join(media, f), path.join(dist, "out", "media", f));
    }
}
// Each part's activity-bar icon, from its own media/. The manifest keeps the
// parts' "media/<name>.svg" paths, so every file lands in one media/ and a
// name two parts both ship is an error rather than a silent overwrite.
fs.mkdirSync(path.join(dist, "media"));
for (const part of PARTS) {
    const media = path.join(repo, "packages", part, "media");
    if (!fs.existsSync(media)) continue;
    for (const f of fs.readdirSync(media)) {
        const to = path.join(dist, "media", f);
        if (fs.existsSync(to)) throw new Error(`two parts both ship media/${f}`);
        fs.copyFileSync(path.join(media, f), to);
    }
}
// procode's own container icon, beside the parts' view icons.
if (fs.existsSync(path.join(dist, "media", "procode-views.svg"))) throw new Error("a part ships media/procode-views.svg");
fs.copyFileSync(path.join(here, "media", "procode-views.svg"), path.join(dist, "media", "procode-views.svg"));
// pdf.js, beside the bundle where Knowledge imports it from (index-vscode/src/pdf.ts).
fs.cpSync(path.join(repo, "packages", "index-vscode", "out", "pdfjs"), path.join(dist, "out", "pdfjs"), { recursive: true });
fs.copyFileSync(path.join(repo, "LICENSE"), path.join(dist, "LICENSE"));
// The skills, whole folders, so what is added to a project matches this
// build's tools.
fs.cpSync(path.join(repo, ".claude", "skills"), path.join(dist, "skills"), { recursive: true });
// The Extensions view's icon: a PNG rendered from media/procode.svg, since
// VS Code does not take an SVG there.
fs.copyFileSync(path.join(here, "media", "procode.png"), path.join(dist, "icon.png"));
// dist/ holds exactly what ships; this only tells vsce so.
fs.writeFileSync(path.join(dist, ".vscodeignore"), "**/*.map\n");
fs.writeFileSync(
    path.join(dist, "README.md"),
    "# procode\n\nLap History, Knowledge, the Board and techdocs in one extension.\n\n" +
        "Knowledge and the Board run the `kb` and `lap` CLIs, which you build with CMake from the procode " +
        "repository (see its README). They are found on PATH, or wherever " +
        "the settings **Knowledge › Cli Path** and **Board › Lap Path** point.\n\n" +
        "The kb, coboard and techdocs MCP servers are registered with VS Code's agent automatically. For Claude Code, " +
        "**procode: Set Up MCP for Claude Code** writes them into the project's `.mcp.json`, and " +
        "**procode: Add Skills for Claude Code** adds the skills that teach Claude to use them to its `.claude/skills`.\n",
);

/* ----------------------------------------------------------------- bundles */

await esbuild.build({
    entryPoints: [path.join(here, "src", "extension.ts")],
    outfile: path.join(dist, "out", "extension.js"),
    bundle: true,
    format: "cjs",
    platform: "node",
    target: "node18",
    logLevel: "warning",
    external: ["vscode"],
});
for (const [name, entry] of [
    ["coboard", `require(${JSON.stringify(path.join(repo, "packages/coboard/out/mcp.js"))}).main();`],
    ["techdocs", `require(${JSON.stringify(path.join(repo, "packages/techdocs/out/mcp.js"))}).main();`],
    [
        "kb",
        `require(${JSON.stringify(path.join(repo, "packages/kb-mcp/out/main.js"))}).main().then(` +
            `(code) => { process.exitCode = code; },` +
            `(e) => { process.stderr.write("kb-mcp: " + (e && e.message ? e.message : String(e)) + "\\n"); process.exitCode = 2; });`,
    ],
]) {
    await esbuild.build({
        stdin: { contents: entry, resolveDir: repo, loader: "js" },
        outfile: path.join(dist, "out", "mcp", `${name}.js`),
        bundle: true,
        format: "cjs",
        platform: "node",
        target: "node18",
        logLevel: "warning",
    });
}
console.log(`combined: built ${dist}`);

/* ---------------------------------------------------------------- package */

if (process.argv.includes("--package")) {
    // A .vsix that would fail to start is not worth producing.
    execFileSync(process.execPath, [path.join(here, "scripts", "check.mjs")], { cwd: here, stdio: "inherit" });
    const out = path.join(here, `procode-${VERSION}.vsix`);
    node(bin("@vscode", "vsce", "vsce"), ["package", "--no-dependencies", "--allow-missing-repository", "--out", out], dist);
    console.log(`combined: packaged ${out}`);
}
