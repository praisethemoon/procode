import * as assert from "node:assert/strict";
import { test } from "node:test";

import { Forms } from "../forms";

const text = [{ id: "a", title: "A", kind: "text" as const }];

test("forms are numbered per window, and opening one tells the tab", () => {
    const forms = new Forms();
    const opened: string[] = [];
    forms.on("open", (r) => opened.push(r.id));
    forms.create("t", text);
    forms.create("u", text);
    assert.deepEqual(opened, ["F-1", "F-2"]);
    assert.deepEqual(forms.pending().map((r) => r.id), ["F-1", "F-2"]);
    assert.throws(() => forms.request("F-3"), /no form F-3/);
    assert.throws(() => forms.request("../x"), /not a form id/);
});

test("submit keeps only the steps asked, and the first answer stands", () => {
    const forms = new Forms();
    const { id } = forms.create("t", text);
    assert.equal(forms.submit(id, { status: "submitted", steps: { a: { state: "answered", text: "x" }, zz: { state: "skipped" } } }), true);
    assert.deepEqual(forms.answer(id)!.steps, { a: { state: "answered", text: "x" } });
    assert.equal(forms.submit(id, { status: "submitted", steps: {} }), false);
    forms.cancel(id);
    assert.equal(forms.answer(id)!.status, "submitted", "cancelling an answered form changes nothing");
    assert.deepEqual(forms.pending(), []);
});

test("wait resolves on the answer, at the timeout, or on abort, and leaves no timers", async () => {
    const forms = new Forms();
    const a = forms.create("t", text);
    setTimeout(() => forms.submit(a.id, { status: "submitted", steps: {} }), 5);
    assert.equal((await forms.wait(a.id, { timeoutMs: 1000 })).status, "submitted");
    assert.equal((await forms.wait(a.id, { timeoutMs: 1000 })).status, "submitted", "an answered form answers at once");

    const b = forms.create("t", text);
    assert.equal((await forms.wait(b.id, { timeoutMs: 10 })).status, "waiting");

    const ac = new AbortController();
    setTimeout(() => ac.abort(), 5);
    assert.equal((await forms.wait(b.id, { timeoutMs: 1000, signal: ac.signal })).status, "aborted");

    let ticks = 0;
    setTimeout(() => forms.cancel(b.id), 30);
    assert.equal((await forms.wait(b.id, { timeoutMs: 1000, tickMs: 5, onTick: () => ticks++ })).status, "cancelled");
    assert.ok(ticks >= 2);
});
