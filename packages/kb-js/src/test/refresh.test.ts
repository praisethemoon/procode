/* §5's two routes, and the answer that is a report rather than an action.
 *
 * THE BUG THESE ARE WRITTEN AGAINST WAS INVISIBLE, and the shape of it is
 * worth keeping in front of whoever edits this next. `Kb.refresh` read
 * `refreshed` and `changed`; `kb refresh` prints neither; both defaulted to 0;
 * and 0 is the correct number for a command that refetches nothing. The reader
 * was wrong and every number it produced was right. It would have kept being
 * right in appearance and become wrong in fact the day refresh started
 * acting — which is the worst possible moment for a reader to be discovered
 * wrong, because that is the day somebody trusts it.
 *
 * `cli.test.ts` is where the class is caught for good: it compares the reader's
 * own keys against the binary's own keys and needs no fixture to agree with.
 * These are the fast checks over the shapes.
 */

import * as assert from "node:assert/strict";
import { test as base } from "node:test";

/* The stand-in kb (fake.ts) is a #! script, which Windows cannot start, so
 * this file's tests run on macOS and Linux only. */
const test = process.platform === "win32" ? base.skip : base;

import { refreshArgv, staleArgv } from "../argv";
import { Kb } from "../client";
import { readRefresh, readStaleList } from "../shape";
import { FakeAnswer, FakeKb, ok } from "./fake";
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

/* The answer `kb refresh --json` gives, field for field. */
const REPORT = {
    action: "report",
    refetched: 0,
    reembedded: 0,
    sources: [
        {
            id: "S-3",
            kind: "url",
            locator: "https://learn.microsoft.test/win32/iocp",
            collection: "win32-iocp",
            staleDocuments: 2,
            fetchedAt: "2024-01-02T00:00:00Z",
            refetchBy: "POST /sources/S-3/refresh",
        },
    ],
    count: 1,
    staleDocuments: 2,
    olderThan: "90d",
    staleBefore: "2026-06-27T00:00:00Z",
    note: 'this is a report, not an action: kb has no HTTP client and fetched nothing. Re-file the content through "kb add" to bring a source up to date.',
};

/* ---------------------------------------------------------------- the argv */

test("refresh sends §5's two narrowings and nothing it invented", () => {
    /* `--document` and `--source` were here. `kb refresh` refuses both with
     * `usage: unknown option`, and §3.1's per-document action is §2's
     * `POST /sources/{id}/refresh` — a different route, not a filter on this
     * one. A narrowing that reported about one source while looking like it
     * had refetched it would be the worst of the three options. */
    assert.deepEqual(refreshArgv(), ["refresh"]);
    assert.deepEqual(
        refreshArgv({ collection: "win32-iocp", olderThan: "90d" }),
        ["refresh", "--collection", "win32-iocp", "--older-than", "90d"],
    );
    const everything = refreshArgv({ collection: "c", olderThan: "1d" });
    assert.equal(everything.includes("--document"), false);
    assert.equal(everything.includes("--source"), false);
});

/* -------------------------------------------------------------- the report */

test("the reader carries the sentence that says nothing was fetched", async () => {
    await withKb([{ stdout: ok(REPORT) }], async (kb, fake) => {
        const report = await kb.refresh({ olderThan: "90d" });
        assert.equal(report.action, "report");
        assert.match(report.note, /report, not an action/);
        assert.equal(report.refetched, 0);
        assert.equal(report.reembedded, 0);
        assert.equal(report.count, 1);
        assert.equal(report.staleDocuments, 2);
        assert.equal(report.olderThan, "90d");
        assert.equal(report.staleBefore, "2026-06-27T00:00:00Z");
        assert.deepEqual(fake.calls()[0].argv, ["refresh", "--older-than", "90d", "--json"]);
    });
});

test("a source row says which source, how stale, and how to fix it", async () => {
    /* `refetchBy` is the store answering "here is how" rather than a caller
     * working it out from `kind`. */
    await withKb([{ stdout: ok(REPORT) }], async (kb) => {
        const [source] = (await kb.refresh()).sources;
        assert.equal(source.id, "S-3");
        assert.equal(source.kind, "url");
        assert.equal(source.collection, "win32-iocp");
        assert.equal(source.staleDocuments, 2);
        assert.equal(source.fetchedAt, "2024-01-02T00:00:00Z");
        assert.equal(source.refetchBy, "POST /sources/S-3/refresh");
    });
});

test("an answer with neither key reads as zero, and says so in three ways", async () => {
    /* The defect, stated as what must stay true. Two zeroes on their own are
     * indistinguishable from a refresh that found nothing to do; the verb and
     * the sentence are what tell them apart, so a reader that answered the
     * numbers without them would be no better than the one that invented
     * them. */
    const report = readRefresh({ refetched: 0, reembedded: 0 });
    assert.equal(report.refetched, 0);
    assert.equal(report.action, "");
    assert.equal(report.note, "");
    assert.deepEqual(report.sources, []);
});

/* --------------------------------------------------------------- the stale */

test("stale answers the threshold along with the rows", async () => {
    /* "18 documents are stale" means nothing without "older than what", and a
     * caller that sent no `olderThan` cannot say which default it got. */
    await withKb(
        [
            {
                stdout: ok({
                    documents: [DOCUMENT_OLD, DOCUMENT],
                    count: 2,
                    olderThan: "90d",
                    staleBefore: "2026-06-27T00:00:00Z",
                }),
            },
        ],
        async (kb, fake) => {
            const list = await kb.stale({ collection: "io-uring" });
            assert.deepEqual(list.documents.map((d) => d.id), ["D-88", "D-241"]);
            assert.equal(list.count, 2);
            assert.equal(list.olderThan, "90d");
            assert.equal(list.staleBefore, "2026-06-27T00:00:00Z");
            assert.deepEqual(fake.calls()[0].argv, [
                "stale",
                "--collection",
                "io-uring",
                "--json",
            ]);
            assert.deepEqual(staleArgv(), ["stale"]);
        },
    );
});

test("the store's own count wins over the array's length", () => {
    /* A store that limited the list would report a total the array does not
     * have, and answering `documents.length` would quietly redefine the
     * field. The length is the answer only when the store gave no count. */
    assert.equal(readStaleList({ documents: [DOCUMENT], count: 41 }).count, 41);
    assert.equal(readStaleList({ documents: [DOCUMENT] }).count, 1);
});
