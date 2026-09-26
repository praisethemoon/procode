/* Copies the stylesheets a board tab links and bundles the two halves: the
 * webviews (a tab and the sidebar; React + baukasten, for the browser) and the extension host (with
 * coboard inlined, so the .vsix needs no node_modules). */

import * as esbuild from "esbuild";
import * as fs from "node:fs";
import * as path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const out = path.join(root, "out", "media");
const dev = process.argv.includes("--dev");

const req = createRequire(path.join(root, "package.json"));
const fromBaukasten = createRequire(req.resolve("baukasten-ui"));
fs.mkdirSync(out, { recursive: true });
for (const [src, to] of [
    [req.resolve("baukasten-ui/dist/baukasten-base.css"), "baukasten-base.css"],
    [req.resolve("baukasten-ui/dist/baukasten-vscode.css"), "baukasten-vscode.css"],
    [fromBaukasten.resolve("@vscode/codicons/dist/codicon.css"), "codicon.css"],
    [fromBaukasten.resolve("@vscode/codicons/dist/codicon.ttf"), "codicon.ttf"],
    [path.join(root, "assets", "board.css"), "board.css"],
]) {
    fs.copyFileSync(src, path.join(out, to));
}
const codicon = path.join(out, "codicon.css");
fs.writeFileSync(
    codicon,
    fs.readFileSync(codicon, "utf8").replace(/url\(["']?\.?\/?codicon\.ttf[^)"']*["']?\)/g, 'url("./codicon.ttf")'),
);

await esbuild.build({
    entryPoints: { board: path.join(root, "webview", "main.tsx"), sidebar: path.join(root, "webview", "sidebar.tsx") },
    outdir: out,
    bundle: true,
    format: "iife",
    platform: "browser",
    target: "es2022",
    jsx: "automatic",
    minify: !dev,
    logLevel: "warning",
    define: { "process.env.NODE_ENV": JSON.stringify(dev ? "development" : "production") },
    plugins: [
        {
            // The webview is a browser document: the store and lap live on
            // the host's side of the wire.
            name: "no-host-imports",
            setup(build) {
                build.onResolve({ filter: /^(node:|vscode$|coboard$)/ }, (args) => ({
                    errors: [{ text: `the webview may not import "${args.path}" (from ${args.importer})` }],
                }));
            },
        },
    ],
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
