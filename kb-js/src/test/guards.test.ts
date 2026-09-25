/* The checks that are about the whole package rather than about one function.
 *
 * Each of these is a mutation that would pass every other test in this
 * directory: a `exec` that takes a command line instead of an argv, a
 * `shell: true` on the spawn, a third-party dependency, a `vscode` import in a
 * package that is supposed to be loadable anywhere. They are source scans
 * because the failure is a call that must never appear, not a behaviour that
 * can be driven.
 */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import { test } from "node:test";

import { CLI_ERROR_CODES, SPEC_ERROR_CODES } from "../errors";

const ROOT = path.resolve(__dirname, "..", "..");

/* The files whose job is to look for the things below, and which therefore
 * have to be able to spell them. Named rather than pattern-matched: the whole
 * value of these checks is that the set of places a forbidden token may appear
 * is small enough to read. */
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
    out.push({
        file: "package.json",
        text: fs.readFileSync(path.join(ROOT, "package.json"), "utf8"),
    });
    return out;
}

/** Comments stripped, because a file's prose has to be able to name what it refuses. */
function code(text: string): string {
    return text.replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, "");
}

/* ------------------------------------------------- no shell, ever */

test("nothing in this package hands a command line to a shell", () => {
    /* THE CLAIM THE WHOLE PACKAGE MAKES. `exec` and `execSync` take a STRING
     * and run it through `/bin/sh`; `spawn` and `execFile` take an argv and
     * run the binary. The difference is whether a query a reader typed can
     * become syntax, and it is one character of API surface apart — which is
     * why this is a scan and not a comment. */
    for (const { file, text } of sources()) {
        if (isChecker(file) || !file.startsWith("src")) {
            continue;
        }
        const c = code(text);
        for (const forbidden of ["exec(", "execSync(", "execFileSync("]) {
            assert.ok(
                !c.includes(forbidden),
                `${file} calls ${forbidden}, which takes a command line; spawn with an argv array is the only way a value a reader typed cannot become syntax`,
            );
        }
        assert.ok(
            !/shell\s*:\s*true/.test(c),
            `${file} sets shell: true, which puts /bin/sh between the argv and the binary and undoes every guarantee argv.ts makes`,
        );
    }
});

test("the spawn says shell: false out loud rather than relying on the default", () => {
    /* A default is a thing somebody changes. The one call site states it, so
     * the check above has something to find and a reader has something to
     * read. */
    const run = fs.readFileSync(path.join(ROOT, "src", "run.ts"), "utf8");
    assert.match(
        code(run),
        /shell:\s*false/,
        "run.ts no longer states shell: false, so nothing says out loud that there is no shell here",
    );
});

test("nothing is spawned from an assembled string, and the argv is an array", () => {
    /* `` spawn(`kb search ${q}`) `` is the shape this package exists not to
     * have. It is not caught by the ban on `exec` above — `spawn` with one
     * string takes the WHOLE thing as the executable's name, so the failure is
     * an ENOENT naming a path with the reader's query in it rather than a
     * shell injection — but it is the same mistake and it is one edit away
     * from becoming the dangerous one.
     *
     * Stated at the CALL rather than over every backtick in the package: a
     * blanket scan fails on `kb is not on the PATH (looked for "${bin}")`,
     * which is a sentence for a person and not a command for a shell, and a
     * guard that fails on prose teaches whoever hits it to loosen the guard. */
    for (const { file, text } of sources()) {
        if (isChecker(file) || !file.startsWith("src")) {
            continue;
        }
        assert.ok(
            !/\b(spawn|spawnSync|execFile|execFileSync|exec|execSync)\s*\(\s*`/.test(code(text)),
            `${file} spawns from a template literal; the executable is a name and the arguments are an array`,
        );
    }
    /* And the one call site hands over an array. A second argument that were a
     * string would be an argv of one character per element. */
    const run = code(fs.readFileSync(path.join(ROOT, "src", "run.ts"), "utf8"));
    assert.match(
        run,
        /spawn\(\s*bin\s*,\s*\[\s*\.\.\.argv\s*\]/,
        "run.ts no longer spawns the binary with the argv as an array",
    );
});

test("the argv builders put a value in its own element and never beside a flag", () => {
    /* The mutation this catches compiles, runs, and sends
     * `--collection win32-iocp` as ONE argument, which the CLI reads as an
     * unknown flag and refuses — or worse, as a collection literally named
     * "win32-iocp" with a leading flag. Stated over the source because the
     * behavioural tests in argv.test.ts check the outputs and this checks that
     * there is no other way to produce one. */
    const argv = code(fs.readFileSync(path.join(ROOT, "src", "argv.ts"), "utf8"));
    assert.ok(
        !/push\(\s*`/.test(argv),
        "argv.ts pushes a template literal, which is how a flag and its value become one argument",
    );
    assert.ok(
        !/push\([^)]*\+[^)]*\)/.test(argv),
        "argv.ts concatenates into an element",
    );
});

/* -------------------------------------------------- the dependency rule */

test("this package has no runtime dependencies at all", () => {
    /* §10 makes this a wrapper over a binary. A wrapper that pulled in a
     * dependency tree would be a bigger thing to audit than the thing it
     * wraps, and every consumer of it — `index-vscode` among them — inherits
     * whatever lands here. */
    const pkg = JSON.parse(fs.readFileSync(path.join(ROOT, "package.json"), "utf8")) as {
        dependencies?: Record<string, string>;
    };
    assert.deepEqual(Object.keys(pkg.dependencies ?? {}), []);
});

test("nothing here imports vscode, or anything that is not Node's own", () => {
    /* This package is loadable from a test run, from an extension host, and
     * from a script. An import of the extension-host API would make it one of
     * those and not the others. */
    for (const { file, text } of sources()) {
        if (!file.startsWith("src")) {
            continue;
        }
        for (const m of code(text).matchAll(/\bfrom\s+["']([^"']+)["']/g)) {
            const spec = m[1];
            assert.ok(
                spec.startsWith(".") || spec.startsWith("node:"),
                `${file} imports "${spec}"; this package depends on Node and on nothing else`,
            );
        }
    }
});

/* ------------------------------------------------------ §11's vocabulary */

test("§11's table is transcribed whole, and the CLI's own codes are kept apart from it", () => {
    /* The two lists answer different questions — "what does the API refuse"
     * and "what does this command line refuse" — and a caller switching on §11
     * must not have to guess which of them it is looking at. Folding them
     * together would make `usage` look like part of the API's contract. */
    assert.deepEqual(
        [...SPEC_ERROR_CODES],
        [
            "not_found",
            "model_mismatch",
            "model_missing",
            "index_stale",
            "unsupported_mime",
            "fetch_failed",
            "store_locked",
            "collection_in_use",
        ],
    );
    for (const cli of CLI_ERROR_CODES) {
        assert.ok(
            !(SPEC_ERROR_CODES as readonly string[]).includes(cli),
            `${cli} is in both lists, so one of them is wrong about what it contains`,
        );
    }
});

test("every code the CLI can actually print is one of the two lists", () => {
    /* Read out of the C sources rather than from a list here, so a command
     * that starts emitting a new code fails this instead of silently arriving
     * as `unrecognised` in somebody's UI.
     *
     * Skipped when the CLI is not beside this package — a consumer installing
     * `kb-js` alone has no C to read — because a test that cannot run is worth
     * more as a skip than as a failure nobody can act on. */
    const cli = path.resolve(ROOT, "..", "kb-cli", "src");
    if (!fs.existsSync(cli)) {
        return;
    }
    const known = new Set<string>([...SPEC_ERROR_CODES, ...CLI_ERROR_CODES]);
    const found = new Set<string>();
    for (const name of fs.readdirSync(cli)) {
        if (!name.endsWith(".c")) {
            continue;
        }
        const text = fs.readFileSync(path.join(cli, name), "utf8");
        /* `err_out(json, "<code>", ...)` and `*code = "<code>"` are the two
         * ways a code is chosen in that tree. */
        for (const m of text.matchAll(/err_out\([^,]+,\s*"([a-z_]+)"/g)) {
            found.add(m[1]);
        }
        for (const m of text.matchAll(/\*code\s*=\s*"([a-z_]+)"/g)) {
            found.add(m[1]);
        }
    }
    assert.ok(found.size > 3, `only ${found.size} codes were found in kb-cli; the scan is broken`);
    assert.deepEqual(
        [...found].filter((c) => !known.has(c)).sort(),
        [],
        "kb-cli prints a code this package has never heard of, so it would reach a caller flagged unrecognised",
    );
});
