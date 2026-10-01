/* Copies the stylesheets a form's tab links, and the two a preview frame is
 * built with (baukasten's tokens and techdocs' page.css, so a preview looks
 * like a techdocs page); bundles the tab's webview (React + baukasten, for
 * the browser) and the extension host (with ask inlined, so the .vsix needs
 * no node_modules). */

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
    [path.join(root, "..", "techdocs-vscode", "assets", "page.css"), "page.css"],
    [path.join(root, "assets", "ask.css"), "ask.css"],
]) {
    fs.copyFileSync(src, path.join(out, to));
}
const codicon = path.join(out, "codicon.css");
fs.writeFileSync(
    codicon,
    fs.readFileSync(codicon, "utf8").replace(/url\(["']?\.?\/?codicon\.ttf[^)"']*["']?\)/g, 'url("./codicon.ttf")'),
);

await esbuild.build({
    entryPoints: { ask: path.join(root, "webview", "main.tsx") },
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
            // The webview is a browser document: the store lives on the
            // host's side of the wire. Types from "ask" are erased; a value
            // import would bring node:fs with it and is refused here.
            name: "no-host-imports",
            setup(build) {
                build.onResolve({ filter: /^(node:|vscode$|ask$)/ }, (args) => ({
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
