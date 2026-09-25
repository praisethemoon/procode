/* §6's layer, which is two commands the CLI does not have yet.
 *
 * SO EVERYTHING HERE IS A CLAIM AND IS WRITTEN AS ONE. `argv.ts` records the
 * spelling each route takes in the CLI's idiom; these tests pin that spelling
 * and pin what this reader does with the answer, so that the day `kb links`
 * lands the reconciliation is one file of flags rather than a hunt through a
 * binding for assumptions nobody wrote down. `cli.test.ts` starts checking the
 * real binary the moment the command turns up in `kb --help`.
 *
 * THE EDGE IS NOT THE ROW. §6 resolves a link "to rows", and the two facts a
 * caller needs are different: the edge says what the relationship is, and the
 * row at the far end says what it points at. A reader that folded them would
 * have nowhere to put a dangling edge.
 */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import { linkArgv, linksArgv } from "../argv";
import { Kb } from "../client";
import { KbError } from "../errors";
import { readLink, readLinks } from "../shape";
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

test("a read names its document behind the end of the flags", () => {
    assert.deepEqual(linksArgv("D-241"), ["links", "--", "D-241"]);
    assert.deepEqual(linksArgv("D-241", "global"), ["links", "--store", "global", "--", "D-241"]);
});

test("a write reads as a sentence: from, type, to", () => {
    /* §6 writes the body `{ from, to, type }`; three bare ids and a word in a
     * row at a terminal are a thing somebody has to get right, and only this
     * ordering can be read back to check. */
    assert.deepEqual(linkArgv("D-241", "analogue_of", "D-88"), [
        "links",
        "add",
        "--",
        "D-241",
        "analogue_of",
        "D-88",
    ]);
    assert.deepEqual(linkArgv("D-241", "cites", "D-88", "project"), [
        "links",
        "add",
        "--store",
        "project",
        "--",
        "D-241",
        "cites",
        "D-88",
    ]);
});

test("a hostile id is one argument of the link commands too", () => {
    const hostile = 'D-1; rm -rf ~ && echo "$(id)"';
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
                    outgoing: [{ from: "D-241", to: "D-88", type: "analogue_of", document: DOCUMENT_OLD }],
                    incoming: [{ from: "D-88", to: "D-241", type: "cites", document: DOCUMENT_OLD }],
                }),
            },
        ],
        async (kb, fake) => {
            const links = await kb.links("D-241");
            assert.equal(links.outgoing.length, 1);
            assert.equal(links.incoming.length, 1);
            assert.equal(links.outgoing[0].type, "analogue_of");
            assert.equal(links.outgoing[0].to, "D-88");
            assert.equal(links.outgoing[0].document?.title, "io_uring and you");
            assert.equal(links.outgoing[0].document?.store, "global");
            assert.equal(links.incoming[0].from, "D-88");
            assert.deepEqual(fake.calls()[0].argv, ["links", "--", "D-241", "--json"]);
        },
    );
});

test("an edge the store could not resolve arrives as an edge with no row", async () => {
    /* A dangling link is a fact about the store. A reader that invented an
     * empty row for it would report a document that is not there, and the one
     * person who could go and fix it would never be told. */
    const links = readLinks({ outgoing: [{ from: "D-241", to: "D-9999", type: "supersedes" }] });
    assert.equal(links.outgoing[0].document, null);
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

test("a write answers the edge it made", async () => {
    await withKb(
        [{ stdout: ok({ link: { from: "D-241", to: "D-88", type: "analogue_of", document: DOCUMENT_OLD } }) }],
        async (kb, fake) => {
            const link = await kb.link("D-241", "analogue_of", "D-88");
            assert.equal(link.from, "D-241");
            assert.equal(link.to, "D-88");
            assert.equal(link.type, "analogue_of");
            assert.equal(link.document?.id, "D-88");
            assert.deepEqual(fake.calls()[0].argv, [
                "links",
                "add",
                "--",
                "D-241",
                "analogue_of",
                "D-88",
                "--json",
            ]);
        },
    );
});

test("a write that answered the edge at the top level is read the same way", async () => {
    /* The command does not exist, so which of the two envelopes it will use is
     * not yet settled. Reading either costs one `??` and removes a reason for
     * the day it lands to be a day of edits. */
    await withKb([{ stdout: ok({ from: "D-1", to: "D-2", type: "cites" }) }], async (kb) => {
        const link = await kb.link("D-1", "cites", "D-2");
        assert.equal(link.from, "D-1");
        assert.equal(link.type, "cites");
    });
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
