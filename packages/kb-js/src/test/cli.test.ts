/* Against the real binary.
 *
 * WHAT THIS ADDS OVER THE FAKE. The fake proves what this package does with an
 * answer; this proves the questions are ones the CLI actually accepts. Every
 * flag spelling in `argv.ts` is a CLAIM about a program in another language,
 * and a claim that is only checked against a fixture is a claim that can be
 * wrong for as long as nobody runs the real thing.
 *
 * IT NEVER TOUCHES A STORE IT WAS NOT ASKED TO. There is one store, the first
 * `.kb/` found by walking up from the working directory, and every test that
 * runs the binary runs it inside a throwaway directory where `kb init` has
 * just made one — so the store it finds is the temporary one, and it exists
 * only for the length of the run. The CLI reads no `KB_STORE`, so there is
 * no second place for a write to land: a test that wrote into somebody's own
 * `.kb/` would be a test that files their research under `win32-iocp` every
 * time it ran.
 *
 * NOR THE DEVELOPER'S HOME. The CLI reads `HOME` to find `~/.kb/models`
 * (§8), so every run here gets a throwaway `HOME` (`ENV` below): a model
 * installed on the machine must not change what these tests see, and nothing
 * they do may reach the real home directory.
 *
 * IT SKIPS RATHER THAN FAILS WHEN THE BINARY IS NOT BUILT. `kb-js` is
 * installable on its own, and a suite that could not pass without a C
 * toolchain beside it would be a suite people stop running.
 */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { execFileSync } from "node:child_process";
import { test } from "node:test";

import { Kb } from "../client";
import { KbError, isKbError } from "../errors";
import { obj } from "../shape";
import { addArgv, addBatchArgv, addDirArgv, embedArgv, batchLines, deleteCollectionArgv, forgetArgv, lsArgv, refreshArgv, refreshSourceArgv, searchArgv, sourceArgv, sourcesArgv } from "../argv";
import { cliBin, noCli } from "./cli-bin";

const BIN = cliBin("kb");

/* One throwaway home for every kb this file starts. */
const HOME = fs.mkdtempSync(path.join(os.tmpdir(), "kb-js-home-"));
process.on("exit", () => fs.rmSync(HOME, { recursive: true, force: true }));
const ENV: NodeJS.ProcessEnv = { ...process.env, HOME };

function built(): boolean {
    return BIN !== "";
}

/* `kb --help`, which is the CLI's own statement of its surface. */
function help(): string {
    return execFileSync(BIN, ["--help"], { encoding: "utf8" });
}

interface Work {
    dir: string;
    kb: Kb;
    /* The same environment the client was given, so a test that runs the
     * binary directly sees exactly what the client's child saw. */
    env: NodeJS.ProcessEnv;
    dispose(): void;
}

function workspace(): Work {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kb-js-real-"));
    const project = path.join(dir, "project");
    fs.mkdirSync(project);
    /* The working directory is inside the throwaway, and that is the whole
     * of the store selection: §1.4 walks up from it and finds the `.kb/` that
     * `kb.init()` creates there. */
    const kb = new Kb({ bin: BIN, cwd: project, env: { ...ENV } });
    return {
        dir: project,
        kb,
        env: { ...ENV },
        dispose: () => fs.rmSync(dir, { recursive: true, force: true }),
    };
}

test("every flag this package spells for an implemented command is one the CLI names", async (t) => {
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const text = help();
    /* The commands that exist today. `search` and `chunk` are checked
     * separately, below, and only once they turn up — so the day they land,
     * this suite starts checking their flags without anybody editing it. */
    const argvs = [
        lsArgv({ collection: "c", source: "S-1", mime: "m", since: "s", limit: 1, after: "D-1", reverse: true }),
        ["get", "D-1", "--include", "text,chunks"],
        ["collections"],
        ["status"],
        ["init"],
        ["add", "--title", "t", "--collection", "c", "--url", "u", "--mime", "m", "--meta", "{}", "--file", "-"],
        addArgv({ title: "t", collection: "c", embedBudget: 0 }),
        addArgv({ title: "t", collection: "c", wait: true }),
        addBatchArgv({ embedBudget: 5 }),
        addDirArgv("d", { collection: "c", wait: true }),
        embedArgv(),
        forgetArgv("D-1"),
        addBatchArgv(),
        addDirArgv("d", { collection: "c", forget: false }),
        deleteCollectionArgv("c", true),
        sourcesArgv({ collection: "c", kind: "file" }),
        sourceArgv("S-1"),
        refreshSourceArgv("S-1"),
    ];
    for (const argv of argvs) {
        assert.ok(text.includes(`  ${argv[0]} `) || text.includes(`  ${argv[0]}\n`), `kb --help does not name the ${argv[0]} command`);
        for (const element of argv) {
            if (!element.startsWith("--")) {
                continue;
            }
            assert.ok(
                text.includes(element),
                `kb --help does not name ${element}, which ${argv[0]} is being given`,
            );
        }
    }
    /* And `--json`, which `run.ts` appends to every one of them. */
    assert.ok(text.includes("--json"));
    /* And not `--store`: the tier selector is gone, and a help text that
     * still named it would be advertising a flag every command refuses. */
    assert.equal(text.includes("--store"), false, "kb --help still names --store");
});

test("search's flags are checked the moment the command exists", async (t) => {
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const text = help();
    if (!/^\s{2}search\b/m.test(text)) {
        t.skip("kb search is not implemented yet; argv.ts's spellings are read off index-api.md §4");
        return;
    }
    for (const element of searchArgv("q", {
        collection: ["a"],
        mode: "hybrid",
        k: 10,
        expand: 1,
        source: "S-1",
        mime: "m",
        since: "s",
        minScore: 0.2,
        rerank: true,
    })) {
        if (element.startsWith("--") && element !== "--") {
            assert.ok(text.includes(element), `kb search does not take ${element}`);
        }
    }
});

test("a store filed into and read back through this package", async (t) => {
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const work = workspace();
    try {
        const created = await work.kb.init();
        assert.equal(
            fs.realpathSync(created.path),
            fs.realpathSync(path.join(work.dir, ".kb")),
            "init did not create the store in the working directory",
        );

        const added = await work.kb.add("# IOCP\n\nCreateIoCompletionPort binds a handle.\n", {
            title: "I/O Completion Ports",
            collection: "win32-iocp",
            url: "https://learn.microsoft.test/win32/iocp",
            meta: { authors: ["MSDN"], year: 2026 },
        });
        assert.equal(added.created, true);
        assert.match(added.document, /^D-\d+$/);

        /* Every field `types.ts` claims a listed document has, off the real
         * binary: the CLI's row is wider than §1.2's `Document` and the reader
         * is built for the wider one. */
        const [row] = await work.kb.ls();
        assert.equal(row.id, added.document);
        assert.equal(row.collection, "win32-iocp");
        assert.equal(row.locator, "https://learn.microsoft.test/win32/iocp");
        /* `.../win32/iocp` has no extension to guess from, so `kb add` reads
         * the text itself: it opens with a heading, so it is Markdown and §3.2
         * of the UI spec renders it as such rather than as a wall of text with
         * its own hashes in it. */
        assert.equal(row.mime, "text/markdown");
        assert.ok(row.bytes > 0);
        assert.ok(row.chunkCount >= 1);
        assert.deepEqual(row.meta, { authors: ["MSDN"], year: 2026 });
        assert.ok(row.fetchedAt.length > 0, "the row carries no fetch date, so §5's badge has nothing to read");

        /* An explicit mime wins over the guess, which is the path a surface
         * that knows the document's type has to take. */
        const typed = await work.kb.add("# Ring\n", {
            title: "io_uring",
            collection: "io-uring",
            url: "https://kernel.test/io_uring",
            mime: "text/plain",
        });
        const typedRow = (await work.kb.ls({ collection: "io-uring" }))[0];
        assert.equal(typedRow.id, typed.document);
        assert.equal(typedRow.mime, "text/plain");

        /* And the filters, which are the argv this package builds. */
        assert.equal((await work.kb.ls({ collection: "win32-iocp" })).length, 1);
        assert.equal((await work.kb.ls({ collection: "nothing-here" })).length, 0);
        assert.equal((await work.kb.ls({ mime: "text/markdown" })).length, 1);
        assert.equal((await work.kb.ls({ mime: "text/plain" })).length, 1);
        assert.equal((await work.kb.ls({ source: row.source })).length, 1);
        assert.equal((await work.kb.ls({ limit: 1 })).length, 1);
        assert.equal((await work.kb.ls()).length, 2);

        const read = await work.kb.get(row.id, { text: true, chunks: true });
        assert.equal(read.document.id, row.id);
        assert.match(read.text ?? "", /CreateIoCompletionPort/);
        assert.ok((read.chunks ?? []).length >= 1);
        assert.match(read.chunks?.[0].id ?? "", /^C-\d+$/);
        assert.equal(read.chunks?.[0].document, row.id);
        assert.ok((read.chunks?.[0].span.end ?? 0) > 0);

        /* A read that did not ask for them has neither. */
        const bare = await work.kb.get(row.id);
        assert.equal("text" in bare, false);
        assert.equal("chunks" in bare, false);

        const collections = await work.kb.collections();
        const iocp = collections.find((c) => c.name === "win32-iocp");
        assert.ok(iocp !== undefined);
        assert.equal(iocp.documents, 1);

        const status = await work.kb.status();
        assert.equal(status.present, true);
        assert.equal(fs.realpathSync(status.path ?? ""), fs.realpathSync(created.path));
        assert.equal(status.documents, 2);
        assert.equal(status.chunking?.current, true);
    } finally {
        work.dispose();
    }
});

test("a real refusal arrives as a KbError carrying §11's code", async (t) => {
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const work = workspace();
    try {
        await work.kb.init();
        const e = (await work.kb.get("D-9999").catch((x: unknown) => x)) as KbError;
        assert.ok(e instanceof KbError, `expected a KbError, got ${String(e)}`);
        assert.equal(e.code, "not_found");
        assert.equal(e.spec, "not_found");
        assert.equal(e.unrecognised, false);
    } finally {
        work.dispose();
    }
});

test("a query full of shell metacharacters reaches the store as one argument", async (t) => {
    /* The claim, made against the real binary rather than against the fake.
     * The store refuses the id — `kb get` wants `D-<n>` — and the reason it
     * gives back is the WHOLE string, which is only possible if the whole
     * string arrived as one argument. A shell would have eaten the semicolon
     * and everything after it. */
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const work = workspace();
    try {
        await work.kb.init();
        const hostile = 'D-1; touch /tmp/kb-js-should-not-exist && echo "$(id)"';
        const e = (await work.kb.get(hostile).catch((x: unknown) => x)) as KbError;
        assert.ok(e instanceof KbError);
        assert.ok(
            e.message.includes(hostile),
            `the store did not see the whole argument; it said: ${e.message}`,
        );
        assert.equal(
            fs.existsSync("/tmp/kb-js-should-not-exist"),
            false,
            "a shell ran the second half of the argument",
        );
    } finally {
        work.dispose();
    }
});

test("a collection name with a space survives the argument list", async (t) => {
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const work = workspace();
    try {
        await work.kb.init();
        await work.kb.add("text\n", { title: "A title with spaces", collection: "two words" });
        const [row] = await work.kb.ls({ collection: "two words" });
        assert.equal(row.collection, "two words");
        assert.equal(row.title, "A title with spaces");
    } finally {
        work.dispose();
    }
});

test("outside any store, a command is refused and says how to make one", async (t) => {
    /* NO FALLBACK. With the global tier gone there is nowhere else to look:
     * a directory with no `.kb/` at or above it is `not_found`, and the
     * message names `kb init` because that is the one thing the reader can do
     * about it. `status` is the exception, and answers "none" rather than
     * refusing, because "is there a store here" is its whole question. */
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const work = workspace();
    try {
        const e = (await work.kb.ls().catch((x: unknown) => x)) as KbError;
        assert.ok(e instanceof KbError, `expected a KbError, got ${String(e)}`);
        assert.equal(e.code, "not_found");
        assert.match(e.message, /kb init/);

        const status = await work.kb.status();
        assert.equal(status.path, null);
        assert.equal(status.present, false);
        assert.equal(status.documents, undefined);
        assert.equal(typeof status.olderThan, "string");
    } finally {
        work.dispose();
    }
});

test("--store is refused as an unknown option on every command", async (t) => {
    /* The flag every builder used to be able to emit, and the one
     * `argv.test.ts` now checks none of them does. Checked here against the
     * binary so that the reason is the CLI's and not this package's opinion:
     * a `kb` that quietly accepted and ignored it would let a caller believe
     * it had narrowed a read that it had not. */
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const work = workspace();
    try {
        await work.kb.init();
        for (const argv of [
            ["ls", "--store", "project"],
            ["status", "--store", "all"],
            ["collections", "--store", "global"],
            ["search", "--store", "all", "q"],
        ]) {
            let refused: { status: number | null; stdout: string } | null = null;
            try {
                execFileSync(BIN, [...argv, "--json"], { cwd: work.dir, env: work.env, encoding: "utf8" });
            } catch (x) {
                const err = x as { status: number | null; stdout: string };
                refused = { status: err.status, stdout: err.stdout };
            }
            assert.ok(refused !== null, `kb ${argv.join(" ")} was accepted`);
            assert.equal(refused.status, 1, `kb ${argv.join(" ")} did not exit 1`);
            const payload = JSON.parse(refused.stdout) as { ok: boolean; error: string; message: string };
            assert.equal(payload.ok, false);
            assert.equal(payload.error, "usage");
            assert.match(payload.message, /unknown option/);
        }
    } finally {
        work.dispose();
    }
});

/* ------------------------------------------- the readers against the binary
 *
 * THE FAILURE THIS FILE DID NOT CATCH, AND WHY. `Kb.refresh` read `refreshed`
 * and `changed` off the payload for as long as `kb refresh` did not exist.
 * When it landed it printed neither, so both read as 0 — and 0 is the right
 * number for a command that refetches nothing, so every fixture-driven test
 * agreed with the reader and the reader was wrong. A hand-written fixture
 * agrees with whoever wrote it. Only the binary disagrees.
 *
 * So these compare the READER'S OWN KEYS against the BINARY'S OWN KEYS, in
 * both directions and without a third list in the middle:
 *
 *   - a key the store prints that the reader does not answer is a fact
 *     arriving at a caller as nothing;
 *   - a key the reader answers that the store does not print is a field with
 *     no source, which is the bug above.
 *
 * Where a reader deliberately reshapes, the keys it drops are named with a
 * reason. That list is the only hand-written part and it is small on purpose:
 * adding to it is a visible act, and a key that turns up later still fails.
 */

function raw(work: Work, argv: readonly string[], input?: string): Record<string, unknown> {
    const out = execFileSync(BIN, [...argv, "--json"], {
        cwd: work.dir,
        env: work.env,
        encoding: "utf8",
        input,
    });
    return JSON.parse(out) as Record<string, unknown>;
}

/* `ok` is the envelope rather than the answer, and `run.ts` deliberately does
 * not strip it — it is not a field any reader claims. */
function printed(payload: Record<string, unknown>): string[] {
    return Object.keys(payload).filter((k) => k !== "ok").sort();
}

function answered(value: object): string[] {
    return Object.keys(value).sort();
}

test("the refresh reader answers exactly the keys the real binary prints", async (t) => {
    /* THE REGRESSION TEST FOR THE BUG ITSELF. `refreshed` and `changed` would
     * appear on the right and nowhere on the left. */
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const work = workspace();
    try {
        await work.kb.init();
        await work.kb.add("# IOCP\n", { title: "IOCP", collection: "win32-iocp" });
        assert.deepEqual(
            answered(await work.kb.refresh({ olderThan: "1d" })),
            printed(raw(work, ["refresh", "--older-than", "1d"])),
        );
        /* And the field that carries the whole meaning of the answer: §12.2
         * kept an HTTP client out of the binary, so this reports and does not
         * act, and a surface that cannot see `note` will say it refreshed. */
        const report = await work.kb.refresh({ olderThan: "1d" });
        assert.equal(report.action, "report");
        assert.match(report.note, /report, not an action/);
        assert.equal(report.refetched, 0);
        assert.equal(report.reembedded, 0);
        assert.equal(report.olderThan, "1d");
    } finally {
        work.dispose();
    }
});

test("refresh takes the two narrowings §5 gives it, and no invented ones", async (t) => {
    /* `--document` and `--source` were sent here and `kb refresh` refuses
     * both. §3.1's per-document action is §2's `POST /sources/{id}/refresh`,
     * a different route; a filter on this one would report about a source
     * while looking like it had refetched it. */
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    assert.deepEqual(refreshArgv(), ["refresh"]);
    assert.deepEqual(
        refreshArgv({ collection: "win32-iocp", olderThan: "90d" }),
        ["refresh", "--collection", "win32-iocp", "--older-than", "90d"],
    );
    for (const flag of refreshArgv({ collection: "c", olderThan: "1d" })) {
        if (flag.startsWith("--")) {
            assert.ok(help().includes(flag), `kb refresh does not take ${flag}`);
        }
    }
    const text = help();
    assert.equal(/refresh[^\n]*--document/.test(text), false);
    assert.equal(/refresh[^\n]*--source\b/.test(text), false);
});

test("forgetting through this package: a document, a source, and a collection with its documents", async (t) => {
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const work = workspace();
    try {
        await work.kb.init();
        const a = await work.kb.add("zzone\n", { title: "one", collection: "keep" });
        const b = await work.kb.add("zztwo\n", { title: "two", collection: "keep" });
        const gone = await work.kb.forget(a.document);
        assert.deepEqual(gone.documents, [a.document]);
        assert.deepEqual(gone.sources, []);
        assert.match(gone.note, /compact/);
        assert.equal((await work.kb.search("zzone")).count, 0);

        const bySource = await work.kb.forget(b.source);
        assert.deepEqual(bySource.documents, [b.document]);
        assert.deepEqual(bySource.sources, [b.source]);

        await work.kb.add("zzthree\n", { title: "three", collection: "topic" });
        await assert.rejects(
            work.kb.deleteCollection("topic"),
            (e: unknown) => isKbError(e) && e.detailsOf("collection_in_use")?.documents === 1,
        );
        await work.kb.deleteCollection("topic", { withDocuments: true });
        assert.deepEqual(await work.kb.collections(), []);
        await assert.rejects(work.kb.forget(a.document), (e: unknown) => isKbError(e) && e.code === "not_found");
    } finally {
        work.dispose();
    }
});

test("sources through this package: listed, shown with their history, and a file source refreshed", async (t) => {
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const work = workspace();
    try {
        await work.kb.init();
        const file = path.join(work.dir, "notes.md");
        fs.writeFileSync(file, "# Notes\n\nzzold\n");
        const filed = await work.kb.add(fs.readFileSync(file, "utf8"), { title: "notes", collection: "research" });
        // Filed from the file itself, so its source is a `file` one.
        execFileSync(BIN, ["add", "--title", "notes", "--collection", "research", "--file", file, "--json"], {
            cwd: work.dir,
            env: work.env,
        });
        const sources = await work.kb.sources({ kind: "file" });
        assert.equal(sources.length, 1);
        const src = sources[0];
        assert.equal(src.kind, "file");
        assert.equal(src.docCount, 1);
        assert.deepEqual((await work.kb.sources({ collection: "research" })).length, 2);
        assert.equal(src.status, "ok");
        assert.equal(src.etag, null);

        const same = await work.kb.refreshSource(src.id);
        assert.equal(same.changed, false);
        fs.writeFileSync(file, "# Notes\n\nzznew\n");
        const changed = await work.kb.refreshSource(src.id);
        assert.equal(changed.changed, true);
        assert.equal((await work.kb.search("zznew")).count, 1);

        const shown = await work.kb.source(src.id);
        assert.equal(shown.source.id, src.id);
        assert.equal(shown.documents.length, 1);
        assert.deepEqual(shown.history.map((h) => h.changed), [true, false, true]);

        await assert.rejects(work.kb.refreshSource(filed.source), (e: unknown) => isKbError(e) && e.code === "usage");
    } finally {
        work.dispose();
    }
});

test("a batch through this package: filed together, and refused together", async (t) => {
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const work = workspace();
    try {
        await work.kb.init();
        const docs = [
            { title: "one", collection: "b", content: "zzone" },
            { title: "two", collection: "b", content: "# Two\n\nzztwo", mime: "text/markdown", meta: { year: 2026 } },
        ];
        const batch = await work.kb.addBatch(docs);
        const added = batch.added;
        assert.deepEqual(added.map((a) => a.created), [true, true]);
        assert.equal(added[1].splitter, "markdown");
        assert.equal(batch.count, 2);
        assert.equal(batch.pending, 0, "a store with no model has nothing pending");
        assert.equal((await work.kb.search("zztwo")).count, 1);

        // Each row carries exactly what the binary printed on it.
        const printedRows = (
            JSON.parse(
                execFileSync(BIN, ["add", "--batch", "--json"], {
                    cwd: work.dir,
                    env: work.env,
                    input: batchLines(docs),
                    encoding: "utf8",
                }),
            ) as { added: Record<string, unknown>[] }
        ).added;
        assert.deepEqual(answered(added[0]), Object.keys(printedRows[0]).sort());
        assert.deepEqual(
            answered(await work.kb.addBatch(docs, { embedBudget: 0 })),
            printed(raw(work, ["add", "--batch", "--embed-budget", "0"], batchLines(docs))),
        );

        const before = fs.readFileSync(path.join(work.dir, ".kb", "documents.jsonl"), "utf8");
        await assert.rejects(
            work.kb.addBatch([{ title: "fine", collection: "b", content: "zzfine" }, { title: "", collection: "b", content: "x" }]),
            (e: unknown) => isKbError(e) && e.code === "usage" && /batch line 2/.test(e.message),
        );
        assert.equal(fs.readFileSync(path.join(work.dir, ".kb", "documents.jsonl"), "utf8"), before, "a refused batch wrote nothing");
    } finally {
        work.dispose();
    }
});

test("a folder through this package: filed, filed again, and a file gone kept or forgotten as asked", async (t) => {
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const work = workspace();
    /* Beside the project rather than inside it, so the store's own `.kb/` is
     * not part of the walk; `dispose` removes the throwaway it sits in. */
    const tree = path.join(work.dir, "..", "tree");
    try {
        await work.kb.init();
        fs.mkdirSync(path.join(tree, "lib"), { recursive: true });
        fs.mkdirSync(path.join(tree, "node_modules", "dep"), { recursive: true });
        fs.writeFileSync(path.join(tree, "README.md"), "# Tree\n\nzzreadme\n");
        fs.writeFileSync(path.join(tree, "lib", "ring.c"), "int zzring(void) { return 1; }\n");
        fs.writeFileSync(path.join(tree, "node_modules", "dep", "index.js"), "module.exports = 1;\n");
        fs.writeFileSync(path.join(tree, ".hidden.md"), "# Hidden\n");

        const first = await work.kb.addDir(tree, { collection: "code" });
        /* The locator is the folder with its links resolved, which on macOS
         * is /private/var rather than the /var that os.tmpdir() answers. */
        assert.equal(first.root, fs.realpathSync(tree));
        assert.match(first.source ?? "", /^S-\d+$/);
        assert.equal(first.collection, "code");
        assert.equal(first.files, 2);
        assert.equal(first.added, 2);
        assert.equal(first.skipped.vendored, 1);
        assert.equal(first.skipped.hidden, 1);
        assert.equal(first.pending, 0, "a store with no model has nothing pending");
        assert.equal((await work.kb.search("zzring")).count, 1);
        const shown = await work.kb.source(first.source ?? "");
        assert.equal(shown.source.kind, "dir");
        assert.deepEqual(shown.documents.map((d) => d.path).sort(), ["README.md", "lib/ring.c"]);
        assert.deepEqual(shown.documents.map((d) => d.title).sort(), ["tree/README.md", "tree/lib/ring.c"]);

        const again = await work.kb.addDir(tree, { collection: "code" });
        assert.equal(again.source, first.source);
        assert.equal(again.unchanged, 2);
        assert.equal(again.added + again.updated, 0);

        /* A file changed and a file gone, with forgetting off: the gone one is
         * still in the store and is named by its path. */
        fs.writeFileSync(path.join(tree, "README.md"), "# Tree\n\nzzchanged\n");
        fs.rmSync(path.join(tree, "lib", "ring.c"));
        const kept = await work.kb.addDir(tree, { collection: "code", forget: false });
        assert.equal(kept.updated, 1);
        assert.deepEqual([...kept.missing], ["lib/ring.c"]);
        assert.deepEqual([...kept.forgotten], []);
        assert.equal((await work.kb.search("zzring")).count, 1);

        const ring = shown.documents.find((d) => d.path === "lib/ring.c")?.id;
        const forgot = await work.kb.addDir(tree, { collection: "code" });
        assert.deepEqual([...forgot.forgotten], [ring]);
        assert.deepEqual([...forgot.missing], []);
        assert.equal((await work.kb.search("zzring")).count, 0);

        // The reader answers exactly the keys the binary prints.
        assert.deepEqual(
            answered(forgot),
            printed(raw(work, ["add", "--dir", tree, "--collection", "code", "--no-forget"])),
        );
        assert.deepEqual(answered(forgot.skipped), Object.keys(obj(raw(work, ["add", "--dir", tree, "--collection", "code"])["skipped"])).sort());

        await assert.rejects(
            work.kb.addDir(path.join(tree, "README.md"), { collection: "code" }),
            (e: unknown) => isKbError(e) && e.code === "not_found",
        );
    } finally {
        work.dispose();
    }
});

test("every reader answers exactly the keys the real binary prints", async (t) => {
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const work = workspace();
    try {
        await work.kb.init();
        const added = await work.kb.add("# IOCP\n\nCreateIoCompletionPort binds a handle.\n", {
            title: "IOCP",
            collection: "win32-iocp",
            mime: "text/markdown",
        });
        const second = await work.kb.add("io_uring_setup\n", {
            title: "Ring",
            collection: "io-uring",
        });
        const chunk = `C-${added.chunkBase}`;

        /* Each row: the command, the reader's answer, and the keys the reader
         * drops on purpose with the reason it drops them. */
        const cases: {
            what: string;
            argv: string[];
            read: object;
            dropped?: Record<string, string>;
            /* For a command that writes: the keys its answer carries, taken
             * from a run of its own rather than by running it again. */
            printed?: string[];
        }[] = [
            { what: "add", argv: [], read: added },
            { what: "stale", argv: ["stale", "--older-than", "1d"], read: await work.kb.stale({ olderThan: "1d" }) },
            {
                what: "links",
                argv: ["links", added.document],
                read: await work.kb.links(added.document),
            },
            {
                what: "search",
                argv: ["search", "CreateIoCompletionPort"],
                read: await work.kb.search("CreateIoCompletionPort"),
            },
            {
                what: "chunk",
                argv: ["chunk", chunk],
                read: await work.kb.chunk(chunk),
            },
            {
                what: "get",
                argv: ["get", added.document],
                read: await work.kb.get(added.document),
            },
            {
                what: "sources show",
                argv: ["sources", "show", added.source],
                read: await work.kb.source(added.source),
            },
            {
                /* Every include at once, on a document that has an edge:
                 * the plain `get` above never asks for links, which is how a
                 * reader that dropped them went unnoticed. */
                what: "get with every include",
                argv: ["get", added.document, "--include", "text,chunks,links"],
                read: await (async () => {
                    await work.kb.link(added.document, "see_also", second.document);
                    return work.kb.get(added.document, { text: true, chunks: true, links: true });
                })(),
            },
            await (async () => {
                /* Forgetting writes, like add, so it cannot be run twice on
                 * one document: the binary's own answer is taken from
                 * forgetting a document filed for the purpose, and the
                 * reader's from forgetting the second one, which nothing
                 * above reads afterwards. */
                const spare = await work.kb.add("zzspare\n", { title: "spare", collection: "spare" });
                return {
                    what: "forget",
                    argv: [],
                    printed: printed(raw(work, ["forget", spare.document])),
                    read: await work.kb.forget(second.document),
                };
            })(),
            {
                what: "stats",
                argv: ["stats"],
                read: await work.kb.stats(),
                dropped: {
                    count: "the number of collections, which is the array's own length",
                },
            },
        ];

        for (const c of cases) {
            /* `add` writes, so its payload is the one already in hand rather
             * than a second ingest. */
            const keys =
                c.what === "add"
                    ? answered(added)
                    : c.printed !== undefined
                      ? c.printed
                      : printed(raw(work, c.argv)).filter(
                          (k) => !Object.keys(c.dropped ?? {}).includes(k),
                      );
            assert.deepEqual(
                answered(c.read),
                keys,
                `the ${c.what} reader and kb ${c.what} do not agree about what the answer contains`,
            );
        }

        /* `collections` is the one reader that answers an ARRAY rather than a
         * record, so there is no key set to compare — the check there is that
         * each ROW carries what the store printed on it. */
        const rows = raw(work, ["collections"])["collections"] as Record<string, unknown>[];
        const read = await work.kb.collections();
        assert.equal(read.length, rows.length);
        for (let i = 0; i < rows.length; i++) {
            assert.deepEqual(
                answered(read[i]),
                Object.keys(rows[i]).sort(),
                "a collections row carries a key the reader does not answer",
            );
        }
        /* `sources` answers an array too: each row must carry exactly what
         * the store printed on it, `etag` and all once there is one. */
        const printedSources = raw(work, ["sources"])["sources"] as Record<string, unknown>[];
        const readSources = await work.kb.sources();
        assert.equal(readSources.length, printedSources.length);
        for (let i = 0; i < printedSources.length; i++) {
            assert.deepEqual(
                answered(readSources[i]),
                Object.keys(printedSources[i]).sort(),
                "a sources row carries a key the reader does not answer",
            );
        }
        assert.equal(second.created, true);
    } finally {
        work.dispose();
    }
});

test("the whole graph through this package: every edge, both ends, and a forgotten end marked", async (t) => {
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const work = workspace();
    try {
        await work.kb.init();
        const a = await work.kb.add("zzga\n", { title: "a", collection: "g" });
        const b = await work.kb.add("zzgb\n", { title: "b", collection: "g" });
        const c = await work.kb.add("zzgc\n", { title: "c", collection: "h" });
        assert.deepEqual(await work.kb.allLinks(), []);
        await work.kb.link(a.document, "cites", b.document);
        await work.kb.link(b.document, "analogue_of", c.document);
        assert.deepEqual(await work.kb.allLinks(), [
            { from: a.document, to: b.document, type: "cites", resolved: true },
            { from: b.document, to: c.document, type: "analogue_of", resolved: true },
        ]);
        await work.kb.forget(c.document);
        assert.equal((await work.kb.allLinks())[1].resolved, false);
    } finally {
        work.dispose();
    }
});

test("filing now and embedding later: pending on every add, vectors on status, and embed refused without a model", async (t) => {
    if (!built()) {
        t.skip(noCli("kb"));
        return;
    }
    const work = workspace();
    try {
        await work.kb.init();
        const filed = await work.kb.add("zzlater\n", { title: "later", collection: "l", embedBudget: 0 });
        /* The throwaway home holds no model, so nothing is ever pending: the
         * store is keyword-only and says so, rather than owing embeddings. */
        assert.equal(filed.pending, 0);
        assert.deepEqual(
            answered(filed),
            printed(raw(work, ["add", "--title", "again", "--collection", "l", "--file", "-", "--embed-budget", "0"], "zzagain\n")),
            "the single-add reader and kb add do not agree about what the answer contains",
        );
        assert.equal((await work.kb.add("zzwait\n", { title: "wait", collection: "l", wait: true })).pending, 0);
        await assert.rejects(
            work.kb.add("zzboth\n", { title: "both", collection: "l", wait: true, embedBudget: 1 }),
            (e: unknown) => isKbError(e) && e.code === "usage",
        );

        const status = await work.kb.status();
        assert.equal(status.vectors?.count, 0);
        assert.equal(status.vectors?.missing, status.chunks, "a keyword-only store has no vector for any chunk");
        assert.equal(status.vectors?.current, false);
        assert.equal(status.model?.available, null);

        const found = await work.kb.search("zzlater");
        assert.equal(found.count, 1);
        assert.equal("unembedded" in found, false, "a keyword search says nothing about vectors");

        const refused = (await work.kb.embed().catch((x: unknown) => x)) as KbError;
        assert.ok(refused instanceof KbError, `expected a KbError, got ${String(refused)}`);
        assert.equal(refused.code, "model_missing");
    } finally {
        work.dispose();
    }
});
