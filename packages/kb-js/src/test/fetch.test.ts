/* Fetching a page to file it, against a real HTTP server on this machine: the
 * bytes come back as they were served, with the type, the ETag and a title
 * read from the page; a redirect is followed; a conditional request can come
 * back 304; and a non-2xx or an unreachable host is an error, not a page. */

import * as assert from "node:assert/strict";
import * as http from "node:http";
import { AddressInfo } from "node:net";
import { test } from "node:test";

import { fetchPage, titleOf } from "../fetch";

const PAGE = "<!doctype html><html><head><title>  Spill\n  slots </title></head><body><p>x19 is callee-saved.</p></body></html>";

async function serve(): Promise<{ base: string; close(): Promise<void> }> {
    const server = http.createServer((req, res) => {
        if (req.url === "/page.html") {
            if (req.headers["if-none-match"] === '"v1"') {
                res.writeHead(304);
                res.end();
                return;
            }
            res.writeHead(200, { "content-type": "text/html; charset=utf-8", etag: '"v1"' });
            res.end(PAGE);
        } else if (req.url === "/notes.md") {
            res.writeHead(200, { "content-type": "text/markdown" });
            res.end("Intro line.\n# Deques\n\nChase-Lev.\n");
        } else if (req.url === "/moved") {
            res.writeHead(302, { location: "/page.html" });
            res.end();
        } else {
            res.writeHead(404, { "content-type": "text/plain" });
            res.end("not here");
        }
    });
    await new Promise<void>((r) => server.listen(0, "127.0.0.1", r));
    const { port } = server.address() as AddressInfo;
    return { base: `http://127.0.0.1:${port}`, close: () => new Promise((r) => server.close(() => r())) };
}

test("a page comes back as served, with its type, ETag and title", async () => {
    const s = await serve();
    try {
        const page = await fetchPage(`${s.base}/page.html`);
        assert.equal(page.notModified, false);
        if (page.notModified) return;
        assert.equal(page.text, PAGE, "the bytes as they arrived");
        assert.equal(page.mime, "text/html", "the type without its parameters");
        assert.equal(page.etag, '"v1"');
        assert.equal(page.title, "Spill slots");

        const md = await fetchPage(`${s.base}/notes.md`);
        assert.ok(!md.notModified && md.title === "Deques" && md.mime === "text/markdown");

        // A redirect is followed to the page.
        const moved = await fetchPage(`${s.base}/moved`);
        assert.ok(!moved.notModified && moved.text === PAGE);

        // Asked conditionally with the ETag already filed: not modified.
        assert.deepEqual(await fetchPage(`${s.base}/page.html`, '"v1"'), { notModified: true });
    } finally {
        await s.close();
    }
});

test("a non-2xx and an unreachable host are errors, not pages", async () => {
    const s = await serve();
    try {
        await assert.rejects(fetchPage(`${s.base}/gone`), /answered 404 Not Found; nothing was filed/);
    } finally {
        await s.close();
    }
    await assert.rejects(fetchPage("http://127.0.0.1:1/nothing-listens"), /Could not reach 127\.0\.0\.1:1/);
});

test("with no title in the page, the host and the last part of the path name it", () => {
    assert.equal(titleOf("plain words", "text/plain", "https://man7.org/linux/man-pages/man7/io_uring.7.html"), "man7.org io_uring.7.html");
    assert.equal(titleOf("", "text/html", "https://example.org/"), "example.org");
    assert.equal(titleOf("<title></title>", "text/html", "https://example.org/a"), "example.org a");
    assert.equal(titleOf("x", "text/plain", "not a url"), "not a url");
});
