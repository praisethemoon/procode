import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { Ask } from "../store";

test("submit writes the tab's answer once, keeping only the steps asked", () => {
    const ask = new Ask(fs.mkdtempSync(path.join(os.tmpdir(), "ask-store-")));
    const req = ask.create("t", [{ id: "a", title: "A", kind: "text" }]);
    assert.equal(ask.submit(req.id, { status: "submitted", steps: { a: { state: "answered", text: "x" }, zz: { state: "skipped" } } }), true);
    assert.deepEqual(ask.answer(req.id)!.steps, { a: { state: "answered", text: "x" } });
    assert.equal(ask.submit(req.id, { status: "cancelled", steps: {} }), false, "the first answer stands");
    assert.equal(ask.answer(req.id)!.status, "submitted");
    assert.throws(() => ask.submit(req.id + "0", { status: "submitted" }), /no form/);
});

test("pending lists the forms not answered yet, oldest first", () => {
    const ask = new Ask(fs.mkdtempSync(path.join(os.tmpdir(), "ask-store-")));
    assert.deepEqual(ask.pending(), []);
    for (let i = 0; i < 11; i++) ask.create(`t${i}`, [{ id: "a", title: "A", kind: "text" }]);
    ask.cancel("F-2");
    const ids = ask.pending().map((r) => r.id);
    assert.equal(ids.length, 10);
    assert.deepEqual(ids.slice(0, 2), ["F-1", "F-3"]);
    assert.equal(ids[9], "F-11");
});
