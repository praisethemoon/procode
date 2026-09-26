/* One document refreshed from its own source, against the real kb binary and
 * a stand-in for the `vscode` module: a file that changed is re-indexed, one
 * that did not only moves its fetch date, a declined fetch files nothing, and
 * inline content says there is nowhere to go back to. */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { Kb } from "kb-js";

import { TEST_ENV } from "./home";

// eslint-disable-next-line @typescript-eslint/no-require-imports
const Module = require("node:module") as { _load: (req: string, parent: unknown, isMain: boolean) => unknown };

const KB = path.resolve(__dirname, "../../../../cli/kb-cli/bin/kb");
let answer: string | undefined = undefined;
const fake = {
    window: { showWarningMessage: async () => answer },
    workspace: { fs: { readFile: async (u: { fsPath: string }) => fs.readFileSync(u.fsPath) } },
    Uri: { file: (p: string) => ({ fsPath: p }) },
};

type Commands = typeof import("../commands");

function load(): Pick<Commands, "refreshDocument" | "fetchPage"> {
    const original = Module._load;
    Module._load = (req, parent, isMain) => (req === "vscode" ? fake : original(req, parent, isMain));
    try {
        return require("../commands") as Commands;
    } finally {
        Module._load = original;
    }
}

test("a document is refreshed from its own source", { skip: !fs.existsSync(KB) && "kb is not built" }, async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kb-refresh-"));
    const kb = new Kb({ bin: KB, cwd: dir, env: TEST_ENV });
    await kb.init();
    const { refreshDocument } = load();

    const file = path.join(dir, "notes.md");
    fs.writeFileSync(file, "# IOCP\n\nThe first version.\n");
    const filed = await kb.add(fs.readFileSync(file, "utf8"), {
        title: "Notes",
        collection: "win32",
        url: file,
        meta: { year: 2026 },
    });

    fs.writeFileSync(file, "# IOCP\n\nThe second version.\n");
    const changed = await refreshDocument(kb, filed.document);
    assert.equal(changed.outcome, "updated");
    const after = await kb.get(filed.document, { text: true });
    assert.match(after.text ?? "", /second version/);
    assert.equal(after.document.title, "Notes", "the title is kept");
    assert.equal(after.document.collection, "win32", "the collection is kept");
    assert.deepEqual(after.document.meta, { year: 2026 }, "the meta is kept");

    const same = await refreshDocument(kb, filed.document);
    assert.equal(same.outcome, "unchanged");

    fs.renameSync(file, file + ".moved");
    const gone = await refreshDocument(kb, filed.document);
    assert.equal(gone.outcome, "cannot");

    const inline = await kb.add("handed over\n", { title: "Inline", collection: "notes" });
    const none = await refreshDocument(kb, inline.document);
    assert.equal(none.outcome, "cannot");

    // A URL is fetched only when the reader agrees; declining files nothing.
    const page = await kb.add("<p>page</p>", { title: "Page", collection: "web", url: "https://example.invalid/page" });
    answer = undefined;
    const declined = await refreshDocument(kb, page.document);
    assert.equal(declined.outcome, "declined");
    assert.equal((await kb.get(page.document)).document.fetchedAt, page.fetchedAt);
});

test("a page is refreshed conditionally on its etag, and a failure files nothing", { skip: !fs.existsSync(KB) && "kb is not built" }, async () => {
    // A tiny site: one page whose body and ETag the test changes, and which
    // answers 304 to a matching If-None-Match, as a real server does.
    const http = await import("node:http");
    let body = "<h1>IOCP</h1><p>first</p>";
    let etag = '"v1"';
    let status = 200;
    const seen: (string | undefined)[] = [];
    const server = http.createServer((req, res) => {
        seen.push(req.headers["if-none-match"] as string | undefined);
        if (status !== 200) {
            res.writeHead(status);
            res.end();
            return;
        }
        if (req.headers["if-none-match"] === etag) {
            res.writeHead(304, { etag });
            res.end();
            return;
        }
        res.writeHead(200, { "content-type": "text/html; charset=utf-8", etag });
        res.end(body);
    });
    await new Promise<void>((r) => server.listen(0, "127.0.0.1", r));
    const url = `http://127.0.0.1:${(server.address() as { port: number }).port}/iocp`;
    try {
        const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kb-url-"));
        const kb = new Kb({ bin: KB, cwd: dir, env: TEST_ENV });
        await kb.init();
        const { refreshDocument, fetchPage } = load();

        // What Add URL files: the page as it arrived, with its ETag.
        const page = await fetchPage(url);
        assert.ok(!page.notModified);
        assert.equal(page.etag, '"v1"');
        assert.equal(page.mime, "text/html");
        const filed = await kb.add(page.text, { title: "IOCP", collection: "web", url, mime: page.mime, etag: page.etag });
        assert.equal((await kb.sources())[0].etag, '"v1"');

        answer = "Fetch";
        const same = await refreshDocument(kb, filed.document);
        assert.equal(same.outcome, "unchanged");
        assert.equal(seen.at(-1), '"v1"', "the refresh asked conditionally");

        body = "<h1>IOCP</h1><p>second</p>";
        etag = '"v2"';
        const changed = await refreshDocument(kb, filed.document);
        assert.equal(changed.outcome, "updated");
        assert.match((await kb.get(filed.document, { text: true })).text ?? "", /second/);
        assert.equal((await kb.sources())[0].etag, '"v2"');

        status = 500;
        await assert.rejects(refreshDocument(kb, filed.document), /answered 500.*nothing was filed/);
        assert.match((await kb.get(filed.document, { text: true })).text ?? "", /second/, "a failed fetch changed nothing");

        await assert.rejects(fetchPage("http://127.0.0.1:1/nothing-listens"), /Could not reach 127\.0\.0\.1:1/);
    } finally {
        answer = undefined;
        server.close();
    }
});
