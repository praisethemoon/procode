/* A document's address, and the lookup from a URL back to the document. */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import { addressIndex, addressKey, documentAddress } from "../address";
import type { KbDocument } from "../types";

function doc(id: string, locator: string, path = ""): KbDocument {
    return { id, locator, path } as unknown as KbDocument;
}

test("a web page's address is its locator; a folder file's is the locator joined with its path", () => {
    assert.equal(documentAddress(doc("D-1", "https://docs.example.com/guide/")), "https://docs.example.com/guide/");
    assert.equal(documentAddress(doc("D-2", "/home/me/proj", "src/main.c")), "file:///home/me/proj/src/main.c");
    assert.equal(documentAddress(doc("D-3", "file:///home/me/proj/", "a b.md")), "file:///home/me/proj/a%20b.md");
    assert.equal(documentAddress(doc("D-4", "C:\\work\\proj", "docs\\x.md")), "file:///C:/work/proj/docs/x.md");
});

test("content handed in directly has no address", () => {
    assert.equal(documentAddress(doc("D-5", "")), null);
    assert.equal(documentAddress(doc("D-6", "notes from a meeting")), null);
});

test("the ways one page is written share one key; the query still tells pages apart", () => {
    const k = addressKey("https://docs.example.com/guide/");
    assert.equal(addressKey("https://docs.example.com/guide"), k);
    assert.equal(addressKey("https://docs.example.com/guide/index.html"), k);
    assert.equal(addressKey("https://DOCS.example.com/guide/#setup"), k);
    assert.notEqual(addressKey("https://docs.example.com/guide/?v=2"), k);
    assert.equal(addressKey("mailto:someone@example.com"), null);
    assert.equal(addressKey("not a url"), null);
});

test("the index finds a document by any way its address is written, the first listed keeping a shared one", () => {
    const index = addressIndex([
        doc("D-1", "https://docs.example.com/guide/"),
        doc("D-2", "/home/me/proj", "src/main.c"),
        doc("D-3", "https://docs.example.com/guide/index.html"),
        doc("D-4", ""),
    ]);
    assert.equal(index.get(addressKey("https://docs.example.com/guide")!), "D-1");
    assert.equal(index.get(addressKey("file:///home/me/proj/src/main.c")!), "D-2");
    assert.equal(index.size, 2);
});
