/* Against the real binary, through the real protocol.
 *
 * WHAT THIS ADDS OVER THE FAKE. The fake proves what this server does with an
 * answer; this proves the questions are ones the CLI actually accepts. Every
 * flag `kb-js` spells is a claim about a program in another language, and a
 * claim checked only against a fixture can be wrong for as long as nobody runs
 * the real thing.
 *
 * THE STORE IS NOT THE AGENT'S TO CREATE. `kb init` is not one of §9's six, so
 * the workspace below is initialised with the binary directly — which is the
 * arrangement in practice as well: a reader makes a store, and an agent fills
 * it and reads it.
 *
 * IT NEVER TOUCHES A STORE IT WAS NOT ASKED TO. There is one store, the first
 * `.kb/` at or above the working directory, and the working directory is a
 * throwaway one that `kb init` has just made a store in — so the store every
 * call finds is the temporary one, and it exists for the length of the run.
 * Nothing in the environment can redirect it: the CLI reads neither `HOME` nor
 * any store variable.
 *
 * IT SKIPS RATHER THAN FAILS FOR A COMMAND THAT IS NOT THERE YET. §5's `stale`
 * and §6's `links` were built in parallel with this and landed during it, so
 * all six tools are now driven against the real binary. The conditional is
 * kept rather than collapsed: it costs one branch, it is what let this file be
 * written before the commands existed, and the next tool built against a route
 * the CLI has not grown yet gets the same treatment. What it asserts while a
 * command is missing is that the tool degrades into a sentence rather than
 * into a crash, which is what a surface offering the action owes whoever
 * pressed it.
 */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { execFileSync } from "node:child_process";
import { test } from "node:test";

import { Kb } from "kb-js";

import { handle } from "../jsonrpc";
import { Server } from "../server";

const BIN = path.resolve(__dirname, "..", "..", "..", "..", "cli", "kb-cli", "bin", "kb");

/* One throwaway home for every kb this file starts: the CLI reads HOME to
 * find ~/.kb/models (§8), and a model on the developer's machine must not
 * change what these tests see, nor may anything here reach their home. */
const HOME = fs.mkdtempSync(path.join(os.tmpdir(), "kb-mcp-home-"));
process.on("exit", () => fs.rmSync(HOME, { recursive: true, force: true }));
const ENV: NodeJS.ProcessEnv = { ...process.env, HOME };

function built(): boolean {
    return fs.existsSync(BIN);
}

function help(): string {
    return execFileSync(BIN, ["--help"], { encoding: "utf8" });
}

function hasCommand(name: string): boolean {
    return new RegExp(`^\\s{2}${name}\\b`, "m").test(help());
}

interface Work {
    server: Server;
    /* The same directory the server was given, so a test that builds a second
     * client points at the same throwaway store. */
    dir: string;
    dispose(): void;
}

function workspace(): Work {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kb-mcp-real-"));
    const project = path.join(dir, "project");
    fs.mkdirSync(project);
    execFileSync(BIN, ["init", "--json"], { cwd: project, env: ENV });
    return {
        server: new Server(new Kb({ bin: BIN, cwd: project, env: ENV })),
        dir: project,
        dispose: () => fs.rmSync(dir, { recursive: true, force: true }),
    };
}

interface Called {
    content: { type: string; text: string }[];
    isError?: boolean;
}

/* One tool call, all the way through JSON-RPC. */
async function call(work: Work, name: string, args: unknown): Promise<Called> {
    const response = (await handle(
        JSON.stringify({
            jsonrpc: "2.0",
            id: 1,
            method: "tools/call",
            params: { name, arguments: args },
        }),
        work.server.dispatch,
    )) as Record<string, unknown>;
    assert.equal(
        "error" in response,
        false,
        `${name} came back as a transport error: ${JSON.stringify(response["error"])}`,
    );
    return response["result"] as Called;
}

function payload(result: Called): Record<string, unknown> {
    return JSON.parse(result.content[0].text) as Record<string, unknown>;
}

/* The marker sits at the far end of a long document, and the term the search
 * will match sits at the near end. A snippet is a window around the match, so
 * a result that contains the marker is a result that carried the whole thing —
 * which is the §4 violation this document is shaped to catch. */
const IOCP = `# I/O Completion Ports

CreateIoCompletionPort associates an open file handle with a completion port.
A thread calls GetQueuedCompletionStatus to dequeue a packet.

${"Each completion packet carries the number of bytes transferred. ".repeat(120)}

THE-WHOLE-DOCUMENT-MARKER
`;

test("a store filed into and searched through §9's tools", async (t) => {
    if (!built()) {
        t.skip("cli/kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    try {
        /* §2's load-bearing route: the caller already has the text. */
        const added = payload(
            await call(work, "kb_add", {
                documents: [
                    {
                        title: "I/O Completion Ports",
                        content: IOCP,
                        collection: "win32-iocp",
                        url: "https://learn.microsoft.test/win32/iocp",
                        mime: "text/markdown",
                        meta: { authors: ["MSDN"], year: 2026 },
                    },
                ],
            }),
        );
        assert.equal(added["filed"], 1);
        const rows = added["added"] as Record<string, unknown>[];
        /* §1.4: a write goes to the one store the working directory finds,
         * and the row no longer says which store — there is only one. */
        assert.equal("store" in rows[0], false);
        assert.match(String(rows[0]["document"]), /^D-\d+$/);
        const documentId = String(rows[0]["document"]);

        const found = payload(await call(work, "kb_search", { q: "CreateIoCompletionPort" }));
        const hits = found["hits"] as Record<string, unknown>[];
        assert.ok(hits.length >= 1, "the document just filed was not found");
        assert.equal(hits[0]["document"], documentId);
        assert.equal(hits[0]["collection"], "win32-iocp");
        assert.equal("store" in hits[0], false);
        assert.ok(String(hits[0]["snippet"]).length > 0);
        assert.ok((hits[0]["matched"] as string[]).length >= 1);

        /* §4, against the real retrieval path: snippets only. */
        assert.equal(
            JSON.stringify(found).includes("THE-WHOLE-DOCUMENT-MARKER"),
            false,
            "a real search result carried the whole document",
        );

        /* And the deliberate read that does carry it. */
        const whole = payload(await call(work, "kb_get", { id: documentId }));
        assert.match(String(whole["text"]), /THE-WHOLE-DOCUMENT-MARKER/);
        assert.equal(
            (whole["document"] as Record<string, unknown>)["mime"],
            "text/markdown",
        );

        /* §1.1's prefix, on the real chunk the search returned. */
        const chunkId = String(hits[0]["chunk"]);
        assert.match(chunkId, /^C-\d+$/);
        const chunk = payload(await call(work, "kb_get", { id: chunkId }));
        assert.equal((chunk["chunk"] as Record<string, unknown>)["document"], documentId);
        assert.ok(String((chunk["chunk"] as Record<string, unknown>)["text"]).length > 0);

        const collections = payload(await call(work, "kb_collections", {}));
        const list = collections["collections"] as Record<string, unknown>[];
        assert.equal(list.length, 1);
        assert.equal(list[0]["name"], "win32-iocp");
        assert.equal(list[0]["documents"], 1);
        /* §9 puts both of §7's routes behind this tool: the chunk count comes
         * from `kb stats` and the date from `kb collections`, and the row
         * carries both or the join is not happening. */
        assert.ok(Number(list[0]["chunks"]) >= 1, "the row has no chunk count, so stats was not read");
        assert.ok(String(list[0]["oldestFetchedAt"]).length > 0);
        assert.equal((collections["totals"] as Record<string, unknown>)["documents"], 1);
    } finally {
        work.dispose();
    }
});

test("a folder filed through kb_add, relative to the server's directory, and a gone file kept", async (t) => {
    if (!built()) {
        t.skip("cli/kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    /* Beside the project, inside the throwaway `dispose` removes, and named
     * relative to the project, which is the server's working directory. */
    const tree = path.join(work.dir, "..", "tree");
    try {
        fs.mkdirSync(path.join(tree, "src"), { recursive: true });
        fs.writeFileSync(path.join(tree, "README.md"), "# Tree\n\nzzreadme\n");
        fs.writeFileSync(path.join(tree, "src", "ring.c"), "int zzring(void) { return 1; }\n");

        const first = payload(await call(work, "kb_add", { dir: "../tree", collection: "code" }));
        assert.equal(first["root"], fs.realpathSync(tree));
        assert.equal(first["files"], 2);
        assert.equal(first["added"], 2);
        assert.match(String(first["source"]), /^S-\d+$/);

        /* Gone from the folder, and still in the store: the agent is told,
         * and forgetting it is left to the reader. */
        fs.rmSync(path.join(tree, "src", "ring.c"));
        const again = payload(await call(work, "kb_add", { dir: "../tree", collection: "code" }));
        assert.equal(again["source"], first["source"]);
        assert.deepEqual(again["missing"], ["src/ring.c"]);
        assert.deepEqual(again["forgotten"], []);
        const found = payload(await call(work, "kb_search", { q: "zzring" }));
        assert.equal((found["hits"] as unknown[]).length, 1, "a file gone from the folder was forgotten");

        // The tool answers every field the reader it is built on answers.
        const read = await new Kb({ bin: BIN, cwd: work.dir, env: ENV }).addDir("../tree", {
            collection: "code",
            forget: false,
        });
        assert.deepEqual(Object.keys(again).sort(), Object.keys(read).sort());
    } finally {
        work.dispose();
    }
});

test("a filter §4 names is one the real binary accepts", async (t) => {
    if (!built()) {
        t.skip("cli/kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    try {
        await call(work, "kb_add", {
            documents: [{ title: "Ring", content: "io_uring_prep_recv\n", collection: "io-uring" }],
        });
        /* Every filter this tool offers, sent at once. A flag the CLI does not
         * have comes back as a `usage` refusal, which is the failure this
         * exists to catch — `mode` is left off because hybrid retrieval needs
         * the model and the CLI answers keyword until §8 lands. */
        const result = await call(work, "kb_search", {
            q: "io_uring_prep_recv",
            collection: ["io-uring"],
            k: 5,
            expand: 1,
            mime: "text/plain",
            since: "2000-01-01T00:00:00Z",
            minScore: 0,
        });
        assert.notEqual(
            result.isError,
            true,
            `the CLI refused a filter §4 names: ${result.content[0].text}`,
        );
        assert.equal((payload(result)["hits"] as unknown[]).length, 1);
    } finally {
        work.dispose();
    }
});

test("a document that is not there is a refusal the model can read", async (t) => {
    if (!built()) {
        t.skip("cli/kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    try {
        const result = await call(work, "kb_get", { id: "D-9999" });
        assert.equal(result.isError, true);
        const answer = payload(result);
        assert.equal(answer["kind"], "refused");
        assert.equal(answer["error"], "not_found");
    } finally {
        work.dispose();
    }
});

test("with no store to find, kb_add is a refusal that tells the agent to run kb init", async (t) => {
    if (!built()) {
        t.skip("cli/kb-cli/bin/kb is not built");
        return;
    }
    /* A throwaway directory with no `kb init` in it. If some ancestor of the
     * temporary directory happens to hold a `.kb/`, the CLI would find it and
     * this would file into somebody else's store — so that case is detected
     * and skipped rather than written into. */
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kb-mcp-nostore-"));
    try {
        for (let up = path.dirname(dir); ; up = path.dirname(up)) {
            if (fs.existsSync(path.join(up, ".kb"))) {
                t.skip(`${up} holds a .kb store, so there is no store-less directory to test from`);
                return;
            }
            if (path.dirname(up) === up) {
                break;
            }
        }
        const server = new Server(new Kb({ bin: BIN, cwd: dir, env: ENV }));
        const result = await call({ server, dir, dispose: () => undefined }, "kb_add", {
            documents: [{ title: "IOCP", content: IOCP, collection: "win32-iocp" }],
        });
        assert.equal(result.isError, true, "kb_add filed with no store to file into");
        const answer = payload(result);
        assert.equal(answer["filed"], 0);
        const because = answer["because"] as Record<string, unknown>;
        assert.equal(because["kind"], "refused");
        assert.equal(because["error"], "not_found");
        assert.match(String(because["message"]), /kb init/);
        /* And nothing was created on the way to refusing. */
        assert.equal(fs.existsSync(path.join(dir, ".kb")), false);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

test("a query full of shell metacharacters reaches the real store as one argument", async (t) => {
    if (!built()) {
        t.skip("cli/kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    try {
        const hostile = 'D-1; touch /tmp/kb-mcp-should-not-exist && echo "$(id)"';
        const result = await call(work, "kb_get", { id: hostile });
        /* Refused for not being a `D-n`, with the WHOLE string in the reason —
         * which is only possible if the whole string arrived as one argument.
         * A shell would have eaten the semicolon and everything after it, and
         * the message would name a shorter id. */
        assert.equal(result.isError, true);
        const answer = payload(result);
        assert.equal(answer["kind"], "refused");
        assert.ok(
            String(answer["message"]).includes(hostile),
            `the store did not see the whole argument; it said: ${String(answer["message"])}`,
        );
        assert.equal(
            fs.existsSync("/tmp/kb-mcp-should-not-exist"),
            false,
            "a shell ran the second half of the argument",
        );
    } finally {
        work.dispose();
    }
});

test("kb_stale is checked against the real binary the moment kb stale exists", async (t) => {
    if (!built()) {
        t.skip("cli/kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    try {
        const result = await call(work, "kb_stale", { olderThan: "90d" });
        if (!hasCommand("stale")) {
            /* Until §5 lands: the refusal has to arrive as a sentence rather
             * than as a crash, because that is what a surface offering the
             * action owes whoever pressed it. */
            assert.equal(result.isError, true);
            const answer = payload(result);
            assert.equal(answer["kind"], "refused", `kb stale failed rather than refused: ${result.content[0].text}`);
            t.diagnostic(`kb stale is not implemented; it refuses with ${String(answer["error"])}`);
            return;
        }
        assert.notEqual(result.isError, true, result.content[0].text);
        assert.ok(Array.isArray(payload(result)["documents"]));
    } finally {
        work.dispose();
    }
});

test("kb_links is checked against the real binary the moment kb links exists", async (t) => {
    if (!built()) {
        t.skip("cli/kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    try {
        const added = payload(
            await call(work, "kb_add", {
                documents: [
                    { title: "IOCP", content: "CreateIoCompletionPort\n", collection: "win32" },
                    { title: "Ring", content: "io_uring_setup\n", collection: "io-uring" },
                ],
            }),
        );
        const [a, b] = (added["added"] as Record<string, unknown>[]).map((r) =>
            String(r["document"]),
        );
        const written = await call(work, "kb_links", {
            op: "add",
            from: a,
            to: b,
            type: "analogue_of",
        });
        if (!hasCommand("links")) {
            assert.equal(written.isError, true);
            assert.equal(payload(written)["kind"], "refused", written.content[0].text);
            t.diagnostic(
                `kb links is not implemented; it refuses with ${String(payload(written)["error"])}`,
            );
            return;
        }
        assert.notEqual(written.isError, true, written.content[0].text);
        const read = payload(await call(work, "kb_links", { op: "list", document: a }));
        assert.equal((read["outgoing"] as unknown[]).length, 1);

        /* The same edge through kb_get's include, from both ends, with
         * the far document resolved to a row. */
        type Edge = { to: string; from: string; type: string; document: { title: string } | null };
        type Links = { outgoing: Edge[]; incoming: Edge[] };
        const fromA = payload(await call(work, "kb_get", { id: a, include: ["links"] }))["links"] as Links;
        assert.deepEqual(
            fromA.outgoing.map((e) => [e.type, e.to, e.document?.title]),
            [["analogue_of", b, "Ring"]],
        );
        assert.deepEqual(fromA.incoming, []);
        const fromB = payload(await call(work, "kb_get", { id: b, include: ["links"] }))["links"] as Links;
        assert.deepEqual(fromB.incoming.map((e) => [e.from, e.document?.title]), [[a, "IOCP"]]);
        const plain = payload(await call(work, "kb_get", { id: a }));
        assert.equal("links" in plain, false, "links only when asked for");
    } finally {
        work.dispose();
    }
});

test("every tool this server offers is one the CLI could serve", async (t) => {
    /* The surface stated against `kb --help`, so that a tool built on a
     * command nobody has written is visible as such rather than discovered by
     * whoever calls it. */
    if (!built()) {
        t.skip("cli/kb-cli/bin/kb is not built");
        return;
    }
    const text = help();
    for (const command of ["search", "get", "chunk", "add", "collections"]) {
        assert.ok(hasCommand(command), `kb --help does not name ${command}`);
    }
    assert.ok(text.includes("--json"));
    /* And the two that are not there yet, recorded rather than asserted. */
    for (const command of ["stale", "links"]) {
        if (!hasCommand(command)) {
            t.diagnostic(`kb ${command} does not exist yet; kb_${command} cannot be exercised end to end`);
        }
    }
});

/* ------------------------------ what the tools answer, against what kb-js read
 *
 * THE SAME CHECK AS `kb-js`'s `cli.test.ts`, ONE LAYER UP, AND IT IS HERE
 * BECAUSE THIS LAYER MADE THE SAME MISTAKE. `kb_search` answered `{count,
 * hits}` off a reader that also carried `mode` and `olderThan`, so the
 * retrieval path that actually ran and the threshold every `stale` flag was
 * measured against reached the agent as nothing. It was invisible for exactly
 * the reason the `refresh` defect was invisible: everything present was
 * correct, and what was missing had no test asking for it.
 *
 * So a tool's payload is compared against the reader's own answer rather than
 * against a list somebody wrote. A field that turns up in `kb-js` and not in a
 * tool fails here; a field a tool invents fails too.
 */

test("a tool answers every field the reader it is built on answered", async (t) => {
    if (!built()) {
        t.skip("cli/kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    try {
        const kb = new Kb({ bin: BIN, cwd: work.dir, env: ENV });
        await call(work, "kb_add", {
            documents: [
                { title: "IOCP", content: IOCP, collection: "win32-iocp", mime: "text/markdown" },
            ],
        });

        /* Each row: the tool's payload, and the reader's answer for the same
         * question. `kb_add` and `kb_collections` are not here — the first
         * wraps its rows in a filing report and the second joins two commands,
         * and both are checked for their own shapes above. */
        const cases: { what: string; tool: Record<string, unknown>; read: object }[] = [
            {
                what: "kb_search",
                tool: payload(await call(work, "kb_search", { q: "CreateIoCompletionPort" })),
                read: await kb.search("CreateIoCompletionPort"),
            },
            {
                what: "kb_stale",
                tool: payload(await call(work, "kb_stale", { olderThan: "1d" })),
                read: await kb.stale({ olderThan: "1d" }),
            },
            {
                what: "kb_links",
                tool: payload(await call(work, "kb_links", { op: "list", document: "D-1" })),
                read: await kb.links("D-1"),
            },
        ];
        for (const c of cases) {
            assert.deepEqual(
                Object.keys(c.tool).sort(),
                Object.keys(c.read).sort(),
                `${c.what} and the reader it is built on do not agree about what the answer contains`,
            );
        }
    } finally {
        work.dispose();
    }
});

test("kb_search tells the agent which path ran and what stale was measured against", async (t) => {
    if (!built()) {
        t.skip("cli/kb-cli/bin/kb is not built");
        return;
    }
    const work = workspace();
    try {
        await call(work, "kb_add", {
            documents: [{ title: "IOCP", content: IOCP, collection: "win32-iocp" }],
        });
        const found = payload(await call(work, "kb_search", { q: "CreateIoCompletionPort" }));
        assert.equal(found["mode"], "keyword");
        assert.ok(String(found["olderThan"]).length > 0);
    } finally {
        work.dispose();
    }
});
