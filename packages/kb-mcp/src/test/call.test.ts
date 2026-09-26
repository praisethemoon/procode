/* What each tool does with its arguments, and what the store sees.
 *
 * THE ARGV THE FAKE RECORDS IS THE EVIDENCE. Every assertion below about a
 * filter is an assertion about a command line that actually left this process:
 * a tool that accepted `collection` and forgot to pass it would return hits
 * from the whole store, confidently, and no test that only looked at the
 * returned rows would notice. The fake is a real process for exactly that
 * reason.
 *
 * AND THE ERROR BOUNDARY IS TESTED FROM BOTH SIDES. An argument this surface
 * will not accept throws and becomes a JSON-RPC error; a store that refused a
 * well-formed call becomes a result with `isError`. Swapping the two is the
 * mutation that makes an agent retry a question it has already been answered,
 * or give up on a tool that is working.
 */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import { Kb } from "kb-js";

import { ToolResult, callTool } from "../call";
import { INVALID_PARAMS, RpcError } from "../jsonrpc";
import { FakeAnswer, FakeKb, ok, refusal } from "./fake";
import {
    ADDED,
    CHUNK,
    COLLECTIONS,
    DIR_ADDED,
    DOCUMENT,
    DOCUMENT_OLD,
    DOCUMENT_TEXT,
    HIT,
    HIT_OLD,
    LINKS,
    SNIPPET,
    STATS,
    TOTALS,
} from "./fixtures";

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

function body(result: ToolResult): Record<string, unknown> {
    assert.equal(result.content.length, 1);
    assert.equal(result.content[0].type, "text");
    return JSON.parse(result.content[0].text) as Record<string, unknown>;
}

/* The flag and its value, read back out of a recorded argv. */
function flag(argv: readonly string[], name: string): string | undefined {
    const i = argv.indexOf(name);
    return i === -1 ? undefined : argv[i + 1];
}

/* --------------------------------------------------------------- kb_search */

test("kb_search passes every filter §4 names through to the store", async () => {
    await withKb([{ stdout: ok({ hits: [HIT], count: 1 }) }], async (kb, fake) => {
        await callTool(kb, "kb_search", {
            q: "CreateIoCompletionPort",
            collection: ["win32-iocp", "io-uring"],
            mode: "keyword",
            k: 5,
            expand: 2,
            source: "S-3",
            mime: "text/markdown",
            since: "2026-01-01T00:00:00Z",
            minScore: 0.25,
        });
        const argv = fake.calls()[0].argv;
        /* Each one named individually, so the failure says WHICH filter was
         * dropped rather than that an array did not match. */
        assert.equal(flag(argv, "--collection"), "win32-iocp,io-uring");
        assert.equal(flag(argv, "--mode"), "keyword");
        assert.equal(flag(argv, "--k"), "5");
        assert.equal(flag(argv, "--expand"), "2");
        assert.equal(flag(argv, "--source"), "S-3");
        assert.equal(flag(argv, "--mime"), "text/markdown");
        assert.equal(flag(argv, "--since"), "2026-01-01T00:00:00Z");
        assert.equal(flag(argv, "--min-score"), "0.25");
        assert.equal(argv[argv.length - 2], "CreateIoCompletionPort");
    });
});

test("a search with nothing but a query asks for nothing but the query", async () => {
    /* The store owns its own defaults. A binding that spelled `--mode hybrid`
     * on every call would silently pin the default the day the store changed
     * it. */
    await withKb([{ stdout: ok({ hits: [], count: 0 }) }], async (kb, fake) => {
        await callTool(kb, "kb_search", { q: "io_uring" });
        assert.deepEqual(fake.calls()[0].argv, ["search", "io_uring", "--json"]);
    });
});

test("a search returns snippets and never the whole document", async () => {
    /* §4: "search returns snippets only. A list must not be able to flood a
     * caller's context." Stated two ways, because either alone can be made to
     * pass by an implementation that breaks the other:
     *
     *   - the text that comes back contains the snippet and does NOT contain
     *     the phrase that only exists in the full document;
     *   - the search cost exactly ONE process. A layer that enriched each hit
     *     with its document's text would be a second call per row, and that is
     *     how a well-meaning change would arrive. */
    await withKb([{ stdout: ok({ hits: [HIT, HIT_OLD], count: 2 }) }], async (kb, fake) => {
        const result = await callTool(kb, "kb_search", { q: "iocp" });
        const text = result.content[0].text;
        assert.ok(text.includes(SNIPPET));
        assert.equal(
            text.includes("THE-WHOLE-DOCUMENT"),
            false,
            "a search result carried the document's full text",
        );
        assert.equal(fake.calls().length, 1, "a search cost more than one process");
        assert.equal(fake.calls()[0].argv.includes("--include"), false);
    });
});

test("a hit keeps every field §4 puts on it", async () => {
    await withKb([{ stdout: ok({ hits: [HIT, HIT_OLD], count: 2 }) }], async (kb) => {
        const answer = body(await callTool(kb, "kb_search", { q: "iocp" }));
        const hits = answer["hits"] as Record<string, unknown>[];
        assert.equal(answer["count"], 2);
        assert.deepEqual(hits[0]["matched"], ["keyword", "semantic"]);
        assert.deepEqual(hits[1]["matched"], ["semantic"]);
        assert.equal(hits[0]["collection"], "win32-iocp");
        assert.equal(hits[1]["collection"], "io-uring");
        assert.equal(hits[0]["heading"], "Creating a completion port");
        assert.equal(hits[1]["heading"], null);
        assert.equal(hits[1]["fetchedAt"], "2024-01-02T00:00:00Z");
        assert.equal(hits[1]["stale"], true);
        assert.deepEqual(hits[0]["scores"], { bm25: 11.25, vector: 0.82, fused: 0.031 });
        assert.equal(hits[0]["chunk"], "C-99812");
        assert.equal(hits[0]["document"], "D-241");
        assert.equal(hits[0]["source"], "S-3");
    });
});

test("a search says which retrieval path ran and what stale was measured against", async () => {
    /* §4 makes hybrid the default and §8 makes it refuse without a model, so
     * the mode that came back is not always the mode that was asked for; and
     * a hit flagged stale without the threshold cannot say older than what.
     * Both are the store's words and both cross. */
    await withKb(
        [{ stdout: ok({ mode: "keyword", olderThan: "90d", hits: [HIT], count: 1 }) }],
        async (kb) => {
            const answer = body(await callTool(kb, "kb_search", { q: "iocp" }));
            assert.equal(answer["mode"], "keyword");
            assert.equal(answer["olderThan"], "90d");
        },
    );
});

test("a filter spelled wrong is refused instead of silently doing nothing", async () => {
    /* `collections` for `collection` is not a search of one collection that
     * fails; it is a search of the whole store that succeeds. */
    await withKb([{ stdout: ok({ hits: [], count: 0 }) }], async (kb, fake) => {
        const e = (await callTool(kb, "kb_search", {
            q: "x",
            collections: ["win32-iocp"],
        }).catch((x: unknown) => x)) as RpcError;
        assert.ok(e instanceof RpcError);
        assert.equal(e.code, INVALID_PARAMS);
        assert.match(e.message, /collections/);
        assert.equal(fake.calls().length, 0, "the store was asked anyway");
    });
});

test("a query that was never given is refused before anything is spawned", async () => {
    await withKb([{ stdout: ok({ hits: [] }) }], async (kb, fake) => {
        await assert.rejects(() => callTool(kb, "kb_search", {}), RpcError);
        await assert.rejects(() => callTool(kb, "kb_search", { q: 7 }), RpcError);
        assert.equal(fake.calls().length, 0);
    });
});

test("§4's ceiling of 100 hits is the schema's and is enforced", async () => {
    await withKb([{ stdout: ok({ hits: [] }) }], async (kb) => {
        await assert.rejects(() => callTool(kb, "kb_search", { q: "x", k: 500 }), RpcError);
        await assert.rejects(() => callTool(kb, "kb_search", { q: "x", k: 0 }), RpcError);
        await assert.rejects(() => callTool(kb, "kb_search", { q: "x", k: 1.5 }), RpcError);
    });
});

test("a query full of shell metacharacters reaches the store as one argument", async () => {
    const hostile = 'io_uring; rm -rf ~/dummy && echo "$(id)"';
    await withKb([{ stdout: ok({ hits: [], count: 0 }) }], async (kb, fake) => {
        await callTool(kb, "kb_search", { q: hostile });
        const argv = fake.calls()[0].argv;
        assert.equal(argv.filter((a) => a === hostile).length, 1);
    });
});

/* ------------------------------------------------------------------ kb_get */

test("kb_get reads the prefix to decide which route it is", async () => {
    /* §1.1 put the prefix there so that a reference is self-describing. A
     * `kind` argument beside the id would state the same fact twice, and the
     * two would eventually disagree. */
    await withKb(
        [
            { stdout: ok({ chunk: CHUNK, neighbours: [] }) },
            { stdout: ok({ document: DOCUMENT, text: DOCUMENT_TEXT }) },
        ],
        async (kb, fake) => {
            await callTool(kb, "kb_get", { id: "C-99812" });
            await callTool(kb, "kb_get", { id: "D-241" });
            assert.equal(fake.calls()[0].argv[0], "chunk");
            assert.equal(fake.calls()[1].argv[0], "get");
        },
    );
});

test("a document read carries the whole text, because that is what it is for", async () => {
    /* The other half of §4's discipline: search is the list and returns
     * snippets; this is how a caller asks for the whole thing, deliberately,
     * one item at a time. */
    await withKb(
        [{ stdout: ok({ document: DOCUMENT, text: DOCUMENT_TEXT }) }],
        async (kb, fake) => {
            const result = await callTool(kb, "kb_get", { id: "D-241" });
            assert.ok(result.content[0].text.includes("THE-WHOLE-DOCUMENT"));
            assert.equal(flag(fake.calls()[0].argv, "--include"), "text");
        },
    );
});

test("a caller that asked for the metadata alone does not get the text", async () => {
    await withKb([{ stdout: ok({ document: DOCUMENT }) }], async (kb, fake) => {
        const result = await callTool(kb, "kb_get", { id: "D-241", include: [] });
        assert.equal(fake.calls()[0].argv.includes("--include"), false);
        assert.equal(result.content[0].text.includes("THE-WHOLE-DOCUMENT"), false);
    });
});

test("a document's links are asked for when §2's third include names them", async () => {
    await withKb(
        [{ stdout: ok({ document: DOCUMENT, text: DOCUMENT_TEXT }) }],
        async (kb, fake) => {
            await callTool(kb, "kb_get", { id: "D-241", include: ["text", "links"] });
            assert.equal(flag(fake.calls()[0].argv, "--include"), "text,links");
        },
    );
});

test("chunks are asked for only when they were asked for", async () => {
    await withKb(
        [{ stdout: ok({ document: DOCUMENT, text: DOCUMENT_TEXT, chunks: [CHUNK] }) }],
        async (kb, fake) => {
            await callTool(kb, "kb_get", { id: "D-241", include: ["text", "chunks"] });
            assert.equal(flag(fake.calls()[0].argv, "--include"), "text,chunks");
        },
    );
});

test("a chunk read takes its neighbours and not an include list", async () => {
    await withKb([{ stdout: ok({ chunk: CHUNK, neighbours: [] }) }], async (kb, fake) => {
        await callTool(kb, "kb_get", { id: "C-99812", expand: 2 });
        const argv = fake.calls()[0].argv;
        assert.deepEqual(argv, ["chunk", "C-99812", "--expand", "2", "--json"]);
    });
});

test("an id of a kind §9 has no route for is refused, and the message says which kinds there are", async () => {
    await withKb([{ stdout: ok({}) }], async (kb, fake) => {
        for (const id of ["S-3", "241", "", "d-241"]) {
            const e = (await callTool(kb, "kb_get", { id }).catch((x: unknown) => x)) as RpcError;
            assert.ok(e instanceof RpcError, `"${id}" was accepted`);
            assert.match(e.message, /C-n.*D-n|D-n.*C-n/s);
        }
        assert.equal(fake.calls().length, 0);
    });
});

/* ------------------------------------------------------------------ kb_add */

test("kb_add names no destination, so the working directory's store decides", async () => {
    /* There is one store, the `.kb/` the CLI finds by walking up from the
     * server's working directory. A write that named anywhere else — the
     * `--store` flag that once chose a tier — no longer exists on the CLI, and
     * a surface that still sent it would fail every filing. */
    await withKb([{ stdout: ok(ADDED) }], async (kb, fake) => {
        await callTool(kb, "kb_add", {
            documents: [{ title: "IOCP", content: "# IOCP\n", collection: "win32-iocp" }],
        });
        const argv = fake.calls()[0].argv;
        assert.equal(argv.includes("--store"), false, "kb_add named a store");
    });
});

test("kb_add with no store to file into is a refusal the agent can read", async () => {
    /* NOT CREATED, AND NOT A TRANSPORT ERROR. Where a store lives is the
     * reader's decision and `kb init` is not one of §9's six, so the CLI's
     * `not_found` is the answer — and it has to arrive as a result the model
     * can read and relay ("there is no store here; run kb init"), with nothing
     * claimed as filed, rather than as a broken tool it will call again. */
    await withKb(
        [
            {
                stdout: refusal(
                    "not_found",
                    'no kb store at or above the current directory (run "kb init")',
                ),
                exit: 1,
            },
        ],
        async (kb, fake) => {
            const result = await callTool(kb, "kb_add", {
                documents: [{ title: "IOCP", content: "# IOCP\n", collection: "win32-iocp" }],
            });
            assert.equal(result.isError, true);
            const answer = body(result);
            assert.equal(answer["filed"], 0);
            assert.deepEqual(answer["added"], []);
            const because = answer["because"] as Record<string, unknown>;
            assert.equal(because["kind"], "refused");
            assert.equal(because["error"], "not_found");
            assert.match(String(because["message"]), /kb init/);
            assert.equal(fake.calls().length, 1);
        },
    );
});

test("the documents go down stdin as one batch, and none of their text is an argument", async () => {
    const content = `# IOCP\n${"x".repeat(5000)}\n`;
    await withKb([{ stdout: ok({ added: [ADDED], count: 1 }) }], async (kb, fake) => {
        await callTool(kb, "kb_add", {
            documents: [
                {
                    title: "IOCP",
                    content,
                    collection: "win32-iocp",
                    url: "https://learn.microsoft.test/win32/iocp",
                    mime: "text/markdown",
                    meta: { authors: ["MSDN"], year: 2026 },
                },
            ],
        });
        const call = fake.calls()[0];
        assert.deepEqual(call.argv, ["add", "--batch", "--json"]);
        assert.deepEqual(JSON.parse(call.stdin.trimEnd()), {
            title: "IOCP",
            collection: "win32-iocp",
            content,
            url: "https://learn.microsoft.test/win32/iocp",
            mime: "text/markdown",
            meta: { authors: ["MSDN"], year: 2026 },
        });
    });
});

test("several documents are filed as one batch, in the order they were given", async () => {
    await withKb(
        [{ stdout: ok({ added: [{ ...ADDED, document: "D-1" }, { ...ADDED, document: "D-2" }], count: 2 }) }],
        async (kb, fake) => {
            const answer = body(
                await callTool(kb, "kb_add", {
                    documents: [
                        { title: "one", content: "a", collection: "c" },
                        { title: "two", content: "b", collection: "c" },
                    ],
                }),
            );
            assert.equal(answer["filed"], 2);
            assert.equal(fake.calls().length, 1, "one call for the whole batch");
            assert.deepEqual(
                fake.calls()[0].stdin.trimEnd().split("\n").map((l) => JSON.parse(l).content),
                ["a", "b"],
            );
            assert.deepEqual(
                (answer["added"] as Record<string, unknown>[]).map((a) => a["document"]),
                ["D-1", "D-2"],
            );
        },
    );
});

test("a refused batch files nothing, and the answer says so", async () => {
    /* One transaction: the store refuses the batch, and an agent must be able
     * to tell that none of its documents went in rather than guess which. */
    await withKb(
        [{ stdout: refusal("usage", "kb: batch line 2: a document needs a title"), exit: 1 }],
        async (kb, fake) => {
            const result = await callTool(kb, "kb_add", {
                documents: [
                    { title: "one", content: "a", collection: "c" },
                    { title: "two", content: "b", collection: "c" },
                ],
            });
            assert.equal(result.isError, true);
            const answer = body(result);
            assert.equal(answer["filed"], 0);
            assert.deepEqual(answer["added"], []);
            assert.equal((answer["because"] as Record<string, unknown>)["error"], "usage");
            assert.match(String(answer["note"]), /none of them/);
            assert.equal(fake.calls().length, 1);
        },
    );
});

test("a document missing its collection is refused before anything is filed", async () => {
    await withKb([{ stdout: ok(ADDED) }], async (kb, fake) => {
        await assert.rejects(
            () => callTool(kb, "kb_add", { documents: [{ title: "t", content: "c" }] }),
            RpcError,
        );
        await assert.rejects(() => callTool(kb, "kb_add", { documents: [] }), RpcError);
        assert.equal(fake.calls().length, 0);
    });
});

test("a folder is filed with --no-forget, always, and its answer comes back whole", async () => {
    /* §9: forgetting is not an agent's decision. The walk would forget a file
     * gone from the folder by default, so the flag that stops it is on every
     * folder filing this server makes, and the file comes back as `missing`. */
    await withKb([{ stdout: ok(DIR_ADDED) }], async (kb, fake) => {
        const answer = body(await callTool(kb, "kb_add", { dir: "cli/kb-cli", collection: "code" }));
        const call = fake.calls()[0];
        assert.deepEqual(call.argv, [
            "add",
            "--dir",
            "cli/kb-cli",
            "--collection",
            "code",
            "--no-forget",
            "--json",
        ]);
        assert.equal(call.stdin, "", "a folder's files are read by the store, not handed down stdin");
        assert.equal(answer["source"], "S-4");
        assert.deepEqual(answer["missing"], ["src/old.c"]);
        assert.deepEqual(answer["forgotten"], []);
        assert.equal((answer["skipped"] as Record<string, unknown>)["large"], 1);
    });
});

test("a folder and documents are two ways to file, and one call is one of them", async () => {
    await withKb([{ stdout: ok(DIR_ADDED) }], async (kb, fake) => {
        for (const args of [
            // Both forms at once.
            {
                dir: "/x",
                collection: "code",
                documents: [{ title: "t", content: "c", collection: "c" }],
            },
            // A folder with no topic to file it under.
            { dir: "/x" },
            { dir: "/x", collection: "  " },
            // A blank folder.
            { dir: "", collection: "code" },
            // A collection with nothing to file under it.
            { collection: "code" },
            { documents: [{ title: "t", content: "c", collection: "c" }], collection: "code" },
            // Neither.
            {},
            // Forgetting cannot be asked for.
            { dir: "/x", collection: "code", forget: true },
        ]) {
            await assert.rejects(() => callTool(kb, "kb_add", args), RpcError, JSON.stringify(args));
        }
        assert.equal(fake.calls().length, 0);
    });
});

test("a folder the store cannot file is a refusal the agent can read", async () => {
    await withKb([{ stdout: refusal("not_found", "/nope is not a directory"), exit: 1 }], async (kb, fake) => {
        const result = await callTool(kb, "kb_add", { dir: "/nope", collection: "code" });
        assert.equal(result.isError, true);
        const answer = body(result);
        assert.equal(answer["kind"], "refused");
        assert.equal(answer["error"], "not_found");
        assert.equal(fake.calls().length, 1);
    });
});

/* ---------------------------------------------------------- kb_collections */

test("kb_collections answers both of §9's routes as one row per collection", async () => {
    /* §9 puts `GET /collections` and `GET /stats` behind this one tool, and
     * the CLI prints them as two commands whose rows overlap and neither of
     * which contains the other. Picking one would either report a chunk count
     * of zero for a full topic or drop the date that says whether it has been
     * looked at this year. */
    await withKb(
        [
            { stdout: ok({ collections: COLLECTIONS, count: 2 }) },
            { stdout: ok({ collections: STATS, count: 2, totals: TOTALS }) },
        ],
        async (kb, fake) => {
            const answer = body(await callTool(kb, "kb_collections", {}));
            const rows = answer["collections"] as Record<string, unknown>[];
            assert.equal(answer["count"], 2);
            assert.equal(rows[0]["name"], "win32-iocp");
            assert.equal(rows[0]["documents"], 7);
            assert.equal(rows[0]["oldestFetchedAt"], "2024-01-02T00:00:00Z");
            /* Joined on the name, which §1.3 makes unique within the store. */
            assert.equal(rows[0]["chunks"], 41);
            assert.equal(rows[1]["name"], "io-uring");
            assert.equal(rows[1]["chunks"], 19);
            assert.deepEqual(answer["totals"], TOTALS);
            assert.deepEqual(fake.calls().map((c) => c.argv[0]), ["collections", "stats"]);
            assert.deepEqual(fake.calls()[0].argv, ["collections", "--json"]);
            assert.deepEqual(fake.calls()[1].argv, ["stats", "--json"]);
        },
    );
});

test("a collection stats does not list gets no chunk count rather than a borrowed one", async () => {
    /* The join is on the name and nothing else. A row `kb stats` did not
     * print is left without `chunks` — absent, the honest answer — rather than
     * given zero, or given the count of whichever row happened to be nearest. */
    await withKb(
        [
            {
                stdout: ok({
                    collections: [{ name: "win32-iocp", documents: 1, bytes: 10 }],
                }),
            },
            {
                stdout: ok({
                    collections: [{ name: "io-uring", documents: 9, chunks: 99, bytes: 900 }],
                    totals: { documents: 9, chunks: 99, bytes: 900 },
                }),
            },
        ],
        async (kb) => {
            const answer = body(await callTool(kb, "kb_collections", {}));
            const rows = answer["collections"] as Record<string, unknown>[];
            assert.equal(rows[0]["name"], "win32-iocp");
            assert.equal("chunks" in rows[0], false, "a count crossed between collections");
        },
    );
});

/* --------------------------------------------------------------- kb_links */

test("kb_links reads both directions of a document's links", async () => {
    await withKb([{ stdout: ok(LINKS) }], async (kb, fake) => {
        const answer = body(await callTool(kb, "kb_links", { op: "list", document: "D-241" }));
        assert.equal((answer["outgoing"] as unknown[]).length, 1);
        assert.equal((answer["incoming"] as unknown[]).length, 1);
        assert.deepEqual(fake.calls()[0].argv, ["links", "D-241", "--json"]);
    });
});

test("kb_links writes an edge into the one store", async () => {
    await withKb(
        [
            {
                stdout: ok({
                    action: "add",
                    from: "D-241",
                    type: "analogue_of",
                    to: "D-88",
                    changed: true,
                    at: "2026-09-25T00:00:00Z",
                }),
            },
        ],
        async (kb, fake) => {
            const answer = body(
                await callTool(kb, "kb_links", {
                    op: "add",
                    from: "D-241",
                    to: "D-88",
                    type: "analogue_of",
                }),
            );
            assert.equal(answer["type"], "analogue_of");
            assert.equal(answer["changed"], true);
            assert.deepEqual(fake.calls()[0].argv, [
                "links",
                "add",
                "D-241",
                "analogue_of",
                "D-88",
                "--json",
            ]);
            assert.equal(fake.calls()[0].argv.includes("--store"), false);
        },
    );
});

test("a link of a type §6 does not name is refused", async () => {
    await withKb([{ stdout: ok({}) }], async (kb, fake) => {
        await assert.rejects(
            () =>
                callTool(kb, "kb_links", {
                    op: "add",
                    from: "D-1",
                    to: "D-2",
                    type: "relates_to",
                }),
            RpcError,
        );
        assert.equal(fake.calls().length, 0);
    });
});

test("the arguments of one operation are refused on the other", async () => {
    /* A `document` on an add, or a `from` on a list, is a caller who thinks
     * they asked for something. Ignoring it would answer a different question
     * and say nothing. */
    await withKb([{ stdout: ok({}) }], async (kb, fake) => {
        await assert.rejects(
            () => callTool(kb, "kb_links", { op: "list", document: "D-1", from: "D-2" }),
            RpcError,
        );
        await assert.rejects(
            () =>
                callTool(kb, "kb_links", {
                    op: "add",
                    from: "D-1",
                    to: "D-2",
                    type: "cites",
                    document: "D-3",
                }),
            RpcError,
        );
        assert.equal(fake.calls().length, 0);
    });
});

/* ------------------------------------------------------------ one store */

test("`store` is an unknown key on every tool, and nothing reaches kb", async () => {
    /* THERE IS ONE STORE AND NO SELECTOR FOR IT. The `store` argument that
     * once chose between a project tier and a global one is gone, and a caller
     * still sending it is refused like any other misspelt key — out loud, as a
     * fault in the call — rather than having it dropped, which would let the
     * caller believe it had narrowed a read or steered a write. */
    const calls: Record<string, Record<string, unknown>> = {
        kb_search: { q: "iocp" },
        kb_get: { id: "D-241" },
        kb_add: { documents: [{ title: "t", content: "c", collection: "k" }] },
        kb_collections: {},
        kb_links: { op: "list", document: "D-241" },
        kb_stale: {},
    };
    await withKb([{ stdout: ok({}) }], async (kb, fake) => {
        for (const [name, args] of Object.entries(calls)) {
            for (const store of ["project", "global", "all"]) {
                const e = (await callTool(kb, name, { ...args, store }).catch(
                    (x: unknown) => x,
                )) as RpcError;
                assert.ok(e instanceof RpcError, `${name} accepted store: "${store}"`);
                assert.equal(e.code, INVALID_PARAMS);
                assert.match(e.message, /no argument called "store"/);
            }
        }
        /* And inside a document to file, which has its own key list. */
        await assert.rejects(
            () =>
                callTool(kb, "kb_add", {
                    documents: [{ title: "t", content: "c", collection: "k", store: "global" }],
                }),
            RpcError,
        );
        assert.equal(fake.calls().length, 0);
    });
});

/* --------------------------------------------------------------- kb_stale */

test("kb_stale passes §5's threshold in §5's own spelling, and answers it back", async () => {
    /* THE THRESHOLD IS PART OF THE ANSWER. "one document is stale" means
     * nothing without "older than what", and an agent that sent no `olderThan`
     * cannot say which default the store applied — so the row set and the
     * threshold that produced it travel together. */
    await withKb(
        [
            {
                stdout: ok({
                    documents: [DOCUMENT_OLD],
                    count: 1,
                    olderThan: "90d",
                    staleBefore: "2026-06-27T00:00:00Z",
                }),
            },
        ],
        async (kb, fake) => {
            const answer = body(
                await callTool(kb, "kb_stale", {
                    olderThan: "90d",
                    collection: "io-uring",
                }),
            );
            assert.equal(answer["count"], 1);
            assert.equal(answer["olderThan"], "90d");
            assert.equal(answer["staleBefore"], "2026-06-27T00:00:00Z");
            assert.equal((answer["documents"] as unknown[]).length, 1);
            assert.deepEqual(fake.calls()[0].argv, [
                "stale",
                "--older-than",
                "90d",
                "--collection",
                "io-uring",
                "--json",
            ]);
        },
    );
});

/* ----------------------------------------------- the error boundary itself */

test("a store that refused a well-formed call is a result the model can read", async () => {
    /* NOT A TRANSPORT ERROR. The tool ran and this is what it found out; a
     * client shown a failed call instead would show the model a broken tool,
     * and the model would ask the same question again. */
    await withKb(
        [{ stdout: refusal("not_found", "kb: no such document: D-9999"), exit: 1 }],
        async (kb) => {
            const result = await callTool(kb, "kb_get", { id: "D-9999" });
            assert.equal(result.isError, true);
            const answer = body(result);
            assert.equal(answer["kind"], "refused");
            assert.equal(answer["error"], "not_found");
            assert.match(String(answer["message"]), /D-9999/);
            assert.equal("details" in answer, false, "a refusal without details grew a details field");
        },
    );
});

test("a refusal's details reach the agent as the store sent them", async () => {
    const envelope =
        JSON.stringify({
            ok: false,
            error: "index_stale",
            message: "fts.db no longer describes the logs",
            details: { structures: ["keyword"] },
        }) + "\n";
    await withKb([{ stdout: envelope, exit: 1 }], async (kb) => {
        const answer = body(await callTool(kb, "kb_search", { q: "x" }));
        assert.deepEqual(answer["details"], { structures: ["keyword"] });
    });
});

test("§11's codes arrive verbatim rather than mapped onto something plausible", async () => {
    for (const code of ["model_mismatch", "index_stale", "store_locked", "unknown_command"]) {
        await withKb([{ stdout: refusal(code, `kb: ${code}`), exit: 1 }], async (kb) => {
            const answer = body(await callTool(kb, "kb_search", { q: "x" }));
            assert.equal(answer["error"], code);
        });
    }
});

test("a fault in kb is told apart from a refusal, because they mean different things", async () => {
    /* §10's exit codes: 1 is something the caller asked for that the store
     * will not do, 2 is a bug in kb. A surface that folded them together would
     * tell the reader to fix input that was never the problem. */
    await withKb([{ stderr: "kb: assertion failed\n", exit: 2 }], async (kb) => {
        const result = await callTool(kb, "kb_search", { q: "x" });
        assert.equal(result.isError, true);
        const answer = body(result);
        assert.equal(answer["kind"], "failed");
        assert.equal(answer["exitCode"], 2);
        assert.match(String(answer["stderr"]), /assertion failed/);
    });
});

test("a kb that is not on the PATH is a fault and not a refusal", async () => {
    const kb = new Kb({ bin: "/nonexistent/kb-should-not-exist" });
    const result = await callTool(kb, "kb_collections", {});
    assert.equal(result.isError, true);
    assert.equal(body(result)["kind"], "failed");
});

test("a tool that does not exist is a transport error and never a result", async () => {
    /* THE MUTATION: returning `{isError: true}` here instead of throwing. A
     * client would show the model a tool that answered, and the model would
     * believe there is a `kb_rebuild` that simply failed this time. */
    await withKb([{ stdout: ok({}) }], async (kb, fake) => {
        for (const name of ["kb_rebuild", "kb_serach", "kb_delete", ""]) {
            const e = (await callTool(kb, name, {}).catch((x: unknown) => x)) as RpcError;
            assert.ok(e instanceof RpcError, `"${name}" came back as a result`);
            assert.equal(e.code, INVALID_PARAMS);
        }
        assert.equal(fake.calls().length, 0);
    });
});

test("a successful call is a result with no isError on it at all", async () => {
    await withKb([{ stdout: ok({ hits: [], count: 0 }) }], async (kb) => {
        const result = await callTool(kb, "kb_search", { q: "x" });
        assert.equal(result.isError, undefined);
    });
});

test("stderr from a failing binary cannot fill the caller's context", async () => {
    await withKb([{ stderr: "x".repeat(100000), exit: 2 }], async (kb) => {
        const answer = body(await callTool(kb, "kb_collections", {}));
        assert.ok(String(answer["stderr"]).length <= 2000);
    });
});
