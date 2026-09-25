/* Put the sibling reader where `require("kb-js")` will find it.
 *
 * A SYMLINK AND NOT A COPY, which is the opposite of what `index-vscode` does
 * and for a reason worth stating rather than discovering. That package ships a
 * `.vsix` and vsce resolves a symlink out of the tree, so it needs a real
 * directory. This one is a server that runs from the checkout: a copy here
 * would mean rebuilding `kb-js` silently does not reach the process that loads
 * it, and the two halves of one repository would drift apart between commits.
 *
 * NOT `npm install`. `file:../kb-js` in package.json says what the dependency
 * is; this makes it resolvable without a registry, a lockfile or a network —
 * which matters because the only third-party name in that file is the sibling
 * beside it, and nothing here should need to reach outside the checkout to
 * start.
 */

import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const modules = path.join(root, "node_modules");
const link = path.join(modules, "kb-js");

fs.mkdirSync(modules, { recursive: true });

/* Replaced rather than left alone when it is anything other than the link this
 * wants: a stale copy from an earlier `npm install` is the failure this script
 * exists to prevent, and it looks exactly like success until something is
 * rebuilt. */
let current = null;
try {
    current = fs.readlinkSync(link);
} catch {
    current = null;
}
if (current !== "../../kb-js") {
    fs.rmSync(link, { recursive: true, force: true });
    fs.symlinkSync("../../kb-js", link, "dir");
}

const entry = path.join(link, "out", "index.js");
if (!fs.existsSync(entry)) {
    console.error(
        `kb-js is linked but not built: ${entry} is missing. Compile the sibling first; this package is a wrapper over it and has nothing to answer with on its own.`,
    );
    process.exit(1);
}
