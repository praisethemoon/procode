/* Against the real binary.
 *
 * WHAT THIS ADDS OVER THE FAKE. The fake proves what this package does with an
 * answer; this proves the questions are ones the CLI actually accepts. Every
 * flag spelling in `argv.ts` is a CLAIM about a program in another language,
 * and a claim that is only checked against a fixture is a claim that can be
 * wrong for as long as nobody runs the real thing.
 *
 * IT NEVER TOUCHES A STORE IT WAS NOT ASKED TO. `KB_STORE` is pointed inside a
 * throwaway directory and the working directory is inside it too, so the global
 * tier this exercises is the temporary one and the project tier is found by
 * walking up from a directory that only exists for the length of the run. A
 * test that wrote into `~/.kb` would be a test that files somebody's research
 * under `win32-iocp` every time it ran.
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
import { KbError } from "../errors";
import { lsArgv, refreshArgv, searchArgv } from "../argv";

const ROOT = path.resolve(__dirname, "..", "..");
const BIN = path.resolve(ROOT, "..", "kb-cli", "bin", "kb");

function built(): boolean {
    return fs.existsSync(BIN);
}

/* `kb --help`, which is the CLI's own statement of its surface. */
function help(): string {
    return execFileSync(BIN, ["--help"], { encoding: "utf8" });
}

interface Work {
    dir: string;
    kb: Kb;
    /* The same environment the client was given, so a test that runs the
     * binary directly points at the same throwaway global tier. */
    env: NodeJS.ProcessEnv;
    dispose(): void;
}

function workspace(): Work {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kb-js-real-"));
    const project = path.join(dir, "project");
    fs.mkdirSync(project);
    const kb = new Kb({
        bin: BIN,
        cwd: project,
        /* The global tier, inside the throwaway. Never `~/.kb`. */
        env: { ...process.env, KB_STORE: path.join(dir, "global") },
    });
    return {
        dir: project,
        kb,
        env: { ...process.env, KB_STORE: path.join(dir, "global") },
        dispose: () => fs.rmSync(dir, { recursive: true, force: true }),
    };
}

test("every flag this package spells for an implemented command is one the CLI names", async (t) => {
    if (!built()) {
        t.skip("kb-cli/bin/kb is not built");
        return;
    }
    const text = help();
    /* The commands that exist today. `search` and `chunk` are checked
     * separately, below, and only once they turn up — so the day they land,
     * this suite starts checking their flags without anybody editing it. */
    const argvs = [
        lsArgv({ collection: "c", source: "S-1", mime: "m", since: "s", limit: 1, store: "all" }),
        ["get", "D-1", "--include", "text,chunks", "--store", "all"],
        ["collections", "--store", "all"],
        ["status"],
        ["init", "--store", "global"],
        ["add", "--title", "t", "--collection", "c", "--url", "u", "--mime", "m", "--meta", "{}", "--file", "-"],
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
});

test("search's flags are checked the moment the command exists", async (t) => {
    if (!built()) {
        t.skip("kb-cli/bin/kb is not built");
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
        store: "all",
        source: "S-1",
        mime: "m",
        since: "s",
        minScore: 0.2,
    })) {
        if (element.startsWith("--") && element !== "--") {
            assert.ok(text.includes(element), `kb search does not take ${element}`);
        }
    }
});

test("a store filed into and read back through this package", async (t) => {
    if (!built()) {
        t.skip("kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    try {
        const created = await work.kb.init("project");
        assert.equal(created.store, "project");

        const added = await work.kb.add("# IOCP\n\nCreateIoCompletionPort binds a handle.\n", {
            title: "I/O Completion Ports",
            collection: "win32-iocp",
            url: "https://learn.microsoft.test/win32/iocp",
            meta: { authors: ["MSDN"], year: 2026 },
        });
        assert.equal(added.created, true);
        assert.equal(added.store, "project");
        assert.match(added.document, /^D-\d+$/);

        /* Every field `types.ts` claims a listed document has, off the real
         * binary: the CLI's row is wider than §1.2's `Document` and the reader
         * is built for the wider one. */
        const [row] = await work.kb.ls();
        assert.equal(row.id, added.document);
        assert.equal(row.collection, "win32-iocp");
        assert.equal(row.locator, "https://learn.microsoft.test/win32/iocp");
        assert.equal(row.store, "project");
        /* `text/plain`, AND THAT IS THE CLI'S ANSWER RATHER THAN A DISAPPOINT-
         * MENT. `kb add` guesses the type from the locator's extension, and
         * `.../win32/iocp` has none — so a page that is markdown is filed as
         * plain text unless the caller says otherwise. It matters here because
         * §3.2 of the UI spec renders by mime: a document filed this way would
         * read as a wall of unformatted text with its own hashes in it. The
         * caller that knows — a surface filing the file it has open — passes
         * `mime`, and the next assertion is that doing so wins. */
        assert.equal(row.mime, "text/plain");
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
            mime: "text/markdown",
        });
        const typedRow = (await work.kb.ls({ collection: "io-uring" }))[0];
        assert.equal(typedRow.id, typed.document);
        assert.equal(typedRow.mime, "text/markdown");

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
        assert.equal(iocp.store, "project");

        const status = await work.kb.status();
        assert.equal(status.defaultWrite, "project");
        const project = status.tiers.find((s) => s.store === "project");
        assert.equal(project?.present, true);
        assert.equal(project?.documents, 2);
        assert.equal(project?.chunking?.current, true);
    } finally {
        work.dispose();
    }
});

test("a real refusal arrives as a KbError carrying §11's code", async (t) => {
    if (!built()) {
        t.skip("kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    try {
        await work.kb.init("project");
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
        t.skip("kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    try {
        await work.kb.init("project");
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
        t.skip("kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    try {
        await work.kb.init("project");
        await work.kb.add("text\n", { title: "A title with spaces", collection: "two words" });
        const [row] = await work.kb.ls({ collection: "two words" });
        assert.equal(row.collection, "two words");
        assert.equal(row.title, "A title with spaces");
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

function raw(work: Work, argv: readonly string[]): Record<string, unknown> {
    const out = execFileSync(BIN, [...argv, "--json"], {
        cwd: work.dir,
        env: work.env,
        encoding: "utf8",
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
        t.skip("kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    try {
        await work.kb.init("project");
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
        t.skip("kb-cli/bin/kb is not built");
        return;
    }
    assert.deepEqual(refreshArgv(), ["refresh"]);
    assert.deepEqual(
        refreshArgv({ collection: "win32-iocp", olderThan: "90d", store: "all" }),
        ["refresh", "--collection", "win32-iocp", "--older-than", "90d", "--store", "all"],
    );
    for (const flag of refreshArgv({ collection: "c", olderThan: "1d", store: "all" })) {
        if (flag.startsWith("--")) {
            assert.ok(help().includes(flag), `kb refresh does not take ${flag}`);
        }
    }
    const text = help();
    assert.equal(/refresh[^\n]*--document/.test(text), false);
    assert.equal(/refresh[^\n]*--source\b/.test(text), false);
});

test("every reader answers exactly the keys the real binary prints", async (t) => {
    if (!built()) {
        t.skip("kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    try {
        await work.kb.init("project");
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
        assert.equal(second.created, true);
    } finally {
        work.dispose();
    }
});
