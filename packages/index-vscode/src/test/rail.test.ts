import * as assert from "node:assert/strict";
import { test } from "node:test";

import type { KbCollection, KbDocument } from "kb-js/pure";

import { HOME, PAGE, collectionsShown, cut, descriptionOf, documentRow, documentsQuery, openCollection, searchScope } from "../view/rail";

test("the sidebar starts on the collections, opens one, and comes back", () => {
    assert.deepEqual(HOME, { kind: "collections" });
    const inside = openCollection("win32-iocp");
    assert.deepEqual(inside, { kind: "collection", name: "win32-iocp" });
    assert.deepEqual(HOME, { kind: "collections" }, "back is the place it started");
});

test("a query searches the whole store from the collections, and the open collection from inside it", () => {
    assert.equal(searchScope(HOME), "");
    assert.equal(searchScope(openCollection("papers")), "papers");
});

test("a collection's documents are asked for a page at a time, newest first, after the last row shown", () => {
    assert.deepEqual(documentsQuery("papers", null), { collection: "papers", limit: PAGE + 1, reverse: true });
    assert.deepEqual(documentsQuery("papers", "D-41"), { collection: "papers", limit: PAGE + 1, reverse: true, after: "D-41" });
    const page = (n: number) => Array.from({ length: n }, (_, i) => ({ id: `D-${1000 - i}` }));
    const full = cut(page(PAGE + 1));
    assert.equal(full.shown.length, PAGE);
    assert.equal(full.next, `D-${1000 - PAGE + 1}`, "Load more continues after the last row drawn");
    assert.deepEqual(cut(page(PAGE)), { shown: page(PAGE), next: null }, "exactly a page: no more to load");
    assert.deepEqual(cut(page(3)).next, null);
});

test("the collections are shown by name, a page more for each Load more", () => {
    const c = (name: string): KbCollection => ({ name, documents: 1, bytes: 1 });
    const many = Array.from({ length: PAGE + 7 }, (_, i) => c(`t${String(i).padStart(3, "0")}`)).reverse();
    const first = collectionsShown(many, 1);
    assert.equal(first.shown.length, PAGE);
    assert.equal(first.more, true);
    assert.equal(first.shown[0].name, "t000", "sorted by name");
    const second = collectionsShown(many, 2);
    assert.equal(second.shown.length, PAGE + 7);
    assert.equal(second.more, false);
});

test("a document row carries its size, type and fetch date, and a description from its metadata when there is one", () => {
    const d = (meta: Record<string, unknown>, title = "Completion ports"): KbDocument =>
        ({ id: "D-7", title, collection: "c", mime: "text/markdown", bytes: 2048, fetchedAt: "2026-09-01T00:00:00Z", meta }) as unknown as KbDocument;
    assert.deepEqual(documentRow(d({ description: "  How IOCP\nqueues  work. " })), {
        reference: "D-7",
        title: "Completion ports",
        bytes: 2048,
        mime: "text/markdown",
        fetchedAt: "2026-09-01T00:00:00Z",
        description: "How IOCP queues work.",
    });
    assert.equal(descriptionOf({ abstract: "We measure." }), "We measure.", "a paper's abstract");
    assert.equal(descriptionOf({ description: "   ", summary: "Short." }), "Short.", "a blank one gives way to the next");
    assert.equal(descriptionOf({ description: 3, tags: ["x"] }), null, "none that is text");
    assert.equal(descriptionOf({}), null);
    assert.equal(documentRow(d({}, "")).title, "D-7", "an untitled document is named by its id");
});
