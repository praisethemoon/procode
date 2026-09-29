/* The client, over fixtures shaped by the specification.
 *
 * `search` AND `chunk` ARE CHECKED AGAINST §4'S HIT AND NOTHING ELSE, because
 * the commands that serve them are being built in parallel with this package.
 * What is pinned here is what this reader does with a hit — every field
 * crosses, `matched` is not narrowed, two hits stay distinguishable — so
 * that when the command lands the only thing left to reconcile is the flag
 * spelling, which `argv.ts` keeps in one place.
 */

import * as assert from "node:assert/strict";
import { test as base } from "node:test";

/* The stand-in kb (fake.ts) is a #! script, which Windows cannot start, so
 * this file's tests run on macOS and Linux only. */
const test = process.platform === "win32" ? base.skip : base;

import { Kb } from "../client";
import { KbError } from "../errors";
import { FakeKb, FakeAnswer, ok, refusal } from "./fake";
import { CHUNK, COLLECTIONS, DOCUMENT, DOCUMENT_OLD, HIT, HIT_OLD, STATUS, STATUS_NONE } from "./fixtures";

async function withKb<T>(
    answers: readonly FakeAnswer[],
    fn: (kb: Kb, fake: FakeKb) => Promise<T>,
): Promise<T> {
    const fake = FakeKb.create(answers);
    try {
        return await fn(new Kb({ bin: fake.bin, env: fake.env() }), fake);
    } finally {
        fake.dispose();
    }
}

test("a list is the store's rows, typed and in the store's own order", async () => {
    /* NOT RE-SORTED HERE. §2 of the UI spec wants newest first and the surface
     * does that where it draws the rows — a reader that re-ordered would be
     * deciding for every caller, including the ones that asked for log order. */
    await withKb([{ stdout: ok({ documents: [DOCUMENT_OLD, DOCUMENT], count: 2 }) }], async (kb) => {
        const rows = await kb.ls({ collection: "win32-iocp" });
        assert.deepEqual(rows.map((r) => r.id), ["D-88", "D-241"]);
        assert.equal(rows[1].collection, "win32-iocp");
        assert.equal(rows[1].locator, "https://learn.microsoft.test/win32/iocp");
        assert.deepEqual(rows[1].meta, { authors: ["MSDN"], year: 2026, section: ["Win32", "IOCP"] });
    });
});

test("a document read carries text and chunks exactly when they were asked for", async () => {
    /* §4's discipline — "search returns snippets only" — is the same rule one
     * route along: a read that was not asked to carry the text must not. An
     * absent key and an empty string are different answers. */
    await withKb(
        [
            { stdout: ok({ document: DOCUMENT }) },
            { stdout: ok({ document: DOCUMENT, text: "# IOCP\n", chunks: [CHUNK] }) },
        ],
        async (kb, fake) => {
            const bare = await kb.get("D-241");
            assert.equal("text" in bare, false);
            assert.equal("chunks" in bare, false);

            const full = await kb.get("D-241", { text: true, chunks: true });
            assert.equal(full.text, "# IOCP\n");
            assert.equal(full.chunks?.length, 1);
            assert.equal(full.chunks?.[0].heading, "Creating a completion port");
            assert.deepEqual(full.chunks?.[0].span, { start: 1024, end: 2048 });

            assert.deepEqual(fake.calls()[1].argv, [
                "get",
                "D-241",
                "--include",
                "text,chunks",
                "--json",
            ]);
        },
    );
});

test("a chunk with no heading says so rather than saying it has an empty one", async () => {
    /* §1.2 writes `heading?`: a chunk in the middle of a plain-text document
     * genuinely has none, and a UI that scrolled to "" would scroll nowhere. */
    await withKb(
        [{ stdout: ok({ document: DOCUMENT, chunks: [{ ...CHUNK, heading: "" }] }) }],
        async (kb) => {
            const read = await kb.get("D-241", { chunks: true });
            assert.equal(read.chunks?.[0].heading, null);
        },
    );
});

test("a search answers §4's hits, field for field", async () => {
    await withKb([{ stdout: ok({ hits: [HIT, HIT_OLD], count: 2 }) }], async (kb, fake) => {
        const result = await kb.search("completion port", { k: 10 });
        assert.equal(result.count, 2);
        assert.deepEqual(result.hits[0], {
            chunk: "C-99812",
            document: "D-241",
            source: "S-3",
            title: "I/O Completion Ports",
            heading: "Creating a completion port",
            snippet: "CreateIoCompletionPort associates an open file handle with a port.",
            collection: "win32-iocp",
            matched: ["keyword", "semantic"],
            scores: { bm25: 11.25, vector: 0.82, fused: 0.031 },
            fetchedAt: "2026-06-01T09:15:00Z",
            stale: false,
        });
        assert.deepEqual(fake.calls()[0].argv, [
            "search",
            "--k",
            "10",
            "completion port",
            "--json",
        ]);
    });
});

test("two hits stay apart, and so do the paths that found them", async () => {
    /* §4: "a result found by both is a different kind of result from one found
     * by either". The path is shown on the row, so it has to survive this
     * layer, and so does everything else that tells one row from the next. */
    await withKb([{ stdout: ok({ hits: [HIT, HIT_OLD], count: 2 }) }], async (kb) => {
        const { hits } = await kb.search("submission queue");
        assert.deepEqual(hits.map((h) => h.collection), ["win32-iocp", "io-uring"]);
        assert.deepEqual(hits.map((h) => [...h.matched]), [["keyword", "semantic"], ["semantic"]]);
        assert.deepEqual(hits.map((h) => h.stale), [false, true]);
        assert.equal(hits[1].heading, null);
    });
});

test("a retrieval path this reader has never heard of still reaches the row", async () => {
    /* Hybrid is two paths today. A reader that filtered `matched` against the
     * two it knows would silently drop a third, and the row would then claim
     * it was found by nothing. */
    await withKb(
        [{ stdout: ok({ hits: [{ ...HIT, matched: ["keyword", "rerank", 7, null] }] }) }],
        async (kb) => {
            const { hits } = await kb.search("x");
            assert.deepEqual([...hits[0].matched], ["keyword", "rerank"]);
        },
    );
});

test("a count the store did not give is the number of hits and not a guess at a total", async () => {
    await withKb([{ stdout: ok({ hits: [HIT] }) }], async (kb) => {
        assert.equal((await kb.search("x")).count, 1);
    });
});

test("a chunk read is the chunk and its neighbours", async () => {
    await withKb(
        [
            {
                stdout: ok({
                    chunk: { ...CHUNK, text: "CreateIoCompletionPort ..." },
                    neighbours: [
                        { ...CHUNK, id: "C-99811", ordinal: 1 },
                        { ...CHUNK, id: "C-99813", ordinal: 3 },
                    ],
                }),
            },
        ],
        async (kb) => {
            const read = await kb.chunk("C-99812", { expand: 1 });
            assert.equal(read.chunk.text, "CreateIoCompletionPort ...");
            assert.deepEqual(read.neighbours.map((n) => n.id), ["C-99811", "C-99813"]);
        },
    );
});

test("collections are the store's rows, with the counts it gave", async () => {
    await withKb([{ stdout: ok({ collections: COLLECTIONS, count: 2 }) }], async (kb, fake) => {
        const rows = await kb.collections();
        assert.deepEqual(
            rows.map((c) => [c.name, c.documents, c.bytes]),
            [
                ["win32-iocp", 7, 90210],
                ["io-uring", 3, 40000],
            ],
        );
        assert.deepEqual(fake.calls()[0].argv, ["collections", "--json"]);
    });
});

test("status keeps a store that is not there distinguishable from an empty one", async () => {
    /* "no store here" and "a store with no documents in it" are two different
     * things to tell a reader, and zero says the second one. There is one
     * store — the `.kb/` found by walking up — so the answer is flat: its
     * counts when it is there, `path: null` and nothing else when it is not. */
    await withKb([{ stdout: ok(STATUS) }, { stdout: ok(STATUS_NONE) }], async (kb) => {
        const status = await kb.status();
        assert.equal(status.path, "/work/project/.kb");
        assert.equal(status.present, true);
        assert.equal(status.documents, 7);
        assert.equal(status.chunking?.current, true);
        assert.equal(status.olderThan, "90d");

        const none = await kb.status();
        assert.equal(none.path, null);
        assert.equal(none.present, false);
        assert.equal(none.documents, undefined);
        assert.equal(none.olderThan, "90d");
    });
});

test("add hands the content over on stdin and answers one shape", async () => {
    await withKb(
        [
            {
                stdout: ok({
                    document: "D-242",
                    source: "S-4",
                    contentHash: "c".repeat(64),
                    bytes: 12,
                    collection: "win32-iocp",
                    mime: "text/markdown",
                    splitter: "markdown",
                    chunkCount: 1,
                    chunkBase: 99813,
                    created: true,
                    reindexed: true,
                    blobWritten: true,
                    fetchedAt: "2026-09-25T10:00:00Z",
                }),
            },
        ],
        async (kb, fake) => {
            const added = await kb.add("# Hello\n", {
                title: "Hello",
                collection: "win32-iocp",
                meta: { authors: ["nobody"] },
            });
            assert.equal(added.document, "D-242");
            assert.equal(added.created, true);
            assert.equal(added.splitter, "markdown");
            assert.equal(fake.calls()[0].stdin, "# Hello\n");
            assert.ok(fake.calls()[0].argv.includes("--file"));
        },
    );
});

test("a search keeps unembedded when the store says some chunks have no vector, and only then", async () => {
    await withKb(
        [
            { stdout: ok({ mode: "hybrid", hits: [HIT], unembedded: 812, count: 1, olderThan: "90d" }) },
            { stdout: ok({ mode: "hybrid", hits: [HIT], count: 1, olderThan: "90d" }) },
        ],
        async (kb) => {
            assert.equal((await kb.search("x")).unembedded, 812);
            assert.equal("unembedded" in (await kb.search("x")), false);
        },
    );
});

test("add and a batch keep what was left to embed, and embed asks for exactly that", async () => {
    await withKb(
        [
            { stdout: ok({ document: "D-9", chunkCount: 900, pending: 812 }) },
            { stdout: ok({ added: [{ document: "D-10" }], count: 1, pending: 3 }) },
            { stdout: ok({ embedded: 815, kept: 88, skipped: 2, pending: 0 }) },
            { stdout: refusal("model_missing", "nothing to embed with"), exit: 1 },
        ],
        async (kb, fake) => {
            const added = await kb.add("big\n", { title: "Big", collection: "c", embedBudget: 0 });
            assert.equal(added.pending, 812);
            const batch = await kb.addBatch([{ title: "t", collection: "c", content: "x" }], { wait: true });
            assert.equal(batch.pending, 3);
            assert.equal(batch.added[0].document, "D-10");
            assert.deepEqual(await kb.embed(), { embedded: 815, kept: 88, skipped: 2, pending: 0 });
            const refused = (await kb.embed().catch((x: unknown) => x)) as KbError;
            assert.ok(refused instanceof KbError);
            assert.equal(refused.code, "model_missing");
            const calls = fake.calls();
            assert.deepEqual(calls[0].argv, ["add", "--title", "Big", "--collection", "c", "--embed-budget", "0", "--file", "-", "--json"]);
            assert.deepEqual(calls[1].argv, ["add", "--batch", "--wait", "--json"]);
            assert.deepEqual(calls[2].argv, ["embed", "--json"]);
        },
    );
});

test("a refusal reaches the caller as a KbError with the store's code", async () => {
    await withKb([{ stdout: refusal("not_found", "no document D-9999"), exit: 1 }], async (kb) => {
        const e = (await kb.get("D-9999").catch((x: unknown) => x)) as KbError;
        assert.ok(e instanceof KbError);
        assert.equal(e.spec, "not_found");
    });
});

test("`at` points a second client at another store", async () => {
    /* §1.4 walks up from the working directory, so the directory IS the store
     * selection: there is no other way to name one. */
    await withKb([{ stdout: ok({ documents: [] }) }], async (kb, fake) => {
        await kb.at(fake.dir).ls();
        const { realpathSync } = await import("node:fs");
        assert.equal(realpathSync(fake.calls()[0].cwd), realpathSync(fake.dir));
    });
});
