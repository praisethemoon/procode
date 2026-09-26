/* The checks that are about the whole package rather than about one function.
 *
 * Each of these is a change that would pass every other test in this directory
 * and break something that cannot be driven: a `console.log` that corrupts the
 * protocol, a dependency that arrives with a tree behind it, a call into the
 * half of `kb-js` that §9 decided not to expose. They are source scans because
 * the failure is a line that must never appear rather than a behaviour that can
 * be exercised.
 */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import { test } from "node:test";

import { NOT_EXPOSED, TOOL_NAMES } from "../tools";

const ROOT = path.resolve(__dirname, "..", "..");
const REPO = path.resolve(ROOT, "..", "..");

function isChecker(file: string): boolean {
    return file.startsWith(path.join("src", "test"));
}

function sources(): { file: string; text: string }[] {
    const out: { file: string; text: string }[] = [];
    const walk = (dir: string): void => {
        for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
            const p = path.join(dir, entry.name);
            if (entry.isDirectory()) {
                walk(p);
            } else if (/\.ts$/.test(entry.name)) {
                out.push({ file: path.relative(ROOT, p), text: fs.readFileSync(p, "utf8") });
            }
        }
    };
    walk(path.join(ROOT, "src"));
    return out;
}

/** Comments stripped, because a file's prose has to be able to name what it refuses. */
function code(text: string): string {
    return text.replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, "");
}

function shipped(): { file: string; text: string }[] {
    return sources().filter((s) => !isChecker(s.file));
}

/* ------------------------------------------------- stdout is the protocol */

test("nothing in this package prints", () => {
    /* A stray line on stdout lands in the middle of a JSON-RPC stream. The
     * client's reaction ranges from ignoring it to closing the session; what
     * it never does is name the module that printed. Diagnostics go to stderr,
     * and `transport.ts` takes its output stream as an argument so that there
     * is exactly one thing writing to the protocol. */
    for (const { file, text } of shipped()) {
        const c = code(text);
        for (const forbidden of [
            "console.log(",
            "console.info(",
            "console.debug(",
            "console.warn(",
            "process.stdout.write(",
        ]) {
            assert.ok(
                !c.includes(forbidden),
                `${file} calls ${forbidden}, which writes into the middle of the protocol`,
            );
        }
    }
});

test("the only stream written by name is stderr", () => {
    const writers = new Set<string>();
    for (const { file, text } of shipped()) {
        for (const m of code(text).matchAll(/process\.(stdout|stderr)\b/g)) {
            writers.add(`${file}:${m[1]}`);
        }
    }
    for (const writer of writers) {
        assert.ok(
            writer.endsWith(":stderr") || writer === "src/main.ts:stdout",
            `${writer} reaches for a standard stream; only main.ts hands stdout to the transport`,
        );
    }
});

/* -------------------------------------------------- the dependency rule */

test("this package depends on the sibling reader and on nothing else", () => {
    /* No SDK. The protocol here is a handshake, a list and a call; a
     * dependency tree to speak it would be a larger thing to audit than the
     * surface it exposes, and every consumer inherits whatever lands here. */
    const pkg = JSON.parse(fs.readFileSync(path.join(ROOT, "package.json"), "utf8")) as {
        dependencies?: Record<string, string>;
        devDependencies?: Record<string, string>;
    };
    assert.deepEqual(Object.keys(pkg.dependencies ?? {}), ["kb-js"]);
    /* "*" is the workspace sibling: npm links the kb-js in this repository. */
    assert.equal(pkg.dependencies?.["kb-js"], "*");
    assert.deepEqual(
        Object.keys(pkg.devDependencies ?? {}).sort(),
        ["@types/node", "typescript"],
        "a dev dependency arrived that is not the compiler or its types",
    );
});

test("nothing imports anything but Node, the sibling, and this package", () => {
    for (const { file, text } of sources()) {
        for (const m of code(text).matchAll(/\bfrom\s+["']([^"']+)["']/g)) {
            const spec = m[1];
            assert.ok(
                spec.startsWith(".") || spec.startsWith("node:") || spec === "kb-js",
                `${file} imports "${spec}"; this package depends on Node, on kb-js, and on nothing else`,
            );
        }
    }
});

test("nothing here spawns anything", () => {
    /* §10: retrieval has exactly one implementation, because searching
     * requires embedding the query and two inference paths that disagree
     * produce a store that answers differently depending on which door the
     * caller came through. `kb-js` starts the process; a second spawn site in
     * this package would be the beginning of the second door. */
    for (const { file, text } of shipped()) {
        const c = code(text);
        for (const forbidden of ["spawn(", "spawnSync(", "exec(", "execSync(", "execFile("]) {
            assert.ok(
                !c.includes(forbidden),
                `${file} starts a process; kb-js is the one place that does`,
            );
        }
    }
});

/* ------------------------------------------------------- §9's withholdings */

test("no tool reaches a part of kb-js that §9 decided not to expose", () => {
    /* THE CHECK THAT CANNOT BE DRIVEN. `kb-js` has `init`, `refresh`,
     * `renameCollection` and `deleteCollection` on the same object as
     * `search`; §9's list of what is NOT exposed is what keeps them out of an
     * agent's reach, and the distance between a tool that files knowledge and
     * a tool that forgets a whole topic is one method call in this file.
     *
     * `ls` and `status` are refused for a quieter reason: they are real
     * routes, they are useful, and §9's table simply does not carry them. A
     * surface that grows by usefulness rather than by the table is a surface
     * with no table. */
    const allowed = [
        "search",
        "chunk",
        "get",
        "add",
        "addBatch",
        "addDir",
        "collections",
        "stats",
        "links",
        "link",
        "stale",
    ];
    const forbidden = [
        "init",
        "refresh",
        "renameCollection",
        "deleteCollection",
        "forget",
        "refreshSource",
        "ls",
        "status",
        "at",
    ];
    const called = new Set<string>();
    for (const { file, text } of shipped()) {
        for (const m of code(text).matchAll(/\bkb\.([A-Za-z]+)\s*\(/g)) {
            called.add(m[1]);
            assert.ok(
                !forbidden.includes(m[1]),
                `${file} calls kb.${m[1]}(), which §9 does not put behind any of the six tools`,
            );
            assert.ok(
                allowed.includes(m[1]),
                `${file} calls kb.${m[1]}(), which is not one of the calls the six tools are built from`,
            );
        }
    }
    assert.ok(called.size >= 6, `only ${called.size} kb methods were found; the scan is broken`);
});

test("a folder is never filed with forgetting on", () => {
    /* §2.1's walk forgets a file gone from the folder unless it is told not
     * to, and that default is right for a reader and wrong for an agent (§9).
     * Every folder filing in the shipped source says so at the call, where a
     * refactor that dropped the option would have to delete it in view. */
    let seen = 0;
    for (const { file, text } of shipped()) {
        for (const m of code(text).matchAll(/\bkb\.addDir\s*\(([^;]*)\)/g)) {
            seen++;
            assert.match(m[1], /\bforget:\s*false\b/, `${file} files a folder without forget: false`);
        }
    }
    assert.ok(seen >= 1, "no folder filing was found; the scan is broken");
});

test("no argv builder for a withheld command is imported", () => {
    /* The other half of the same rule, one layer down: `deleteCollectionArgv`
     * exists in `kb-js` and importing it here would be a `kb_forget` waiting
     * to be wired up. */
    for (const { file, text } of shipped()) {
        for (const name of [
            "deleteCollectionArgv",
            "renameCollectionArgv",
            "refreshArgv",
            "initArgv",
            "lsArgv",
            "statusArgv",
        ]) {
            assert.ok(
                !code(text).includes(name),
                `${file} imports ${name}; §9 does not expose that route`,
            );
        }
    }
});

test("no tool is named after something §9 withholds, anywhere in the source", () => {
    for (const { file, text } of shipped()) {
        for (const m of code(text).matchAll(/\bkb_([a-z_]+)\b/g)) {
            const name = `kb_${m[1]}`;
            assert.ok(
                TOOL_NAMES.includes(name),
                `${file} names a tool called ${name}, which is not one of §9's six`,
            );
            for (const forbidden of NOT_EXPOSED) {
                assert.ok(!name.includes(forbidden), `${file} names ${name}`);
            }
        }
    }
});

/* ----------------------------------------------------------- the packaging */

test("the build outputs are ignored by the repository, the way the siblings' are", () => {
    const ignore = fs.readFileSync(path.join(REPO, ".gitignore"), "utf8");
    for (const line of ["packages/kb-mcp/out/", "node_modules/"]) {
        assert.ok(
            ignore.split("\n").includes(line),
            `.gitignore does not carry ${line}, so a build would be committed`,
        );
    }
});

test("the entry point is executable and starts the compiled server", () => {
    const entry = path.join(ROOT, "bin", "kb-mcp");
    assert.ok(fs.existsSync(entry));
    assert.ok((fs.statSync(entry).mode & 0o111) !== 0, "bin/kb-mcp is not executable");
    const text = fs.readFileSync(entry, "utf8");
    assert.match(text, /^#!/, "bin/kb-mcp has no shebang, so a host cannot exec it");
    assert.match(text, /require\("\.\.\/out\/main\.js"\)/);
});

test("the sibling reader is reachable, and is the checkout rather than a copy", () => {
    /* A copy would mean rebuilding `kb-js` silently does not reach this
     * process, and the two halves of one repository would drift between
     * commits while every test here still passed. Resolved the way Node will
     * resolve it, so the answer does not depend on where npm put the link. */
    const resolved = fs.realpathSync(require.resolve("kb-js", { paths: [ROOT] }));
    const checkout = fs.realpathSync(path.resolve(ROOT, "..", "kb-js"));
    assert.ok(
        resolved.startsWith(checkout + path.sep),
        `kb-js resolves to ${resolved}, not the checkout; run npm install at the repository root`,
    );
});
