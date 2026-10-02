import * as assert from "node:assert/strict";
import * as http from "node:http";
import { test } from "node:test";

import { Forms } from "../forms";
import { MCP_PATH, Served, portFor, serve } from "../http";

type Json = Record<string, unknown>;

function post(s: Served, body: unknown, headers: Record<string, string> = {}): Promise<{ status: number; headers: http.IncomingHttpHeaders; text: string }> {
    return new Promise((resolve, reject) => {
        const req = http.request(
            { host: "127.0.0.1", port: s.port, path: MCP_PATH, method: "POST", headers: { "content-type": "application/json", ...headers } },
            (res) => {
                let text = "";
                res.on("data", (d) => (text += d));
                res.on("end", () => resolve({ status: res.statusCode!, headers: res.headers, text }));
            },
        );
        req.on("error", reject);
        req.end(typeof body === "string" ? body : JSON.stringify(body));
    });
}

/** The data lines of an event stream, parsed. */
function events(text: string): Json[] {
    return text
        .split("\n\n")
        .filter((e) => e.startsWith("data: "))
        .map((e) => JSON.parse(e.slice(6)) as Json);
}

async function window(progressMs = 5) {
    const forms = new Forms();
    const s = await serve({ ctx: { forms, waitMs: 2000, progressMs }, port: 0 });
    return { forms, s };
}

test("initialize and tools/list are answered with JSON, and initialize hands out a session", async (t) => {
    const { s } = await window();
    t.after(() => s.close());
    const init = await post(s, { jsonrpc: "2.0", id: 1, method: "initialize", params: { protocolVersion: "2025-11-25" } });
    assert.equal(init.status, 200);
    assert.match(String(init.headers["content-type"]), /application\/json/);
    assert.ok(init.headers["mcp-session-id"]);
    assert.equal((JSON.parse(init.text).result as Json)["protocolVersion"], "2025-11-25");
    const note = await post(s, { jsonrpc: "2.0", method: "notifications/initialized" });
    assert.equal(note.status, 202);
    const list = JSON.parse((await post(s, { jsonrpc: "2.0", id: 2, method: "tools/list" })).text);
    assert.equal(list.result.tools.length, 2);
    assert.equal(s.url, `http://127.0.0.1:${s.port}${MCP_PATH}`);
});

test("a tool call streams its progress, then its result", async (t) => {
    const { forms, s } = await window();
    t.after(() => s.close());
    forms.on("open", (req) => setTimeout(() => forms.submit(req.id, { status: "submitted", steps: { "1": { state: "answered", text: "hi" } } }), 40));
    const res = await post(s, {
        jsonrpc: "2.0",
        id: 7,
        method: "tools/call",
        params: { name: "ask", arguments: { title: "t", steps: [{ title: "A?" }] }, _meta: { progressToken: 3 } },
    });
    assert.match(String(res.headers["content-type"]), /text\/event-stream/);
    const evs = events(res.text);
    const last = evs.at(-1)!;
    assert.ok(evs.slice(0, -1).length >= 2 && evs.slice(0, -1).every((e) => e["method"] === "notifications/progress"));
    assert.equal(last["id"], 7);
    const out = JSON.parse(((last["result"] as Json)["content"] as { text: string }[])[0].text);
    assert.equal(out.status, "submitted");
    assert.equal(out.steps[0].text, "hi");
});

test("a client that hangs up, or cancels, cancels its form", async (t) => {
    const { forms, s } = await window(1000);
    t.after(() => s.close());
    const closed: string[] = [];
    forms.on("answer", (id, a) => closed.push(`${id} ${a.status}`));
    const body = JSON.stringify({ jsonrpc: "2.0", id: 1, method: "tools/call", params: { name: "ask", arguments: { title: "t", steps: [{ title: "A?" }] } } });
    const req = http.request({ host: "127.0.0.1", port: s.port, path: MCP_PATH, method: "POST", headers: { "content-type": "application/json" } });
    req.on("error", () => {});
    req.end(body);
    await new Promise((r) => forms.on("open", r));
    req.destroy();
    await new Promise((r) => setTimeout(r, 50));
    assert.deepEqual(closed, ["F-1 cancelled"]);

    const second = post(s, { jsonrpc: "2.0", id: 2, method: "tools/call", params: { name: "ask", arguments: { title: "t", steps: [{ title: "B?" }] } } }, { "mcp-session-id": "x" });
    await new Promise((r) => forms.on("open", r));
    await post(s, { jsonrpc: "2.0", method: "notifications/cancelled", params: { requestId: 2 } }, { "mcp-session-id": "x" });
    const res = await second;
    assert.deepEqual(events(res.text), [], "a cancelled call gets no result");
    assert.deepEqual(closed, ["F-1 cancelled", "F-2 cancelled"]);
});

test("only this machine's agents: other hosts, browser origins and other bodies are refused", async (t) => {
    const { s } = await window();
    t.after(() => s.close());
    const ping = { jsonrpc: "2.0", id: 1, method: "ping" };
    assert.equal((await post(s, ping)).status, 200);
    assert.equal((await post(s, ping, { host: "evil.example:80" })).status, 403);
    assert.equal((await post(s, ping, { origin: "http://evil.example" })).status, 403);
    assert.equal((await post(s, ping, { "content-type": "text/plain" })).status, 415);
    assert.equal((await post(s, "[1,2]")).status, 400);
    assert.equal((await post(s, "{nope")).status, 400);
    const get = await new Promise<number>((resolve) => http.get({ host: "127.0.0.1", port: s.port, path: MCP_PATH }, (r) => (r.resume(), resolve(r.statusCode!))));
    assert.equal(get, 405);
});

test("a taken port moves to the next, and a folder always asks for the same one", async (t) => {
    const forms = new Forms();
    const a = await serve({ ctx: { forms }, port: 0 });
    t.after(() => a.close());
    const b = await serve({ ctx: { forms }, port: a.port });
    t.after(() => b.close());
    assert.equal(b.port, a.port + 1);
    assert.equal(portFor("/Users/x/project"), portFor("/Users/x/project"));
    assert.notEqual(portFor("/Users/x/project"), portFor("/Users/x/other"));
    assert.ok(portFor("/a") >= 40000 && portFor("/a") < 49000);
});
