/* The wiring, over real streams.
 *
 * WHAT THIS ADDS OVER `framing.test.ts` AND `jsonrpc.test.ts`. Those test a
 * buffer and a string; this tests the thing that is actually connected to a
 * pipe — that a chunk event reaches the reader, that a response is written as
 * its own line, that a notification writes nothing at all, and that two
 * messages in flight do not overtake each other. Each of those is a wire that
 * can be connected to the wrong terminal while both ends are perfectly good.
 */

import * as assert from "node:assert/strict";
import { PassThrough, Writable } from "node:stream";
import { test } from "node:test";

import { Dispatch, RpcError, METHOD_NOT_FOUND } from "../jsonrpc";
import { serve } from "../transport";

function sink(): { stream: Writable; text(): string; lines(): unknown[] } {
    const chunks: Buffer[] = [];
    const stream = new Writable({
        write(chunk: Buffer, _encoding, done): void {
            chunks.push(Buffer.from(chunk));
            done();
        },
    });
    const text = (): string => Buffer.concat(chunks).toString("utf8");
    return {
        stream,
        text,
        lines: (): unknown[] =>
            text()
                .split("\n")
                .filter((l) => l.length > 0)
                .map((l) => JSON.parse(l) as unknown),
    };
}

const echo: Dispatch = async (method: string, params: unknown): Promise<unknown> => {
    if (method === "nope") {
        throw new RpcError(METHOD_NOT_FOUND, "no");
    }
    return { method, params };
};

test("a request written in two pieces is answered once", async () => {
    /* The obligation, through the streams rather than through the buffer: the
     * split is between two `write` calls, which is what a client on the other
     * side of a pipe actually produces. */
    const input = new PassThrough();
    const out = sink();
    const done = serve(input, out.stream, echo);
    const message = `${JSON.stringify({ jsonrpc: "2.0", id: 1, method: "tools/list" })}\n`;
    input.write(message.slice(0, 20));
    input.write(message.slice(20));
    input.end();
    await done;
    assert.deepEqual(out.lines(), [
        { jsonrpc: "2.0", id: 1, result: { method: "tools/list" } },
    ]);
});

test("every response is its own line and the stream ends on one", async () => {
    const input = new PassThrough();
    const out = sink();
    const done = serve(input, out.stream, echo);
    for (const id of [1, 2, 3]) {
        input.write(`${JSON.stringify({ jsonrpc: "2.0", id, method: "ping" })}\n`);
    }
    input.end();
    await done;
    const text = out.text();
    assert.equal(text.endsWith("\n"), true);
    assert.equal(text.split("\n").filter((l) => l.length > 0).length, 3);
});

test("responses come back in the order the requests arrived", async () => {
    /* Not a JSON-RPC requirement — a client matches by id — but the store
     * underneath is a set of append-only logs with a lock on it, and two calls
     * running at once would be two processes contending for it. §11 has
     * `store_locked` for what the loser gets, and it would be a refusal caused
     * by nothing but this server's own concurrency. */
    const started: number[] = [];
    const slow: Dispatch = async (_method, params): Promise<unknown> => {
        const n = (params as { n: number }).n;
        started.push(n);
        await new Promise((r) => setTimeout(r, n === 1 ? 30 : 1));
        return { n };
    };
    const input = new PassThrough();
    const out = sink();
    const done = serve(input, out.stream, slow);
    input.write(`${JSON.stringify({ jsonrpc: "2.0", id: 1, method: "m", params: { n: 1 } })}\n`);
    input.write(`${JSON.stringify({ jsonrpc: "2.0", id: 2, method: "m", params: { n: 2 } })}\n`);
    input.end();
    await done;
    assert.deepEqual(started, [1, 2]);
    assert.deepEqual(
        out.lines().map((l) => (l as { id: number }).id),
        [1, 2],
    );
});

test("a notification writes nothing at all", async () => {
    const input = new PassThrough();
    const out = sink();
    const done = serve(input, out.stream, echo);
    input.write(`${JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" })}\n`);
    input.end();
    await done;
    assert.equal(out.text(), "");
});

test("malformed JSON on the wire is answered and does not stop the session", async () => {
    const input = new PassThrough();
    const out = sink();
    const done = serve(input, out.stream, echo);
    input.write("{ not json\n");
    input.write(`${JSON.stringify({ jsonrpc: "2.0", id: 9, method: "ping" })}\n`);
    input.end();
    await done;
    const lines = out.lines() as Record<string, unknown>[];
    assert.equal(lines.length, 2);
    assert.equal((lines[0]["error"] as Record<string, unknown>)["code"], -32700);
    assert.equal(lines[1]["id"], 9);
});

test("a batch on the wire comes back as one line carrying an array", async () => {
    const input = new PassThrough();
    const out = sink();
    const done = serve(input, out.stream, echo);
    input.write(
        `${JSON.stringify([
            { jsonrpc: "2.0", id: 1, method: "ping" },
            { jsonrpc: "2.0", method: "notifications/initialized" },
            { jsonrpc: "2.0", id: 2, method: "nope" },
        ])}\n`,
    );
    input.end();
    await done;
    const lines = out.lines();
    assert.equal(lines.length, 1, "a batch was answered with several lines");
    const batch = lines[0] as Record<string, unknown>[];
    assert.equal(batch.length, 2);
    assert.deepEqual(batch.map((r) => r["id"]), [1, 2]);
});

test("a peer that never frames anything is reported and the server stops reading", async () => {
    /* Once a message longer than the cap has arrived with no newline in it
     * there is no boundary left to resynchronise on, so carrying on would mean
     * parsing the tail of somebody's document as if it were a request. */
    const input = new PassThrough();
    const out = sink();
    const errors: Error[] = [];
    const done = serve(input, out.stream, echo, {
        maxLineBytes: 64,
        onError: (e) => errors.push(e),
    });
    input.write("x".repeat(200));
    await done;
    assert.equal(errors.length, 1);
    assert.match(errors[0].message, /without a newline/);
    assert.equal(out.text(), "");
});

test("the server finishes when its input ends, even having answered nothing", async () => {
    const input = new PassThrough();
    const out = sink();
    const done = serve(input, out.stream, echo);
    input.end();
    await done;
    assert.equal(out.text(), "");
});

test("a last message with no trailing newline is still answered", async () => {
    const input = new PassThrough();
    const out = sink();
    const done = serve(input, out.stream, echo);
    input.end(JSON.stringify({ jsonrpc: "2.0", id: 4, method: "ping" }));
    await done;
    assert.equal((out.lines()[0] as Record<string, unknown>)["id"], 4);
});
