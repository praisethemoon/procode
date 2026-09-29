/* §6's layer, and §7's second half.
 *
 * THE SPELLINGS ARE THE CLI'S OWN AND WERE RECONCILED AGAINST IT. `kb links`
 * takes its positional arguments directly rather than through the common
 * parser, so — unlike `collections rename` — a `--` is a fourth argument to a
 * command that takes three; `argv.ts` records why that is the one place this
 * package writes a bare value.
 *
 * THE EDGE IS NOT THE ROW. §6 resolves a link "to rows", and the two facts a
 * caller needs are different: the edge says what the relationship is, and the
 * row at the far end says what it points at. A reader that folded them would
 * have nowhere to put a dangling edge.
 */

import * as assert from "node:assert/strict";
import { test as base } from "node:test";

/* The stand-in kb (fake.ts) is a #! script, which Windows cannot start, so
 * this file's tests run on macOS and Linux only. */
const test = process.platform === "win32" ? base.skip : base;

import { linkArgv, linksArgv, statsArgv } from "../argv";
import { Kb } from "../client";
import { KbError } from "../errors";
import { readLink, readLinks, readStats } from "../shape";
import { LINK_TYPES, isLinkType } from "../types";
import { FakeAnswer, FakeKb, ok, refusal } from "./fake";
import { DOCUMENT, DOCUMENT_OLD } from "./fixtures";

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

/* ------------------------------------------------------------- the argv */

test("§6's five types are transcribed whole and in the document's order", () => {
    assert.deepEqual(
        [...LINK_TYPES],
        ["supersedes", "cites", "analogue_of", "implements", "see_also"],
    );
    assert.equal(isLinkType("analogue_of"), true);
    assert.equal(isLinkType("relates_to"), false);
    assert.equal(isLinkType(7), false);
});

test("a read names its document positionally, the way the command reads it", () => {
    assert.deepEqual(linksArgv("D-241"), ["links", "D-241"]);
});

test("a write reads as a sentence: from, type, to", () => {
    /* §6 writes the body `{ from, to, type }`; three bare ids and a word in a
     * row at a terminal are a thing somebody has to get right, and only this
     * ordering can be read back to check. */
    assert.deepEqual(linkArgv("D-241", "analogue_of", "D-88"), [
        "links",
        "add",
        "D-241",
        "analogue_of",
        "D-88",
    ]);
});

test("a hostile id is one argument of the link commands too", () => {
    const hostile = 'D-1; rm -rf ~/dummy && echo "$(id)"';
    for (const argv of [linksArgv(hostile), linkArgv(hostile, hostile, hostile)]) {
        assert.equal(argv.filter((a) => a === hostile).length >= 1, true);
        for (const element of argv) {
            if (element.startsWith("-") && element !== "--") {
                assert.ok(!/\s/.test(element), `a flag was built with whitespace in it: ${element}`);
            }
        }
    }
});

/* ------------------------------------------------------------ the answer */

test("a read carries both directions, each resolved to the row at the far end", async () => {
    await withKb(
        [
            {
                stdout: ok({
                    document: "D-241",
                    outgoing: [
                        {
                            from: "D-241",
                            to: "D-88",
                            type: "analogue_of",
                            resolved: true,
                            document: DOCUMENT_OLD,
                            createdAt: "2026-09-25T00:00:00Z",
                        },
                    ],
                    incoming: [
                        {
                            from: "D-88",
                            to: "D-241",
                            type: "cites",
                            resolved: true,
                            document: DOCUMENT_OLD,
                            createdAt: "2026-09-25T00:00:00Z",
                        },
                    ],
                }),
            },
        ],
        async (kb, fake) => {
            const links = await kb.links("D-241");
            assert.equal(links.document, "D-241");
            assert.equal(links.outgoing.length, 1);
            assert.equal(links.incoming.length, 1);
            assert.equal(links.outgoing[0].type, "analogue_of");
            assert.equal(links.outgoing[0].to, "D-88");
            assert.equal(links.outgoing[0].document?.title, "io_uring and you");
            assert.equal(links.outgoing[0].document?.collection, "io-uring");
            assert.equal(links.incoming[0].from, "D-88");
            assert.equal(links.outgoing[0].createdAt, "2026-09-25T00:00:00Z");
            assert.equal(links.outgoing[0].resolved, true);
            assert.deepEqual(fake.calls()[0].argv, ["links", "D-241", "--json"]);
        },
    );
});

test("an edge the store could not resolve arrives as an edge with no row", async () => {
    /* A dangling link is a fact about the store. A reader that invented an
     * empty row for it would report a document that is not there, and the one
     * person who could go and fix it would never be told. */
    const links = readLinks({
        document: "D-241",
        outgoing: [{ from: "D-241", to: "D-9999", type: "supersedes", resolved: false }],
    });
    assert.equal(links.outgoing[0].document, null);
    assert.equal(links.outgoing[0].resolved, false);
    assert.equal(links.outgoing[0].to, "D-9999");
    assert.deepEqual(links.incoming, []);
});

test("a type this binding has never heard of is still the word the store used", () => {
    /* The same rule `matched` follows: a sixth relationship added later must
     * not be dropped by a reader that only knew five, or an edge would claim a
     * relationship it does not have. */
    const link = readLink({ from: "D-1", to: "D-2", type: "refutes" });
    assert.equal(link.type, "refutes");
    assert.equal(isLinkType(link.type), false);
});

test("a write answers the edge it made, and whether it changed anything", async () => {
    /* `changed: false` is an edge that was already there. Stating a
     * relationship twice costs nothing, which is the same posture §2 takes
     * toward filing the same text twice. */
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
            const link = await kb.link("D-241", "analogue_of", "D-88");
            assert.equal(link.from, "D-241");
            assert.equal(link.to, "D-88");
            assert.equal(link.type, "analogue_of");
            assert.equal(link.changed, true);
            assert.equal(link.at, "2026-09-25T00:00:00Z");
            assert.deepEqual(fake.calls()[0].argv, [
                "links",
                "add",
                "D-241",
                "analogue_of",
                "D-88",
                "--json",
            ]);
        },
    );
});

test("§7's stats are the counts collections does not carry", async () => {
    /* The two commands answer overlapping questions and neither contains the
     * other: `collections` has the oldest fetch date, `stats` has the chunk
     * count and a store-wide total. */
    await withKb(
        [
            {
                stdout: ok({
                    collections: [
                        { name: "win32-iocp", documents: 7, chunks: 41, bytes: 90210 },
                    ],
                    count: 1,
                    totals: { documents: 7, chunks: 41, bytes: 90210 },
                }),
            },
        ],
        async (kb, fake) => {
            const stats = await kb.stats();
            assert.equal(stats.collections[0].chunks, 41);
            assert.equal(stats.totals.documents, 7);
            assert.deepEqual(fake.calls()[0].argv, ["stats", "--json"]);
            assert.deepEqual(statsArgv(), ["stats"]);
        },
    );
});

test("a count the store did not carry is absent rather than zero", async () => {
    /* `kb collections` prints no chunk count. A reader that defaulted it to
     * zero would report a full topic as confidently as an empty one. */
    await withKb(
        [
            {
                stdout: ok({
                    collections: [
                        {
                            name: "win32-iocp",
                            documents: 7,
                            bytes: 90210,
                            oldestFetchedAt: "2024-01-02T00:00:00Z",
                        },
                    ],
                }),
            },
        ],
        async (kb) => {
            const [row] = await kb.collections();
            assert.equal(row.documents, 7);
            assert.equal(row.oldestFetchedAt, "2024-01-02T00:00:00Z");
            assert.equal("chunks" in row, false);
            assert.equal("sources" in row, false);
            assert.equal(readStats({}).totals.chunks, 0);
        },
    );
});

test("until the command exists the refusal reaches the caller as a refusal", async () => {
    /* `unknown_command` is exit 1 — something the caller asked for that this
     * binary will not do — so it must arrive as a KbError a surface can put in
     * a sentence, and never as a crash that reads like a bug in kb. */
    await withKb(
        [{ stdout: refusal("unknown_command", "kb: unknown command: links"), exit: 1 }],
        async (kb) => {
            const e = (await kb.links("D-241").catch((x: unknown) => x)) as KbError;
            assert.ok(e instanceof KbError);
            assert.equal(e.code, "unknown_command");
            assert.equal(e.spec, null);
            assert.equal(e.unrecognised, false);
        },
    );
});

test("a link into a document that is not there is §11's not_found", async () => {
    await withKb(
        [{ stdout: refusal("not_found", "kb: no such document: D-9999"), exit: 1 }],
        async (kb) => {
            const e = (await kb
                .link(DOCUMENT.id, "cites", "D-9999")
                .catch((x: unknown) => x)) as KbError;
            assert.ok(e instanceof KbError);
            assert.equal(e.spec, "not_found");
        },
    );
});
