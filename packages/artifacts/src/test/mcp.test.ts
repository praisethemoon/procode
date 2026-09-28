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
        ["artifact_publish", "artifact_template", "artifact_list", "artifact_get"],
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

test("the report template is served, and it carries no colour of its own", async () => {
    const list = JSON.parse((await call("/", "artifact_template", {})).text);
    assert.deepEqual(list.templates.map((t: { name: string }) => t.name), ["report"]);
    const report = JSON.parse((await call("/", "artifact_template", { name: "report" })).text);
    const html: string = report.html;
    assert.match(html, /class="kpis"/);
    assert.match(html, /class="callout ok"/);
    assert.match(html, /<svg class="chart"/);
    assert.match(html, /class="tabs"/);
    // Every colour comes from the viewer: no CSS, no inline style, no literal colour.
    assert.doesNotMatch(html, /<style|style="/);
    assert.doesNotMatch(html, /#[0-9a-fA-F]{3,8}\b|rgba?\(|hsla?\(/);
    assert.doesNotMatch(html, /\b(fill|stroke|color)=/);
    assert.match((await call("/", "artifact_template", { name: "slides" })).text, /^not_found: /);
});

test("every class the template uses is one the viewer styles", () => {
    const { TEMPLATES } = require("../templates") as typeof import("../templates");
    const css = require("node:fs").readFileSync(require("node:path").join(__dirname, "../../../artifacts-vscode/assets/artifact.css"), "utf8") as string;
    const used = new Set<string>();
    for (const m of TEMPLATES[0].html.matchAll(/class="([^"]+)"/g)) for (const c of m[1].split(/\s+/)) used.add(c);
    const own = new Set(["filterable", "label", "muted"]); // hooks for the script, and styled elsewhere
    for (const c of used) {
        if (own.has(c) && c !== "label" && c !== "muted") continue;
        assert.match(css, new RegExp(`\\.${c}\\b`), `.${c} is used by the template and not styled by the viewer`);
    }
});

test("keywords: publish with them, list with and without the keyword filter, get", async () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "artifacts-mcp-"));
    fs.mkdirSync(path.join(root, ".git"));
    await call(root, "artifact_publish", { title: "Merge notes", html: "<p>m</p>", keywords: ["lap", "Merge"] });
    await call(root, "artifact_publish", { title: "Search notes", html: "<p>s</p>", keywords: ["kb"] });
    await call(root, "artifact_publish", { title: "Plain", html: "<p>p</p>" });

    const all = JSON.parse((await call(root, "artifact_list", {})).text);
    assert.equal(all.count, 3);
    assert.deepEqual(all.artifacts.map((a: Json) => a["keywords"]), [[], ["kb"], ["lap", "merge"]]);
    const merge = JSON.parse((await call(root, "artifact_list", { keyword: "MERGE" })).text);
    assert.deepEqual(merge.artifacts.map((a: Json) => a["id"]), ["A-1"]);
    assert.equal(JSON.parse((await call(root, "artifact_list", { keyword: "none" })).text).count, 0);
    assert.deepEqual(JSON.parse((await call(root, "artifact_get", { id: "A-2" })).text).artifact.keywords, ["kb"]);

    const bad = await call(root, "artifact_publish", { title: "x", html: "<p>x</p>", keywords: "lap" });
    assert.equal(bad.isError, true);
    assert.match(bad.text, /^invalid: keywords must be a list/);
    assert.match(INSTRUCTIONS, /two to five keywords/);
});
