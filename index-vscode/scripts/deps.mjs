/* Build the sibling package this extension ships inside its .vsix.
 *
 * `install-links=true` (see .npmrc) makes npm copy `kb-js` into node_modules as
 * a real directory rather than symlinking it, which is what the .vsix can carry
 * — but npm copies whatever is on disk at install time and never rebuilds it.
 * So the sibling is compiled first and then reinstalled, in that order, or the
 * extension ships yesterday's reader.
 *
 * `install-links` IS THIS PACKAGE'S SETTING AND MUST NOT LEAK SIDEWAYS. It
 * belongs to the packages that produce a `.vsix`, because vsce resolves a
 * symlink and would otherwise pack `../kb-js/**`, devDependencies and all. A
 * LIBRARY wants the symlink, and not for tidiness: with a copy in place,
 * rebuilding `kb-js` silently does not reach whoever installed it, which is the
 * exact staleness this script exists to prevent here.
 *
 * It leaks by two routes, and both are closed below:
 *
 *   - npm reads `.npmrc` from the CWD, not from `--prefix`. So
 *     `npm --prefix ../kb-js install` would run with THIS package's config.
 *     Every sibling command therefore runs with `cwd` set to the sibling.
 *   - npm exports its effective config to child processes as `npm_config_*`.
 *     A script launched by `npm run` inherits `npm_config_install_links=true`
 *     and hands it on to every npm it spawns, at any depth — so the cwd alone
 *     is not enough, and the variable is stripped from the child environment.
 *     The explicit `--install-links=false` is the third layer: a flag beats
 *     both a file and an environment variable, and the one thing that must not
 *     happen here is silent.
 *
 * A SIBLING THAT IS ALREADY CURRENT IS NOT REBUILT. Rewriting `out/*.js` under
 * a process that is running that package's own tests is how a test run loses a
 * file it was in the middle of loading.
 */

import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..");

/** Newest mtime under a directory, in ms; 0 if it is not there. */
function newest(dir) {
    let latest = 0;
    let entries;
    try {
        entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
        return 0;
    }
    for (const entry of entries) {
        const p = path.join(dir, entry.name);
        latest = Math.max(latest, entry.isDirectory() ? newest(p) : fs.statSync(p).mtimeMs);
    }
    return latest;
}

/* The environment a sibling's npm sees: this one, minus the config this
 * package sets for itself. Stripped rather than overridden, so the sibling's
 * own `.npmrc` — or its absence — is what decides. */
function siblingEnv() {
    const env = { ...process.env };
    delete env["npm_config_install_links"];
    return env;
}

function run(cwd, args, env = process.env) {
    try {
        execFileSync("npm", args, { cwd, env, stdio: ["ignore", "pipe", "pipe"] });
        return null;
    } catch (e) {
        return e;
    }
}

function fail(message, e) {
    console.error(message);
    if (e !== undefined && e !== null) {
        console.error(String(e.stdout ?? e.stderr ?? e.message));
    }
    process.exit(1);
}

for (const name of ["kb-js"]) {
    const dir = path.resolve(root, "..", name);
    const env = siblingEnv();
    if (newest(path.join(dir, "out")) <= newest(path.join(dir, "src"))) {
        run(dir, ["install", "--install-links=false", "--no-audit", "--no-fund", "--silent"], env);
        const failure = run(dir, ["run", "compile", "--silent"], env);
        if (failure !== null) {
            fail(`${name}: compile failed.`, failure);
        }
    }
    const entry = path.join(dir, "out", "index.js");
    if (!fs.existsSync(entry)) {
        fail(`${name}: ${entry} was not produced.`);
    }
    if (newest(path.join(dir, "out")) <= newest(path.join(dir, "src"))) {
        fail(`${name}: out/ is not newer than src/, so this extension would ship yesterday's reader.`);
    }
}

/* And only now the copy into THIS package's node_modules, so what lands there
 * is what was just built.
 *
 * The removal first is not belt and braces. npm keys a `file:` dependency by
 * its version, not by its contents, so a rebuilt sibling at the same version is
 * "already installed" and the stale copy stays.
 *
 * `baukasten-ui` IS IN THIS LIST AND IS NOT BUILT ABOVE. It is a `file:`
 * dependency on a checkout outside this workspace, and `install-links` copies
 * it exactly as it copies the sibling — so it inherits the same staleness trap
 * and none of the fix. It is NOT compiled here: it is somebody else's package
 * with its own build, and a script in this repository that ran it would be
 * claiming ownership of a tree it does not own. */
for (const name of ["kb-js", "baukasten-ui"]) {
    fs.rmSync(path.join(root, "node_modules", name), { recursive: true, force: true });
}
const failure = run(root, ["install", "--no-audit", "--no-fund", "--silent"]);
if (failure !== null) {
    fail("index-vscode: install failed.", failure);
}
const installed = path.join(root, "node_modules", "kb-js");
if (!fs.existsSync(installed) || fs.lstatSync(installed).isSymbolicLink()) {
    fail(
        `node_modules/kb-js is a symlink or missing. vsce resolves a symlink to its real path and packages "../kb-js/**" at paths outside the extension, so the .vsix would have no node_modules/kb-js and the installed extension could not require it. Check .npmrc.`,
    );
}

/* baukasten never reaches the .vsix as a package — esbuild bundles its
 * JavaScript into `out/media/knowledge-webview.js` and `assets.mjs` copies its
 * two stylesheets into `out/media/` — so what has to be true of it is only that
 * the build can read it, and that it is the checkout rather than the registry.
 * The `Tree` chunk is the difference between the two builds and is the cheapest
 * thing to look for. */
const baukastenDist = path.join(root, "node_modules", "baukasten-ui", "dist");
if (!fs.existsSync(path.join(baukastenDist, "baukasten-vscode.css"))) {
    fail("node_modules/baukasten-ui/dist is missing; the surface has no token layer to ship.");
}
if (!fs.readdirSync(baukastenDist).some((f) => f.startsWith("Tree-"))) {
    fail(
        "node_modules/baukasten-ui is the published 0.3.0, not the checkout this package depends on. Both call themselves 0.3.0, so npm considers the registry copy to satisfy a file: dependency it installed earlier. Repair it with: rm -rf node_modules/baukasten-ui && npm install",
    );
}
