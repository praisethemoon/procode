import * as assert from "node:assert/strict";
import { test } from "node:test";

import { documentLocation, linkTarget, localPath } from "../view/locator";

const there = (...paths: string[]) => (p: string) => paths.includes(p);

test("a folder's document is at the folder's path joined with its own", () => {
    assert.equal(documentLocation("/Users/me/lap/cli/kb-cli", "src/simd.h"), "/Users/me/lap/cli/kb-cli/src/simd.h");
    assert.equal(documentLocation("/Users/me/lap/cli/kb-cli/", "/src/simd.h"), "/Users/me/lap/cli/kb-cli/src/simd.h", "no doubled separator");
    assert.equal(documentLocation("file:///Users/me/lap/cli/kb-cli", "src/simd.h"), "/Users/me/lap/cli/kb-cli/src/simd.h");
    assert.equal(documentLocation("C:\\work\\kb-cli", "src/simd.h"), "C:\\work\\kb-cli\\src\\simd.h");
});

test("a file's document is the file; a web document is its locator as filed", () => {
    assert.equal(documentLocation("/Users/me/notes/iocp.md", ""), "/Users/me/notes/iocp.md");
    assert.equal(documentLocation("file:///Users/me/notes/a%20b.md", ""), "/Users/me/notes/a b.md");
    assert.equal(documentLocation("https://example.test/iocp", ""), "https://example.test/iocp");
    assert.equal(documentLocation("https://example.test/iocp", "ignored"), "https://example.test/iocp");
});

test("local paths: bare absolute paths and file URLs, not relative paths or other schemes", () => {
    assert.equal(localPath("/a/b"), "/a/b");
    assert.equal(localPath("C:/a/b"), "C:/a/b");
    assert.equal(localPath("\\\\server\\share\\x"), "\\\\server\\share\\x");
    assert.equal(localPath("file:///C:/a/b"), "C:/a/b");
    assert.equal(localPath("file://server/share/x"), "//server/share/x");
    assert.equal(localPath("a/b"), null);
    assert.equal(localPath("https://example.test/"), null);
    assert.equal(localPath("vscode://settings"), null);
});

test("a link opens a local file that is there, and says so when it is gone", () => {
    const p = "/Users/me/lap/cli/kb-cli/src/simd.h";
    assert.deepEqual(linkTarget(p, there(p)), { kind: "file", path: p });
    assert.deepEqual(linkTarget(`file://${p}`, there(p)), { kind: "file", path: p });
    assert.deepEqual(linkTarget(p, there()), { kind: "missing", path: p });
});

test("web links go to the web; any other scheme is refused", () => {
    assert.deepEqual(linkTarget(" https://example.test/iocp ", there()), { kind: "web", url: "https://example.test/iocp" });
    assert.deepEqual(linkTarget("mailto:a@example.test", there()), { kind: "web", url: "mailto:a@example.test" });
    assert.deepEqual(linkTarget("javascript:alert(1)", there()), { kind: "refused", href: "javascript:alert(1)" });
    assert.deepEqual(linkTarget("relative/path", there("relative/path")), { kind: "refused", href: "relative/path" });
});
