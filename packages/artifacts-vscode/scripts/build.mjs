/* Copies the two stylesheets an artifact's frame is built with, and the
 * list's stylesheet and icon font; bundles the list's webview script for the
 * browser, and the extension host with the artifacts store inlined, so the
 * .vsix needs no node_modules. The tests run against the unbundled out/ tree,
 * so the host bundle is written beside it as out/extension.js only. */

import * as esbuild from "esbuild";
import * as fs from "node:fs";
import * as path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const out = path.join(root, "out", "media");
const req = createRequire(path.join(root, "package.json"));

fs.mkdirSync(out, { recursive: true });
fs.copyFileSync(req.resolve("baukasten-ui/dist/baukasten-vscode.css"), path.join(out, "baukasten-vscode.css"));
fs.copyFileSync(path.join(root, "assets", "artifact.css"), path.join(out, "artifact.css"));
fs.copyFileSync(path.join(root, "assets", "pages.css"), path.join(out, "pages.css"));
/* The codicons baukasten brings, with the font next to its stylesheet. */
const fromBaukasten = createRequire(req.resolve("baukasten-ui"));
fs.copyFileSync(fromBaukasten.resolve("@vscode/codicons/dist/codicon.ttf"), path.join(out, "codicon.ttf"));
fs.writeFileSync(
    path.join(out, "codicon.css"),
    fs
        .readFileSync(fromBaukasten.resolve("@vscode/codicons/dist/codicon.css"), "utf8")
        .replace(/url\(["']?\.?\/?codicon\.ttf[^)"']*["']?\)/g, 'url("./codicon.ttf")'),
);

await esbuild.build({
    entryPoints: [path.join(root, "webview", "pages.ts")],
    outfile: path.join(out, "pages.js"),
    bundle: true,
    format: "iife",
    platform: "browser",
    target: "es2022",
    minify: true,
    logLevel: "warning",
});

await esbuild.build({
    entryPoints: [path.join(root, "out", "extension.js")],
    outfile: path.join(root, "out", "extension.js"),
    allowOverwrite: true,
    bundle: true,
    format: "cjs",
    platform: "node",
    target: "node18",
    logLevel: "warning",
    external: ["vscode"],
});
