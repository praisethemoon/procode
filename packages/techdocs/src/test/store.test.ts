import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { Techdocs, TechdocsError, MAX_KEYWORD, MAX_KEYWORDS, defaultRoot, findTechdocs, hasKeyword, isPageId } from "../store";

function workspace(): string {
    return fs.mkdtempSync(path.join(os.tmpdir(), "techdocs-"));
}

const refused = (code: string) => (e: unknown) => e instanceof TechdocsError && e.code === code;

test("publishing creates A-1, A-2 … with the page and its metadata", () => {
    const root = workspace();
    const store = new Techdocs(root);
    const t0 = new Date("2026-09-26T10:00:00.123Z");
    const a = store.publish({ title: " IOCP vs io_uring ", html: "<h1>x</h1>", description: "a comparison" }, t0);
    assert.equal(a.page.id, "A-1");
    assert.equal(a.page.title, "IOCP vs io_uring");
    assert.equal(a.page.createdAt, "2026-09-26T10:00:00Z");
    assert.equal(a.page.bytes, 10);
    assert.equal(a.path, path.join(root, ".techdocs", "A-1", "index.html"));
    assert.equal(fs.readFileSync(a.path, "utf8"), "<h1>x</h1>");
    const meta = JSON.parse(fs.readFileSync(path.join(root, ".techdocs", "A-1", "page.json"), "utf8"));
    assert.deepEqual(meta, {
        id: "A-1",
        title: "IOCP vs io_uring",
        description: "a comparison",
        keywords: [],
        createdAt: "2026-09-26T10:00:00Z",
        updatedAt: "2026-09-26T10:00:00Z",
    });
    assert.equal(store.publish({ title: "second", html: "<p>2</p>" }).page.id, "A-2");
    assert.equal(findTechdocs(path.join(root, ".techdocs", "A-1")), root);
});

test("republishing by id replaces the page and keeps createdAt", () => {
    const store = new Techdocs(workspace());
    store.publish({ title: "v1", html: "<p>1</p>" }, new Date("2026-01-01T00:00:00Z"));
    const b = store.publish({ id: "A-1", title: "v2", html: "<p>two</p>" }, new Date("2026-02-01T00:00:00Z"));
    assert.equal(b.page.createdAt, "2026-01-01T00:00:00Z");
    assert.equal(b.page.updatedAt, "2026-02-01T00:00:00Z");
    const got = store.get("A-1");
    assert.equal(got.html, "<p>two</p>");
    assert.equal(got.page.title, "v2");
});

test("the list is newest update first, and skips what is not a page", () => {
    const root = workspace();
    const store = new Techdocs(root);
    store.publish({ title: "old", html: "x" }, new Date("2026-01-01T00:00:00Z"));
    store.publish({ title: "new", html: "x" }, new Date("2026-03-01T00:00:00Z"));
    store.publish({ title: "mid", html: "x" }, new Date("2026-02-01T00:00:00Z"));
    fs.mkdirSync(path.join(root, ".techdocs", "notes"));
    fs.mkdirSync(path.join(root, ".techdocs", "A-99")); // half-published: no metadata
    assert.deepEqual(
        store.list().map((a) => a.title),
        ["new", "mid", "old"],
    );
    assert.deepEqual(new Techdocs(workspace()).list(), []);
});

test("a deleted page's id is never handed out again", () => {
    const root = workspace();
    const store = new Techdocs(root);
    store.publish({ title: "a", html: "x" });
    store.publish({ title: "b", html: "x" });
    fs.rmSync(path.join(root, ".techdocs", "A-2"), { recursive: true });
    assert.equal(store.publish({ title: "c", html: "x" }).page.id, "A-3");
});

test("bad input is refused before anything is written", () => {
    const root = workspace();
    const store = new Techdocs(root);
    assert.throws(() => store.publish({ title: "", html: "x" }), refused("invalid"));
    assert.throws(() => store.publish({ title: "two\nlines", html: "x" }), refused("invalid"));
    assert.throws(() => store.publish({ title: "t".repeat(201), html: "x" }), refused("invalid"));
    assert.throws(() => store.publish({ title: "t", html: "   " }), refused("invalid"));
    assert.throws(() => store.publish({ title: "t", html: "x", description: "d".repeat(2001) }), refused("invalid"));
    assert.equal(fs.existsSync(path.join(root, ".techdocs")), false, "a refusal created the directory");
    assert.throws(() => store.publish({ id: "A-7", title: "t", html: "x" }), refused("not_found"));
    assert.throws(() => store.get("A-7"), refused("not_found"));
});

test("an id is A-<n> and nothing that could name a path", () => {
    for (const bad of ["../A-1", "A-1/../../etc", "A-01", "A-0", "a-1", "A-", "A-1 ", ".techdocs", ""]) {
        assert.equal(isPageId(bad), false, bad);
        assert.throws(() => new Techdocs(workspace()).get(bad), refused("bad_id"), bad);
    }
    assert.equal(isPageId("A-12"), true);
});

test("a first page goes to the repository root, not a subdirectory", () => {
    const root = workspace();
    fs.mkdirSync(path.join(root, ".git"));
    fs.mkdirSync(path.join(root, "src", "deep"), { recursive: true });
    assert.equal(defaultRoot(path.join(root, "src", "deep")), root);
    const outside = workspace();
    assert.equal(defaultRoot(outside), outside);
});

test("keywords are saved lowercase, trimmed and once each, within their limits", () => {
    const root = workspace();
    const store = new Techdocs(root);
    const a = store.publish({ title: "t", html: "<p>x</p>", keywords: [" Lap ", "merge", "LAP", "  design   note ", ""] });
    assert.deepEqual(a.page.keywords, ["lap", "merge", "design note"]);
    const meta = JSON.parse(fs.readFileSync(path.join(root, ".techdocs", "A-1", "page.json"), "utf8"));
    assert.deepEqual(meta.keywords, ["lap", "merge", "design note"]);
    assert.deepEqual(store.get("A-1").page.keywords, ["lap", "merge", "design note"]);
    assert.ok(hasKeyword(a.page, " Design Note ") && !hasKeyword(a.page, "design"));

    const many = Array.from({ length: MAX_KEYWORDS + 1 }, (_, i) => `k${i}`);
    assert.throws(() => store.publish({ title: "t", html: "<p>x</p>", keywords: many }), refused("invalid"));
    assert.throws(() => store.publish({ title: "t", html: "<p>x</p>", keywords: ["x".repeat(MAX_KEYWORD + 1)] }), refused("invalid"));
    assert.throws(() => store.publish({ title: "t", html: "<p>x</p>", keywords: "lap" as unknown as string[] }), refused("invalid"));
    assert.throws(() => store.publish({ title: "t", html: "<p>x</p>", keywords: [3 as unknown as string] }), refused("invalid"));
    assert.equal(store.list().length, 1, "a refused publish wrote nothing");
});

test("a republish replaces the keywords when given, keeps them when not; an old page has none", () => {
    const root = workspace();
    const store = new Techdocs(root);
    store.publish({ title: "t", html: "<p>1</p>", keywords: ["lap"] });
    assert.deepEqual(store.publish({ id: "A-1", title: "t", html: "<p>2</p>" }).page.keywords, ["lap"]);
    assert.deepEqual(store.publish({ id: "A-1", title: "t", html: "<p>3</p>", keywords: ["kb"] }).page.keywords, ["kb"]);
    assert.deepEqual(store.publish({ id: "A-1", title: "t", html: "<p>4</p>", keywords: [] }).page.keywords, []);

    /* written before keywords existed */
    fs.mkdirSync(path.join(root, ".techdocs", "A-9"));
    fs.writeFileSync(path.join(root, ".techdocs", "A-9", "index.html"), "<p>old</p>");
    fs.writeFileSync(path.join(root, ".techdocs", "A-9", "page.json"),
        JSON.stringify({ id: "A-9", title: "old", description: "", createdAt: "2026-01-01T00:00:00Z", updatedAt: "2026-01-01T00:00:00Z" }));
    assert.deepEqual(store.get("A-9").page.keywords, []);
});
