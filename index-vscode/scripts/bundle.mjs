/* The webview bundle.
 *
 * index-ui.md §1 commits the sidebar to baukasten rather than to a native tree,
 * and baukasten is a React component library — so this package brings React,
 * react-dom and baukasten with it, and needs a bundler to get them into a
 * document. `kb-js` gains nothing from this file and must never be in the
 * output: it spawns a process, which a browser document cannot do.
 *
 * ONE BUNDLE, TWO VIEWS. The sidebar and a `kb:` tab share their row
 * rendering, their RPC client and their pure view layer, and two bundles would
 * carry two copies of React to save nothing.
 *
 * `src/view/*` IS COMPILED TWICE ON PURPOSE — by tsc into `out/` for the tests,
 * and by esbuild into the bundle. They are the same sources, so the behaviour a
 * test pins is the behaviour that ships.
 *
 * `kb-js` IS BUNDLED FOR ITS TYPES AND FOR THREE PURE FUNCTIONS, WHICH IS WHY
 * IT IS NOT IN THE REFUSAL LIST BELOW. `isStale`, `fetchedAtKey` and the
 * shapes are all `src/view` needs from it; the client that spawns is reached
 * only from the extension host. The refusal is on `node:*`, which is what the
 * spawn actually uses — so importing `Kb` into a webview module fails the
 * build with the name of the file that did it.
 */

import * as esbuild from "esbuild";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..");
const dev = process.argv.includes("--dev");

await esbuild.build({
    entryPoints: [path.join(root, "webview", "main.tsx")],
    outfile: path.join(root, "out", "media", "knowledge-webview.js"),
    bundle: true,
    format: "iife",
    platform: "browser",
    target: "es2022",
    jsx: "automatic",
    minify: !dev,
    sourcemap: dev ? "inline" : false,
    logLevel: "warning",
    external: [],
    alias: {},
    define: { "process.env.NODE_ENV": JSON.stringify(dev ? "development" : "production") },
    plugins: [
        {
            name: "no-host-only-imports",
            setup(build) {
                build.onResolve({ filter: /^(node:|vscode$)/ }, (args) => ({
                    errors: [
                        {
                            text: `The webview bundle may not import "${args.path}": it is a browser document, and ${args.importer} reached for the extension host's side of the wire.`,
                        },
                    ],
                }));
            },
        },
    ],
});
