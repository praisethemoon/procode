import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { Ctx, INSTRUCTIONS, handle } from "../mcp";

type Json = Record<string, unknown>;

function repo(): string {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "ask-mcp-"));
    fs.mkdirSync(path.join(root, ".git"));
    return root;
}

async function call(ctx: Ctx, name: string, args: Json, meta?: Json): Promise<{ isError: boolean; text: string }> {
    const params: Json = { name, arguments: args, ...(meta ? { _meta: meta } : {}) };
    const out = (await handle({ jsonrpc: "2.0", id: 1, method: "tools/call", params }, ctx)) as Json;
    const result = out["result"] as Json;
    return { isError: result["isError"] === true, text: (result["content"] as { text: string }[])[0].text };
}

/** Plays the editor tab: waits for the request, then writes the answer. */
function answerWhenAsked(root: string, id: string, answer: Json): Promise<Json> {
    const dir = path.join(root, ".ask", id);
    return new Promise((resolve) => {
        const t = setInterval(() => {
            if (!fs.existsSync(path.join(dir, "request.json"))) return;
            clearInterval(t);
            const req = JSON.parse(fs.readFileSync(path.join(dir, "request.json"), "utf8")) as Json;
            fs.writeFileSync(path.join(dir, "answer.json"), JSON.stringify(answer));
            resolve(req);
        }, 5);
    });
}

const fast = (cwd: string, extra: Partial<Ctx> = {}): Ctx => ({ cwd, pollMs: 5, waitMs: 2000, ...extra });

test("the server introduces itself and its two tools", async () => {
    const out = (await handle({ jsonrpc: "2.0", id: 1, method: "initialize", params: {} }, { cwd: "/" })) as Json;
    assert.equal(((out["result"] as Json)["serverInfo"] as Json)["name"], "ask");
    assert.match(INSTRUCTIONS, /two or more questions/);
    const tools = (await handle({ jsonrpc: "2.0", id: 2, method: "tools/list" }, { cwd: "/" })) as Json;
    assert.deepEqual(
        ((tools["result"] as Json)["tools"] as { name: string }[]).map((t) => t.name),
        ["ask", "ask_wait"],
    );
});

test("ask writes the request at the repository root and returns what the tab answered", async () => {
    const root = repo();
    const sub = path.join(root, "src");
    fs.mkdirSync(sub);
    const tab = answerWhenAsked(root, "F-1", {
        status: "submitted",
        answeredAt: "2026-10-02T00:00:00Z",
        steps: { layout: { state: "answered", choices: ["Tab"] }, why: { state: "needs_more", question: "for what?" } },
    });
    const res = await call(fast(sub), "ask", {
        title: "The ask tab",
        steps: [{ id: "layout", title: "Where?", options: ["Sidebar", "Tab"] }, { id: "why", title: "Why?" }, { title: "Later" }],
    });
    assert.equal(res.isError, false);
    const req = await tab;
    assert.equal(req["title"], "The ask tab");
    assert.equal(fs.readFileSync(path.join(root, ".ask", ".gitignore"), "utf8"), "*\n");
    const out = JSON.parse(res.text);
    assert.equal(out.id, "F-1");
    assert.equal(out.status, "submitted");
    assert.deepEqual(
        out.steps.map((s: Json) => [s["id"], s["state"]]),
        [
            ["layout", "answered"],
            ["why", "needs_more"],
            ["3", "skipped"],
        ],
    );
    assert.match(out.next, /step why/);
    assert.match(out.next, /from: "F-1"/);
});

test("asking again with from fills in the answers given, not the ones sent back", async () => {
    const root = repo();
    void answerWhenAsked(root, "F-1", {
        status: "submitted",
        steps: { a: { state: "answered", text: "yes" }, b: { state: "needs_more", question: "?" } },
    });
    await call(fast(root), "ask", { title: "t", steps: [{ id: "a", title: "A" }, { id: "b", title: "B" }] });
    const second = answerWhenAsked(root, "F-2", { status: "submitted", steps: {} });
    await call(fast(root), "ask", { title: "t", steps: [{ id: "a", title: "A" }, { id: "b", title: "B", body: "more" }], from: "F-1" });
    const req = await second;
    assert.deepEqual(req["previous"], { a: { state: "answered", text: "yes" } });
    assert.match((await call(fast(root), "ask", { title: "t", steps: [{ title: "A" }], from: "F-9" })).text, /^not_found: /);
    assert.match((await call(fast(root), "ask", { title: "t", steps: [{ title: "A" }], from: "../x" })).text, /^bad_id: /);
});

test("a form closed in the editor comes back cancelled", async () => {
    const root = repo();
    void answerWhenAsked(root, "F-1", { status: "cancelled", steps: {} });
    const out = JSON.parse((await call(fast(root), "ask", { title: "t", steps: [{ title: "A" }] })).text);
    assert.equal(out.status, "cancelled");
});

test("a form not answered in time comes back waiting, and ask_wait picks it up", async () => {
    const root = repo();
    const first = JSON.parse((await call(fast(root, { waitMs: 30 }), "ask", { title: "t", steps: [{ title: "A" }] })).text);
    assert.equal(first.status, "waiting");
    assert.match(first.next, /ask_wait with id "F-1"/);
    void answerWhenAsked(root, "F-1", { status: "submitted", steps: { "1": { state: "answered", text: "ok" } } });
    const later = JSON.parse((await call(fast(root), "ask_wait", { id: "F-1" })).text);
    assert.equal(later.status, "submitted");
    assert.equal(later.steps[0].text, "ok");
    assert.match((await call(fast(root), "ask_wait", { id: "F-7" })).text, /^not_found: /);
});

test("a call the client cancels cancels its form, so the tab closes", async () => {
    const root = repo();
    const ac = new AbortController();
    const pending = call(fast(root, { signal: ac.signal, waitMs: 5000 }), "ask", { title: "t", steps: [{ title: "A" }] });
    setTimeout(() => ac.abort(), 30);
    await pending;
    const ans = JSON.parse(fs.readFileSync(path.join(root, ".ask", "F-1", "answer.json"), "utf8"));
    assert.equal(ans.status, "cancelled");
});

test("progress is sent while waiting when the client gave a token", async (t) => {
    t.mock.timers.enable({ apis: ["Date"] });
    const root = repo();
    const sent: Json[] = [];
    const pending = call(fast(root, { notify: (m) => sent.push(m), waitMs: 60_000 }), "ask", { title: "t", steps: [{ title: "A" }] }, { progressToken: "p1" });
    for (let i = 0; i < 4; i++) {
        await new Promise((r) => setImmediate(r));
        t.mock.timers.tick(16_000);
        await new Promise((r) => setTimeout(r, 10));
    }
    fs.writeFileSync(path.join(root, ".ask", "F-1", "answer.json"), JSON.stringify({ status: "submitted", steps: {} }));
    await pending;
    assert.ok(sent.length >= 2, `${sent.length} progress notifications`);
    const p = sent[0]["params"] as Json;
    assert.equal(p["progressToken"], "p1");
    assert.ok((sent[1]["params"] as Json)["progress"] as number > (p["progress"] as number));
});

test("refusals are results with the code first", async () => {
    const root = repo();
    assert.match((await call(fast(root), "ask", { title: "", steps: [{ title: "A" }] })).text, /^invalid: title/);
    assert.match((await call(fast(root), "ask", { title: "t", steps: [] })).text, /^invalid: steps/);
    assert.match((await call(fast(root), "ask", { title: "t", steps: [{ title: "A" }], wait: 1 })).text, /^invalid: ask takes no "wait"/);
    assert.equal(fs.existsSync(path.join(root, ".ask")), false, "a refused form writes nothing");
});
