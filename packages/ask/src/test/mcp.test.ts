import * as assert from "node:assert/strict";
import { test } from "node:test";

import { Forms } from "../forms";
import { Ctx, INSTRUCTIONS, handle } from "../mcp";

type Json = Record<string, unknown>;

async function call(ctx: Ctx, name: string, args: Json): Promise<{ isError: boolean; text: string }> {
    const out = (await handle({ jsonrpc: "2.0", id: 1, method: "tools/call", params: { name, arguments: args } }, ctx)) as Json;
    const result = out["result"] as Json;
    return { isError: result["isError"] === true, text: (result["content"] as { text: string }[])[0].text };
}

/** A window's forms, with a stand-in tab that answers each form as it opens. */
function window(answer?: (id: string) => Json): Ctx {
    const forms = new Forms();
    if (answer) forms.on("open", (req) => setImmediate(() => forms.submit(req.id, answer(req.id))));
    return { forms, waitMs: 2000, progressMs: 5 };
}

test("the server introduces itself and its two tools", async () => {
    const ctx = window();
    const out = (await handle({ jsonrpc: "2.0", id: 1, method: "initialize", params: {} }, ctx)) as Json;
    assert.equal(((out["result"] as Json)["serverInfo"] as Json)["name"], "ask");
    assert.match(INSTRUCTIONS, /two or more questions/);
    const tools = (await handle({ jsonrpc: "2.0", id: 2, method: "tools/list" }, ctx)) as Json;
    assert.deepEqual(
        ((tools["result"] as Json)["tools"] as { name: string }[]).map((t) => t.name),
        ["ask", "ask_wait"],
    );
});

test("ask opens a form and returns what the tab answered", async () => {
    const opened: string[] = [];
    const ctx = window(() => ({
        status: "submitted",
        steps: { layout: { state: "answered", choices: ["Tab"] }, why: { state: "needs_more", question: "for what?" } },
    }));
    ctx.forms.on("open", (req) => opened.push(req.title));
    const res = await call(ctx, "ask", {
        title: "The ask tab",
        steps: [{ id: "layout", title: "Where?", options: ["Sidebar", "Tab"] }, { id: "why", title: "Why?" }, { title: "Later" }],
    });
    assert.equal(res.isError, false);
    assert.deepEqual(opened, ["The ask tab"]);
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
    const ctx = window((id) => (id === "F-1" ? { status: "submitted", steps: { a: { state: "answered", text: "yes" }, b: { state: "needs_more", question: "?" } } } : { status: "submitted", steps: {} }));
    await call(ctx, "ask", { title: "t", steps: [{ id: "a", title: "A" }, { id: "b", title: "B" }] });
    await call(ctx, "ask", { title: "t", steps: [{ id: "a", title: "A" }, { id: "b", title: "B", body: "more" }], from: "F-1" });
    assert.deepEqual(ctx.forms.request("F-2").previous, { a: { state: "answered", text: "yes" } });
    assert.match((await call(ctx, "ask", { title: "t", steps: [{ title: "A" }], from: "F-9" })).text, /^not_found: /);
    assert.match((await call(ctx, "ask", { title: "t", steps: [{ title: "A" }], from: "../x" })).text, /^bad_id: /);
});

test("a form closed in the editor comes back cancelled", async () => {
    const ctx = window();
    ctx.forms.on("open", (req) => setImmediate(() => ctx.forms.cancel(req.id)));
    const out = JSON.parse((await call(ctx, "ask", { title: "t", steps: [{ title: "A" }] })).text);
    assert.equal(out.status, "cancelled");
});

test("a form not answered in time comes back waiting, and ask_wait picks it up", async () => {
    const ctx = { ...window(), waitMs: 30 };
    const first = JSON.parse((await call(ctx, "ask", { title: "t", steps: [{ title: "A" }] })).text);
    assert.equal(first.status, "waiting");
    assert.match(first.next, /ask_wait with id "F-1"/);
    setTimeout(() => ctx.forms.submit("F-1", { status: "submitted", steps: { "1": { state: "answered", text: "ok" } } }), 10);
    const later = JSON.parse((await call({ ...ctx, waitMs: 2000 }, "ask_wait", { id: "F-1" })).text);
    assert.equal(later.status, "submitted");
    assert.equal(later.steps[0].text, "ok");
    assert.match((await call(ctx, "ask_wait", { id: "F-7" })).text, /^not_found: /);
});

test("a call its client cancels cancels its form, so the tab closes", async () => {
    const ac = new AbortController();
    const ctx = { ...window(), signal: ac.signal };
    const closed: string[] = [];
    ctx.forms.on("answer", (id, a) => closed.push(`${id} ${a.status}`));
    const pending = call(ctx, "ask", { title: "t", steps: [{ title: "A" }] });
    setTimeout(() => ac.abort(), 20);
    await pending;
    assert.deepEqual(closed, ["F-1 cancelled"]);
});

test("progress goes to the client while waiting, when it gave a token", async () => {
    const sent: Json[] = [];
    const ctx = { ...window(), progressToken: "p1", notify: (m: Json) => sent.push(m) };
    setTimeout(() => ctx.forms.submit("F-1", { status: "submitted", steps: {} }), 40);
    await call(ctx, "ask", { title: "t", steps: [{ title: "A" }] });
    assert.ok(sent.length >= 2, `${sent.length} progress notifications`);
    assert.equal(sent[0]["method"], "notifications/progress");
    assert.equal((sent[0]["params"] as Json)["progressToken"], "p1");
    const without: Json[] = [];
    const quiet = { ...window(), notify: (m: Json) => without.push(m) };
    setTimeout(() => quiet.forms.submit("F-1", { status: "submitted", steps: {} }), 30);
    await call(quiet, "ask", { title: "t", steps: [{ title: "A" }] });
    assert.deepEqual(without, [], "no token, no progress");
});

test("refusals are results with the code first, and open nothing", async () => {
    const ctx = window();
    assert.match((await call(ctx, "ask", { title: "", steps: [{ title: "A" }] })).text, /^invalid: title/);
    assert.match((await call(ctx, "ask", { title: "t", steps: [] })).text, /^invalid: steps/);
    assert.match((await call(ctx, "ask", { title: "t", steps: [{ title: "A" }], wait: 1 })).text, /^invalid: ask takes no "wait"/);
    assert.deepEqual(ctx.forms.pending(), []);
});
