/* The protocol, not the handlers.
 *
 * Everything here is driven through a dispatch that records what it was asked
 * and answers a fixed value, so that what is being checked is the JSON-RPC
 * layer's own decisions: who gets a response, who gets none, what a batch is,
 * and which failures are transport errors rather than results.
 */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import {
    INTERNAL_ERROR,
    INVALID_REQUEST,
    METHOD_NOT_FOUND,
    PARSE_ERROR,
    RpcError,
    classify,
    handle,
} from "../jsonrpc";

interface Seen {
    method: string;
    params: unknown;
}

function recorder(answer: unknown = { ok: true }): {
    dispatch: (m: string, p: unknown) => Promise<unknown>;
    seen: Seen[];
} {
    const seen: Seen[] = [];
    return {
        seen,
        dispatch: async (method: string, params: unknown): Promise<unknown> => {
            seen.push({ method, params });
            if (method === "nope") {
                throw new RpcError(METHOD_NOT_FOUND, `There is no method called "${method}".`);
            }
            if (method === "boom") {
                throw new Error("the handler itself fell over");
            }
            return answer;
        },
    };
}

function line(value: unknown): string {
    return JSON.stringify(value);
}

/* ------------------------------------------------------------- the basics */

test("a request gets a result carrying its own id", async () => {
    const { dispatch } = recorder({ tools: [] });
    const response = (await handle(
        line({ jsonrpc: "2.0", id: 7, method: "tools/list" }),
        dispatch,
    )) as Record<string, unknown>;
    assert.deepEqual(response, { jsonrpc: "2.0", id: 7, result: { tools: [] } });
});

test("a string id comes back as the same string, not as a number", async () => {
    const { dispatch } = recorder();
    const response = (await handle(
        line({ jsonrpc: "2.0", id: "42", method: "ping" }),
        dispatch,
    )) as Record<string, unknown>;
    assert.strictEqual(response["id"], "42");
});

/* ----------------------------------------------------------- malformed in */

test("malformed JSON is a parse error against no id at all", async () => {
    const response = (await handle("{not json", recorder().dispatch)) as Record<string, unknown>;
    assert.equal(response["jsonrpc"], "2.0");
    assert.equal(response["id"], null);
    assert.equal((response["error"] as Record<string, unknown>)["code"], PARSE_ERROR);
});

test("valid JSON that is not a message is an invalid request", async () => {
    for (const text of ["7", '"hello"', "null", "true"]) {
        const response = (await handle(text, recorder().dispatch)) as Record<string, unknown>;
        assert.equal(
            (response["error"] as Record<string, unknown>)["code"],
            INVALID_REQUEST,
            `${text} was not refused`,
        );
    }
});

test("a message without jsonrpc 2.0 is refused, and the id still comes back", async () => {
    const response = (await handle(
        line({ id: 5, method: "ping" }),
        recorder().dispatch,
    )) as Record<string, unknown>;
    assert.equal(response["id"], 5);
    assert.equal((response["error"] as Record<string, unknown>)["code"], INVALID_REQUEST);
});

test("params that are neither an object nor an array are refused", async () => {
    const response = (await handle(
        line({ jsonrpc: "2.0", id: 1, method: "ping", params: "text" }),
        recorder().dispatch,
    )) as Record<string, unknown>;
    assert.equal((response["error"] as Record<string, unknown>)["code"], INVALID_REQUEST);
});

/* --------------------------------------------------------------- the id */

test("a message with no id is a notification and is answered with silence", async () => {
    /* THE RULE PEOPLE BREAK, because answering seems helpful. A client that
     * receives a response it has no id to match has an unhandled message, and
     * some of them close the session over it. */
    const { dispatch, seen } = recorder();
    assert.equal(await handle(line({ jsonrpc: "2.0", method: "ping" }), dispatch), null);
    assert.deepEqual(seen.map((s) => s.method), ["ping"]);
});

test("a notification for a method that does not exist is still answered with silence", async () => {
    /* Not even an error. `notifications/*` are how MCP clients stay forward
     * compatible; refusing one out loud would be a response to a message that
     * asked for none. */
    const { dispatch } = recorder();
    assert.equal(await handle(line({ jsonrpc: "2.0", method: "nope" }), dispatch), null);
});

test("an id of null is refused rather than treated as a notification", async () => {
    /* JSON-RPC allows null and MCP forbids it, and the reason is that null is
     * also what a server sends when it could not work out whose request
     * failed — so a client using it as an id cannot tell its own call from a
     * message about nobody's. */
    const { dispatch, seen } = recorder();
    const response = (await handle(
        line({ jsonrpc: "2.0", id: null, method: "ping" }),
        dispatch,
    )) as Record<string, unknown>;
    assert.equal(response["id"], null);
    assert.equal((response["error"] as Record<string, unknown>)["code"], INVALID_REQUEST);
    assert.deepEqual(seen, [], "a refused message reached the handler anyway");
});

test("an id that is an object is not an id", async () => {
    const response = (await handle(
        line({ jsonrpc: "2.0", id: { n: 1 }, method: "ping" }),
        recorder().dispatch,
    )) as Record<string, unknown>;
    assert.equal(response["id"], null);
    assert.equal((response["error"] as Record<string, unknown>)["code"], INVALID_REQUEST);
});

/* ------------------------------------------------------- unknown methods */

test("an unknown method is refused with an error and never answered with a result", async () => {
    /* A client discovers what a server has by asking. A server that answered
     * `{}` to everything would look like it had every method, and the client
     * would find out otherwise only by acting on an empty answer. */
    const { dispatch } = recorder();
    const response = (await handle(
        line({ jsonrpc: "2.0", id: 2, method: "nope" }),
        dispatch,
    )) as Record<string, unknown>;
    assert.equal("result" in response, false, "an unknown method came back as a result");
    assert.equal((response["error"] as Record<string, unknown>)["code"], METHOD_NOT_FOUND);
});

test("a handler that falls over is an internal error carrying its own sentence", async () => {
    const { dispatch } = recorder();
    const response = (await handle(
        line({ jsonrpc: "2.0", id: 3, method: "boom" }),
        dispatch,
    )) as Record<string, unknown>;
    const error = response["error"] as Record<string, unknown>;
    assert.equal(error["code"], INTERNAL_ERROR);
    assert.match(String(error["message"]), /fell over/);
});

/* -------------------------------------------------------------- batches */

test("a batch answers an array of the responses it owes, in order", async () => {
    const { dispatch, seen } = recorder({ ok: true });
    const response = (await handle(
        line([
            { jsonrpc: "2.0", id: 1, method: "ping" },
            { jsonrpc: "2.0", method: "notifications/initialized" },
            { jsonrpc: "2.0", id: 2, method: "tools/list" },
        ]),
        dispatch,
    )) as Record<string, unknown>[];
    assert.ok(Array.isArray(response));
    /* Two, not three: the notification in the middle is owed nothing. */
    assert.equal(response.length, 2);
    assert.deepEqual(response.map((r) => r["id"]), [1, 2]);
    assert.deepEqual(seen.map((s) => s.method), [
        "ping",
        "notifications/initialized",
        "tools/list",
    ]);
});

test("a batch of nothing but notifications produces no output at all", async () => {
    const { dispatch, seen } = recorder();
    const response = await handle(
        line([
            { jsonrpc: "2.0", method: "notifications/initialized" },
            { jsonrpc: "2.0", method: "notifications/cancelled" },
        ]),
        dispatch,
    );
    assert.equal(response, null, "an empty array was written, which a client cannot match");
    assert.equal(seen.length, 2);
});

test("an empty batch is one invalid request and not an empty array of them", async () => {
    const response = (await handle("[]", recorder().dispatch)) as Record<string, unknown>;
    assert.equal(Array.isArray(response), false);
    assert.equal((response["error"] as Record<string, unknown>)["code"], INVALID_REQUEST);
});

test("one bad element of a batch does not take the good ones with it", async () => {
    const { dispatch } = recorder();
    const response = (await handle(
        line([7, { jsonrpc: "2.0", id: 1, method: "ping" }]),
        dispatch,
    )) as Record<string, unknown>[];
    assert.equal(response.length, 2);
    assert.equal((response[0]["error"] as Record<string, unknown>)["code"], INVALID_REQUEST);
    assert.deepEqual(response[1]["result"], { ok: true });
});

test("a batch runs its elements one after another and not all at once", async () => {
    /* The elements may be two writes into one store, and a store is a set of
     * append-only logs with a lock on it. Running them at once would turn a
     * batch into a contention test the caller did not ask for. */
    let running = 0;
    let overlapped = false;
    const dispatch = async (): Promise<unknown> => {
        running++;
        overlapped = overlapped || running > 1;
        await new Promise((r) => setTimeout(r, 5));
        running--;
        return {};
    };
    await handle(
        line([
            { jsonrpc: "2.0", id: 1, method: "a" },
            { jsonrpc: "2.0", id: 2, method: "b" },
            { jsonrpc: "2.0", id: 3, method: "c" },
        ]),
        dispatch,
    );
    assert.equal(overlapped, false);
});

/* ------------------------------------------------------ a response inbound */

test("a response to a request this server never made is ignored, not argued with", async () => {
    const { dispatch, seen } = recorder();
    assert.equal(await handle(line({ jsonrpc: "2.0", id: 1, result: {} }), dispatch), null);
    assert.equal(await handle(line({ jsonrpc: "2.0", id: 2, error: {} }), dispatch), null);
    assert.deepEqual(seen, []);
});

/* ---------------------------------------------------------- classify itself */

test("classify names each kind of message", () => {
    assert.equal(classify({ jsonrpc: "2.0", id: 1, method: "m" }).kind, "request");
    assert.equal(classify({ jsonrpc: "2.0", method: "m" }).kind, "notification");
    assert.equal(classify({ jsonrpc: "2.0", id: 1, result: {} }).kind, "ignore");
    assert.equal(classify([]).kind, "invalid");
    assert.equal(classify({ jsonrpc: "1.0", id: 1, method: "m" }).kind, "invalid");
});
