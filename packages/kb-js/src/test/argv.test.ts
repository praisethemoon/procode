/* The command lines, as pure values.
 *
 * Every assertion here is a CLAIM ABOUT THE CLI — that `ls` takes
 * `--collection`, that `get` takes one `--include` carrying a comma list —
 * and `cli.test.ts` checks the same spellings against `kb --help` so that a
 * claim cannot stay true here and false there.
 */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import {
    addArgv,
    addBatchArgv,
    addDirArgv,
    batchLines,
    chunkArgv,
    embedArgv,
    collectionsArgv,
    deleteCollectionArgv,
    forgetArgv,
    getArgv,
    refreshSourceArgv,
    sourceArgv,
    sourcesArgv,
    initArgv,
    linkArgv,
    linksArgv,
    lsArgv,
    refreshArgv,
    renameCollectionArgv,
    searchArgv,
    staleArgv,
    statsArgv,
    statusArgv,
} from "../argv";

test("an unfiltered list asks for nothing it was not asked for", () => {
    /* A blank filter is not a filter: `--collection ""` asks for documents
     * whose collection is the empty string, which is nobody's question. */
    assert.deepEqual(lsArgv(), ["ls"]);
    assert.deepEqual(lsArgv({ collection: "  ", source: "", mime: null, since: undefined }), ["ls"]);
});

test("every filter §2 names is a flag, and the value is trimmed", () => {
    assert.deepEqual(
        lsArgv({
            collection: " win32-iocp ",
            source: "S-3",
            mime: "text/markdown",
            since: "2026-01-01T00:00:00Z",
            limit: 50,
        }),
        [
            "ls",
            "--collection",
            "win32-iocp",
            "--source",
            "S-3",
            "--mime",
            "text/markdown",
            "--since",
            "2026-01-01T00:00:00Z",
            "--limit",
            "50",
        ],
    );
});

test("ls takes several collections, a text filter and meta, as search does", () => {
    assert.deepEqual(lsArgv({ collection: ["win32", " io-uring ", ""], q: "ports", meta: { year: 2024 } }), [
        "ls",
        "--collection",
        "win32,io-uring",
        "--q",
        "ports",
        "--meta",
        '{"year":2024}',
    ]);
    assert.deepEqual(searchArgv("x", { meta: { tags: "iocp" } }), ["search", "--meta", '{"tags":"iocp"}', "x"]);
    assert.deepEqual(sourcesArgv({ status: "fetch_failed", q: "kernel" }), [
        "sources",
        "--status",
        "fetch_failed",
        "--q",
        "kernel",
    ]);
    assert.deepEqual(addArgv({ title: "T", collection: "c", etag: "abc" }), [
        "add",
        "--title",
        "T",
        "--collection",
        "c",
        "--etag",
        "abc",
        "--file",
        "-",
    ]);
});

test("a limit that is not a number is left off rather than sent as one", () => {
    /* `--limit NaN` is a parse failure at the far end, and the far end is
     * where a caller would have to read the reason. */
    assert.deepEqual(lsArgv({ limit: Number.NaN }), ["ls"]);
    assert.deepEqual(lsArgv({ limit: Number.POSITIVE_INFINITY }), ["ls"]);
    /* Zero IS a number and is passed: the CLI refuses it with a reason, and a
     * binding that silently dropped it would answer a `--limit 0` with every
     * document in the store. */
    assert.deepEqual(lsArgv({ limit: 0 }), ["ls", "--limit", "0"]);
});

test("get takes its subject positionally and one --include carrying a list", () => {
    /* Two `--include` flags would leave the CLI's last-wins parsing deciding
     * which counted, which is a coin toss written as an argument list. */
    assert.deepEqual(getArgv("D-241"), ["get", "D-241"]);
    assert.deepEqual(getArgv("D-241", { text: true }), ["get", "D-241", "--include", "text"]);
    assert.deepEqual(getArgv("D-241", { chunks: true }), ["get", "D-241", "--include", "chunks"]);
    assert.deepEqual(getArgv("D-241", { text: true, chunks: true }), [
        "get",
        "D-241",
        "--include",
        "text,chunks",
    ]);
});

test("collections and status are the whole of their own command lines", () => {
    assert.deepEqual(collectionsArgv(), ["collections"]);
    assert.deepEqual(statusArgv(), ["status"]);
    assert.deepEqual(initArgv(), ["init"]);
});

/* ------------------------------------------------------------- the search */

test("§4's parameters are flags of the same name, and the query is the subject", () => {
    assert.deepEqual(
        searchArgv("completion port", {
            collection: ["win32-iocp", "io-uring"],
            mode: "hybrid",
            k: 10,
            expand: 1,
            source: "S-3",
            mime: "text/markdown",
            since: "2026-01-01T00:00:00Z",
            minScore: 0.2,
        }),
        [
            "search",
            "--collection",
            "win32-iocp,io-uring",
            "--mode",
            "hybrid",
            "--k",
            "10",
            "--expand",
            "1",
            "--source",
            "S-3",
            "--mime",
            "text/markdown",
            "--since",
            "2026-01-01T00:00:00Z",
            "--min-score",
            "0.2",
            "completion port",
        ],
    );
});

test("a bare search is the command and the query and nothing else", () => {
    /* `mode` is NOT sent. §4 makes hybrid the default; spelling it here would
     * pin this package to today's default the day the store changed it. */
    assert.deepEqual(searchArgv("io_uring"), ["search", "io_uring"]);
    assert.equal(searchArgv("io_uring").includes("--mode"), false);
});

test("a query that begins with a dash is still a query", () => {
    /* `--` ends the flags for every command, so a subject that could be read
     * as one is written behind it. Not unconditionally: an always-present `--`
     * is a second thing for a reader of a log line to decode. */
    assert.deepEqual(searchArgv("--json"), ["search", "--", "--json"]);
    assert.deepEqual(searchArgv("-k"), ["search", "--", "-k"]);
    assert.deepEqual(searchArgv("k-means"), ["search", "k-means"]);
});

test("a blank query is sent, so the store is the one that refuses it", () => {
    /* Covering it up here would send a DIFFERENT command than the one asked
     * for, and the reader would see every document rather than a reason. */
    assert.deepEqual(searchArgv(""), ["search", ""]);
    assert.deepEqual(searchArgv("   "), ["search", "   "]);
});

test("a collection list is one flag whether it arrives as a list or a string", () => {
    assert.deepEqual(searchArgv("q", { collection: "papers" }), [
        "search",
        "--collection",
        "papers",
        "q",
    ]);
    assert.deepEqual(searchArgv("q", { collection: ["papers", " ", ""] }), [
        "search",
        "--collection",
        "papers",
        "q",
    ]);
    assert.deepEqual(searchArgv("q", { collection: [] }), ["search", "q"]);
});

test("--rerank is sent only when it is asked for", () => {
    /* It costs seconds a query and needs the reranker model, so false, null
     * and absent all leave the store's own ordering alone. */
    assert.deepEqual(searchArgv("q", { rerank: true }), ["search", "--rerank", "q"]);
    assert.deepEqual(searchArgv("q", { rerank: false }), ["search", "q"]);
    assert.deepEqual(searchArgv("q", { rerank: null }), ["search", "q"]);
    assert.deepEqual(searchArgv("-q", { rerank: true, k: 5 }), ["search", "--k", "5", "--rerank", "--", "-q"]);
});

test("a chunk is read by id, with its neighbours asked for by number", () => {
    assert.deepEqual(chunkArgv("C-99812"), ["chunk", "C-99812"]);
    assert.deepEqual(chunkArgv("C-99812", { expand: 2 }), [
        "chunk",
        "C-99812",
        "--expand",
        "2",
    ]);
});

/* -------------------------------------------------------------- the write */

test("add names the document and takes its content from stdin", () => {
    /* An argument list has a hard size limit a page reaches long before
     * anybody notices, and a page's text in a process listing is not where a
     * page's text belongs. */
    assert.deepEqual(addArgv({ title: "IOCP", collection: "win32-iocp" }), [
        "add",
        "--title",
        "IOCP",
        "--collection",
        "win32-iocp",
        "--file",
        "-",
    ]);
});

test("meta crosses as one argument of JSON, which is safe whatever is in it", () => {
    const argv = addArgv({
        title: "IOCP",
        collection: "win32-iocp",
        url: "https://example.test/x",
        mime: "text/markdown",
        meta: { authors: ["a b"], note: 'he said "hi"; rm -rf ~/dummy' },
    });
    assert.deepEqual(argv, [
        "add",
        "--title",
        "IOCP",
        "--collection",
        "win32-iocp",
        "--url",
        "https://example.test/x",
        "--mime",
        "text/markdown",
        "--meta",
        '{"authors":["a b"],"note":"he said \\"hi\\"; rm -rf ~/dummy"}',
        "--file",
        "-",
    ]);
    /* One element, whole: the quotes and the semicolon are inside it. */
    assert.equal(argv.filter((a) => a.includes("rm -rf")).length, 1);
});

test("a title or a collection the caller left empty is still sent, so the CLI says why", () => {
    /* The store's refusal names the missing flag. A binding that dropped the
     * flag would produce `kb add --collection x` and a message about a
     * different missing argument. */
    const argv = addArgv({ title: "", collection: "" });
    assert.equal(argv[argv.indexOf("--title") + 1], "");
    assert.equal(argv[argv.indexOf("--collection") + 1], "");
});

test("a folder is one argument and the collection another, and forgetting is on unless turned off", () => {
    /* The store walks the folder itself, so nothing goes down stdin and there
     * is no `--file -`. `--no-forget` is sent only when asked for: the CLI
     * forgets by default, and a binding that defaulted the other way would be
     * a second answer to what `kb add --dir` does. */
    assert.deepEqual(addDirArgv("/work/lap/cli", { collection: "code" }), [
        "add",
        "--dir",
        "/work/lap/cli",
        "--collection",
        "code",
    ]);
    assert.deepEqual(addDirArgv("/work/lap/cli", { collection: "code", forget: true }), [
        "add",
        "--dir",
        "/work/lap/cli",
        "--collection",
        "code",
    ]);
    assert.deepEqual(addDirArgv("my notes", { collection: "code", forget: false }), [
        "add",
        "--dir",
        "my notes",
        "--collection",
        "code",
        "--no-forget",
    ]);
    // A blank collection is still sent, so the CLI's refusal names it.
    const blank = addDirArgv("x", { collection: "" });
    assert.equal(blank[blank.indexOf("--collection") + 1], "");
});

test("every add takes the embedding budget: seconds, or --wait for no limit", () => {
    /* Absent means the store's own default of 20 seconds, so nothing is sent;
     * 0 is a budget (embed nothing now), not an absence, and is sent. */
    assert.deepEqual(addArgv({ title: "t", collection: "c", embedBudget: 0 }), [
        "add", "--title", "t", "--collection", "c", "--embed-budget", "0", "--file", "-",
    ]);
    assert.deepEqual(addArgv({ title: "t", collection: "c", wait: true }), [
        "add", "--title", "t", "--collection", "c", "--wait", "--file", "-",
    ]);
    assert.deepEqual(addArgv({ title: "t", collection: "c", embedBudget: null, wait: false }), [
        "add", "--title", "t", "--collection", "c", "--file", "-",
    ]);
    assert.deepEqual(addArgv({ title: "t", collection: "c", embedBudget: Number.NaN }), [
        "add", "--title", "t", "--collection", "c", "--file", "-",
    ]);
    assert.deepEqual(addBatchArgv(), ["add", "--batch"]);
    assert.deepEqual(addBatchArgv({ embedBudget: 2.5 }), ["add", "--batch", "--embed-budget", "2.5"]);
    assert.deepEqual(addBatchArgv({ wait: true }), ["add", "--batch", "--wait"]);
    assert.deepEqual(addDirArgv("d", { collection: "c", forget: false, embedBudget: 0 }), [
        "add", "--dir", "d", "--collection", "c", "--no-forget", "--embed-budget", "0",
    ]);
    assert.deepEqual(addDirArgv("d", { collection: "c", wait: true }), [
        "add", "--dir", "d", "--collection", "c", "--wait",
    ]);
    /* Both at once is the caller's mistake, and the store's refusal says so:
     * the binding sends both rather than choosing one. */
    assert.deepEqual(addBatchArgv({ embedBudget: 1, wait: true }), ["add", "--batch", "--embed-budget", "1", "--wait"]);
});

test("embed is the whole of its command line", () => {
    assert.deepEqual(embedArgv(), ["embed"]);
});

/* -------------------------------------------------- nothing is ever a line */

test("no builder ever produces an argument that is two arguments", () => {
    /* THE PROPERTY THE WHOLE PACKAGE RESTS ON, STATED OVER THE BUILDERS RATHER
     * THAN OVER ONE OF THEM. A value a reader typed is one element of the argv
     * however hostile it is, because there is no command string for it to be
     * interpolated into. What would break this is somebody writing
     * `` `--collection ${name}` `` — one element that the far end would split
     * into two — so the check is that no element carries an interior space
     * unless the caller's own value did.
     *
     * The values below are the ones a reader can actually type into §2's box
     * and §3's title. */
    const hostile = 'a b; rm -rf ~/dummy && echo "$(whoami)" | tee /tmp/x `id` \n\t--json';
    const lines: string[][] = [
        lsArgv({ collection: hostile, mime: hostile, since: hostile }),
        getArgv(hostile, { text: true }),
        searchArgv(hostile, { collection: hostile, source: hostile, mime: hostile }),
        chunkArgv(hostile),
        addArgv({ title: hostile, collection: hostile, url: hostile, meta: { k: hostile } }),
        addDirArgv(hostile, { collection: hostile, forget: false }),
        collectionsArgv(),
        initArgv(),
        statusArgv(),
    ];
    for (const argv of lines) {
        for (const element of argv) {
            const flag = element.startsWith("-");
            if (flag) {
                assert.ok(
                    !/\s/.test(element),
                    `a flag was built with whitespace in it, which is two arguments pretending to be one: ${JSON.stringify(element)}`,
                );
            }
        }
        /* And the hostile text arrives whole wherever it was passed, never
         * split and never quoted — quoting would be this package inventing a
         * shell that is not there. */
        const carried = argv.filter((a) => a.includes("rm -rf ~/dummy"));
        for (const element of carried) {
            assert.ok(
                element === hostile || element.includes(JSON.stringify(hostile).slice(1, -1)),
                `a value was altered on the way into the argv: ${JSON.stringify(element)}`,
            );
        }
    }
});

/* ------------------------------------------------------- one store, no tier */

test("no builder ever emits --store, because the CLI no longer has one", () => {
    /* THERE IS ONE STORE: the first `.kb/` at or above the working directory
     * (§1.4). The global tier and its `--store project|global|all` selector
     * are gone from the CLI, and `kb` now refuses the flag as `usage: unknown
     * option` on every command — so a builder that still spelled it would
     * turn every call it made into a refusal. Stated over EVERY builder, each
     * with every option it takes filled in, because the flag used to ride on
     * the options object and the way it comes back is somebody re-adding it
     * to one of them. */
    const lines: string[][] = [
        lsArgv({ collection: "c", source: "S-1", mime: "text/plain", since: "2026-01-01T00:00:00Z", limit: 5 }),
        getArgv("D-1", { text: true, chunks: true }),
        collectionsArgv(),
        statusArgv(),
        searchArgv("q", {
            collection: ["a", "b"],
            mode: "hybrid",
            k: 3,
            expand: 1,
            source: "S-1",
            mime: "text/plain",
            since: "2026-01-01T00:00:00Z",
            minScore: 0.1,
            rerank: true,
        }),
        chunkArgv("C-1", { expand: 2 }),
        addArgv({ title: "t", collection: "c", url: "https://example.test", mime: "text/plain", meta: { a: 1 } }),
        staleArgv({ olderThan: "30d", collection: "c" }),
        refreshArgv({ collection: "c", olderThan: "30d" }),
        renameCollectionArgv("a", "b"),
        deleteCollectionArgv("a"),
        deleteCollectionArgv("a", true),
        forgetArgv("D-1"),
        addBatchArgv(),
        addDirArgv("/tmp/x", { collection: "c", forget: false }),
        sourcesArgv({ collection: "c", kind: "file" }),
        sourceArgv("S-1"),
        refreshSourceArgv("S-1"),
        linksArgv("D-1"),
        linkArgv("D-1", "analogue_of", "D-2"),
        statsArgv(),
        initArgv(),
    ];
    for (const argv of lines) {
        assert.ok(!argv.includes("--store"), `a builder emitted --store: ${JSON.stringify(argv)}`);
        assert.ok(
            !argv.some((a) => a.startsWith("--store=")),
            `a builder emitted --store=: ${JSON.stringify(argv)}`,
        );
    }
});

test("forgetting is asked for by id, and a collection's documents only by name", () => {
    assert.deepEqual(forgetArgv("D-241"), ["forget", "D-241"]);
    assert.deepEqual(forgetArgv("S-3"), ["forget", "S-3"]);
    assert.deepEqual(deleteCollectionArgv("io-uring"), ["collections", "delete", "--", "io-uring"]);
    assert.deepEqual(deleteCollectionArgv("io-uring", true), ["collections", "delete", "--with-documents", "--", "io-uring"]);
    // A collection name that looks like the flag is still a name.
    assert.deepEqual(deleteCollectionArgv("--with-documents"), ["collections", "delete", "--", "--with-documents"]);
});

test("sources are listed with their narrowings as flags, shown and refreshed by id", () => {
    assert.deepEqual(sourcesArgv(), ["sources"]);
    assert.deepEqual(sourcesArgv({ collection: " research ", kind: "file" }), ["sources", "--collection", "research", "--kind", "file"]);
    assert.deepEqual(sourcesArgv({ collection: "", kind: null }), ["sources"]);
    assert.deepEqual(sourceArgv("S-3"), ["sources", "show", "S-3"]);
    assert.deepEqual(refreshSourceArgv("S-3"), ["refresh", "S-3"]);
});

test("a batch is one JSON line per document on stdin, and nothing of it is an argument", () => {
    assert.deepEqual(addBatchArgv(), ["add", "--batch"]);
    const text = batchLines([
        { title: "one", collection: "c", content: "line one\nline two" },
        { title: "two", collection: "c", content: "x", url: " https://example.test ", mime: "", meta: { year: 2026 } },
    ]);
    const lines = text.split("\n");
    assert.equal(lines.length, 3, "two lines and the final newline; a newline inside content stays escaped");
    assert.deepEqual(JSON.parse(lines[0]), { title: "one", collection: "c", content: "line one\nline two" });
    assert.deepEqual(JSON.parse(lines[1]), { title: "two", collection: "c", content: "x", url: "https://example.test", meta: { year: 2026 } });
});
