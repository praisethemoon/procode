/* The MCP methods, through the JSON-RPC layer that carries them.
 *
 * Driven as whole messages rather than by calling `dispatch` directly, because
 * the thing a client sees is a response envelope and the two places a method
 * can go wrong — what it answers, and whether it answers at all — are on
 * different sides of that envelope.
 */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import { Kb } from "kb-js";

import { INVALID_PARAMS, METHOD_NOT_FOUND, handle } from "../jsonrpc";
import {
    INSTRUCTIONS,
    PROTOCOL_VERSION,
    SERVER_NAME,
    SUPPORTED_PROTOCOL_VERSIONS,
    Server,
} from "../server";
import { TOOL_NAMES } from "../tools";
import { FakeKb, ok } from "./fake";

function server(bin = "/nonexistent/kb"): Server {
    return new Server(new Kb({ bin }));
}

async function ask(s: Server, message: unknown): Promise<Record<string, unknown>> {
    const response = await handle(JSON.stringify(message), s.dispatch);
    assert.notEqual(response, null, "a request was answered with silence");
    return response as Record<string, unknown>;
}

function result(response: Record<string, unknown>): Record<string, unknown> {
    assert.equal("error" in response, false, `expected a result, got ${JSON.stringify(response)}`);
    return response["result"] as Record<string, unknown>;
}

function error(response: Record<string, unknown>): Record<string, unknown> {
    assert.equal("result" in response, false, `expected an error, got ${JSON.stringify(response)}`);
    return response["error"] as Record<string, unknown>;
}

/* ----------------------------------------------------------- the handshake */

test("initialize announces tools, a name and the instructions", async () => {
    const answer = result(
        await ask(server(), { jsonrpc: "2.0", id: 1, method: "initialize", params: {} }),
    );
    assert.deepEqual(answer["capabilities"], { tools: {} });
    assert.deepEqual(answer["serverInfo"], { name: SERVER_NAME, version: "0.1.0" });
    assert.equal(answer["instructions"], INSTRUCTIONS);
    assert.equal(answer["protocolVersion"], PROTOCOL_VERSION);
});

test("a version this server speaks comes back unchanged", async () => {
    for (const version of SUPPORTED_PROTOCOL_VERSIONS) {
        const answer = result(
            await ask(server(), {
                jsonrpc: "2.0",
                id: 1,
                method: "initialize",
                params: { protocolVersion: version },
            }),
        );
        assert.equal(answer["protocolVersion"], version);
    }
});

test("a version this server does not speak is answered with the one it does", async () => {
    /* The handshake exists so that a client knows what to expect. A server
     * that echoed whatever it was told would claim every revision, including
     * the ones whose additions it does not have. */
    for (const version of ["2025-06-18", "1999-01-01", "", "not a version"]) {
        const answer = result(
            await ask(server(), {
                jsonrpc: "2.0",
                id: 1,
                method: "initialize",
                params: { protocolVersion: version },
            }),
        );
        assert.equal(answer["protocolVersion"], PROTOCOL_VERSION);
    }
});

test("initialize with no params at all is still answered", async () => {
    /* Leniently, because a handshake that refuses is a server nobody can
     * connect to, and there is nothing in these params this server needs. */
    const answer = result(await ask(server(), { jsonrpc: "2.0", id: 1, method: "initialize" }));
    assert.equal(answer["protocolVersion"], PROTOCOL_VERSION);
});

test("the initialized notification is accepted and answered with silence", async () => {
    const response = await handle(
        JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }),
        server().dispatch,
    );
    assert.equal(response, null);
});

test("ping answers an empty result", async () => {
    assert.deepEqual(result(await ask(server(), { jsonrpc: "2.0", id: 2, method: "ping" })), {});
});

/* -------------------------------------------------------------- the tools */

test("tools/list is §9's six and only those", async () => {
    const answer = result(await ask(server(), { jsonrpc: "2.0", id: 3, method: "tools/list" }));
    const tools = answer["tools"] as { name: string; inputSchema: unknown }[];
    assert.equal(tools.length, 6);
    assert.deepEqual(tools.map((t) => t.name), [...TOOL_NAMES]);
    for (const tool of tools) {
        assert.equal(typeof tool.inputSchema, "object");
    }
});

test("a listed tool survives being encoded and read back", async () => {
    /* The schemas cross a JSON boundary before anything validates against
     * them; a value that does not survive the round trip is a schema the
     * client never sees. */
    const answer = result(await ask(server(), { jsonrpc: "2.0", id: 3, method: "tools/list" }));
    assert.deepEqual(JSON.parse(JSON.stringify(answer)), answer);
});

test("a tools/call without a name is a transport error", async () => {
    const answer = error(
        await ask(server(), { jsonrpc: "2.0", id: 4, method: "tools/call", params: {} }),
    );
    assert.equal(answer["code"], INVALID_PARAMS);
});

test("a tools/call for a tool §9 withholds is an error and not a result", async () => {
    /* `kb_rebuild` does not exist and must not look like a tool that happened
     * to fail this time. */
    for (const name of ["kb_rebuild", "kb_promote", "kb_delete_collection"]) {
        const answer = error(
            await ask(server(), {
                jsonrpc: "2.0",
                id: 5,
                method: "tools/call",
                params: { name, arguments: {} },
            }),
        );
        assert.equal(answer["code"], INVALID_PARAMS);
        assert.match(String(answer["message"]), new RegExp(name));
    }
});

test("a tools/call that reaches the store comes back as a result", async () => {
    const fake = FakeKb.create([
        { stdout: ok({ collections: [], count: 0 }) },
        { stdout: ok({ collections: [], count: 0, totals: { documents: 0, chunks: 0, bytes: 0 } }) },
    ]);
    try {
        const s = new Server(new Kb({ bin: fake.bin, env: fake.env() }));
        const answer = result(
            await ask(s, {
                jsonrpc: "2.0",
                id: 6,
                method: "tools/call",
                params: { name: "kb_collections", arguments: {} },
            }),
        );
        const content = answer["content"] as { type: string; text: string }[];
        assert.equal(content[0].type, "text");
        assert.deepEqual(JSON.parse(content[0].text), {
            count: 0,
            collections: [],
            totals: { documents: 0, chunks: 0, bytes: 0 },
        });
        assert.equal("isError" in answer, false);
    } finally {
        fake.dispose();
    }
});

test("a _meta the client attached to the params is left alone", async () => {
    /* The strictness that matters is over a tool's own arguments, where an
     * unread key is a filter that silently does nothing. MCP reserves the
     * right to add fields at this level. */
    const fake = FakeKb.create([{ stdout: ok({ collections: [], count: 0, totals: {} }) }]);
    try {
        const s = new Server(new Kb({ bin: fake.bin, env: fake.env() }));
        const answer = await ask(s, {
            jsonrpc: "2.0",
            id: 7,
            method: "tools/call",
            params: { name: "kb_collections", arguments: {}, _meta: { progressToken: 1 } },
        });
        assert.equal("error" in answer, false);
    } finally {
        fake.dispose();
    }
});

/* ------------------------------------------------------------ everything else */

test("a method this server does not have is refused rather than answered", async () => {
    /* THE MUTATION: a `default:` that returns `{}`. The server would then look
     * like it had every method, and a client would find out otherwise only by
     * acting on an empty answer. */
    for (const method of [
        "resources/list",
        "prompts/list",
        "resources/read",
        "completion/complete",
        "tools/calls",
        "",
    ]) {
        const answer = error(await ask(server(), { jsonrpc: "2.0", id: 8, method }));
        assert.equal(answer["code"], METHOD_NOT_FOUND, `${method} was answered`);
    }
});

test("nothing is announced that is not also answered", async () => {
    /* A capability a client reads and a method a server has are two statements
     * of one fact. `resources` and `prompts` are absent from both. */
    const answer = result(
        await ask(server(), { jsonrpc: "2.0", id: 9, method: "initialize", params: {} }),
    );
    const capabilities = answer["capabilities"] as Record<string, unknown>;
    assert.deepEqual(Object.keys(capabilities), ["tools"]);
});
