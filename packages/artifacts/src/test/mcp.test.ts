import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { INSTRUCTIONS, handle } from "../mcp";

type Json = Record<string, unknown>;

async function call(cwd: string, name: string, args: Json): Promise<{ isError: boolean; text: string }> {
    const out = (await handle({ jsonrpc: "2.0", id: 1, method: "tools/call", params: { name, arguments: args } }, { cwd })) as Json;
    const result = out["result"] as Json;
    const content = result["content"] as { text: string }[];
    return { isError: result["isError"] === true, text: content[0].text };
}

test("the server introduces itself with the tokens a page should use", async () => {
    const out = (await handle({ jsonrpc: "2.0", id: 1, method: "initialize", params: {} }, { cwd: "/" })) as Json;
    const r = out["result"] as Json;
    assert.equal((r["serverInfo"] as Json)["name"], "artifacts");
    assert.match(String(r["instructions"]), /--bk-color-foreground/);
    assert.match(INSTRUCTIONS, /no network/);
    const tools = (await handle({ jsonrpc: "2.0", id: 2, method: "tools/list" }, { cwd: "/" })) as Json;
    assert.deepEqual(
        ((tools["result"] as Json)["tools"] as { name: string }[]).map((t) => t.name),
        ["artifact_publish", "artifact_list", "artifact_get"],
    );
});

test("publish, list and get, from a subdirectory of the repository", async () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "artifacts-mcp-"));
    fs.mkdirSync(path.join(root, ".git"));
    const sub = path.join(root, "src");
    fs.mkdirSync(sub);
    const empty = await call(sub, "artifact_list", {});
    assert.deepEqual(JSON.parse(empty.text), { artifacts: [], count: 0 });

    const pub = await call(sub, "artifact_publish", { title: "Findings", html: "<h1>Findings</h1>", description: "what we learned" });
    assert.equal(pub.isError, false);
    const published = JSON.parse(pub.text);
    assert.equal(published.artifact.id, "A-1");
    assert.equal(published.path, path.join(root, ".artifact", "A-1", "index.html"));

    const list = JSON.parse((await call(sub, "artifact_list", {})).text);
    assert.equal(list.count, 1);
    assert.equal(list.artifacts[0].description, "what we learned");
    const got = JSON.parse((await call(root, "artifact_get", { id: "A-1" })).text);
    assert.equal(got.html, "<h1>Findings</h1>");
});

test("refusals are results the agent can read, with the code first", async () => {
    const cwd = fs.mkdtempSync(path.join(os.tmpdir(), "artifacts-mcp-"));
    const noTitle = await call(cwd, "artifact_publish", { title: "", html: "x" });
    assert.equal(noTitle.isError, true);
    assert.match(noTitle.text, /^invalid: /);
    assert.match((await call(cwd, "artifact_get", { id: "A-4" })).text, /^not_found: /);
    await call(cwd, "artifact_publish", { title: "t", html: "x" });
    assert.match((await call(cwd, "artifact_get", { id: "../../etc/passwd" })).text, /^bad_id: /);
    assert.match((await call(cwd, "artifact_get", { id: "A-4" })).text, /^not_found: /);
    const extra = await call(cwd, "artifact_publish", { title: "t", html: "x", delete: true });
    assert.match(extra.text, /^invalid: artifact_publish takes no "delete"/);
});
