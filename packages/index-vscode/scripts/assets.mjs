/* Copy what a webview has to load from disk.
 *
 * A VSCode webview's CSP reaches no network, so everything the document needs
 * is copied into `out/media/` at build time and served through `asWebviewUri`
 * from `localResourceRoots`. Nothing is fetched at runtime and nothing is
 * resolved out of `node_modules` by the installed extension — `.vscodeignore`
 * drops node_modules except for the one sibling package, and a stylesheet that
 * lived there would be missing from the .vsix in exactly the way that is not
 * noticed until somebody installs it.
 *
 * THE VSCODE TOKEN LAYER IS COPIED AND THE WEB ONE DELIBERATELY IS NOT. The
 * web build supplies fixed fallback values and would pin every surface to a
 * default palette that ignores the user's theme. `guards.test.ts` checks that
 * no file in this package names it.
 */

import * as fs from "node:fs";
import * as path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..");
const out = path.join(root, "out", "media");

/* Resolved rather than spelled as node_modules paths: in the npm workspace the
 * dependencies are hoisted to the repository root. codicons arrives through
 * baukasten, so it is resolved from where baukasten is. */
const fromHere = createRequire(path.join(root, "package.json"));
const fromBaukasten = createRequire(fromHere.resolve("baukasten-ui"));
const find = (resolve, spec) => {
    try {
        return resolve(spec);
    } catch {
        console.error(`assets: ${spec} is missing. Run npm install at the repository root first.`);
        process.exit(1);
    }
};

const COPIES = [
    [find(fromHere.resolve, "baukasten-ui/dist/baukasten-base.css"), "baukasten-base.css"],
    [find(fromHere.resolve, "baukasten-ui/dist/baukasten-vscode.css"), "baukasten-vscode.css"],
    [find(fromBaukasten.resolve, "@vscode/codicons/dist/codicon.css"), "codicon.css"],
    [find(fromBaukasten.resolve, "@vscode/codicons/dist/codicon.ttf"), "codicon.ttf"],
    [path.join(root, "assets", "knowledge.css"), "knowledge.css"],
];

fs.mkdirSync(out, { recursive: true });
for (const [src, to] of COPIES) {
    fs.copyFileSync(src, path.join(out, to));
}

/* The codicon stylesheet points at the font with a relative `url(./codicon.ttf)`
 * carrying a cache-busting query. Both files land side by side above, so the
 * reference resolves — but the query string is a version hash of a copy that is
 * no longer being fetched, and `font-src` is an origin match rather than a URL
 * match, so it is dropped rather than left to look like a fetch of something
 * else. */
const cssPath = path.join(out, "codicon.css");
const css = fs.readFileSync(cssPath, "utf8");
const fixed = css.replace(/url\(["']?\.?\/?codicon\.ttf[^)"']*["']?\)/g, 'url("./codicon.ttf")');
if (fixed === css) {
    console.error("assets: codicon.css no longer references codicon.ttf the way this script expects.");
    process.exit(1);
}
fs.writeFileSync(cssPath, fixed, "utf8");
