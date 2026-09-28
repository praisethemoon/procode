import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { Eggzibit, EggzibitError, LEGACY_DIR, LEGACY_META, MAX_KEYWORD, MAX_KEYWORDS, defaultRoot, findEggzibit, hasKeyword, isPageId } from "../store";

function workspace(): string {
    return fs.mkdtempSync(path.join(os.tmpdir(), "eggzibit-"));
}

const refused = (code: string) => (e: unknown) => e instanceof EggzibitError && e.code === code;

test("publishing creates A-1, A-2 … with the page and its metadata", () => {
    const root = workspace();
    const store = new Eggzibit(root);
    const t0 = new Date("2026-09-26T10:00:00.123Z");
    const a = store.publish({ title: " IOCP vs io_uring ", html: "<h1>x</h1>", description: "a comparison" }, t0);
    assert.equal(a.page.id, "A-1");
    assert.equal(a.page.title, "IOCP vs io_uring");
    assert.equal(a.page.createdAt, "2026-09-26T10:00:00Z");
    assert.equal(a.page.bytes, 10);
    assert.equal(a.path, path.join(root, ".eggzibit", "A-1", "index.html"));
    assert.equal(fs.readFileSync(a.path, "utf8"), "<h1>x</h1>");
    const meta = JSON.parse(fs.readFileSync(path.join(root, ".eggzibit", "A-1", "page.json"), "utf8"));
    assert.deepEqual(meta, {
        id: "A-1",
        title: "IOCP vs io_uring",
        description: "a comparison",
        keywords: [],
        createdAt: "2026-09-26T10:00:00Z",
        updatedAt: "2026-09-26T10:00:00Z",
    });
    assert.equal(store.publish({ title: "second", html: "<p>2</p>" }).page.id, "A-2");
    assert.equal(findEggzibit(path.join(root, ".eggzibit", "A-1")), root);
});

test("republishing by id replaces the page and keeps createdAt", () => {
    const store = new Eggzibit(workspace());
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
    const store = new Eggzibit(root);
    store.publish({ title: "old", html: "x" }, new Date("2026-01-01T00:00:00Z"));
    store.publish({ title: "new", html: "x" }, new Date("2026-03-01T00:00:00Z"));
    store.publish({ title: "mid", html: "x" }, new Date("2026-02-01T00:00:00Z"));
    fs.mkdirSync(path.join(root, ".eggzibit", "notes"));
    fs.mkdirSync(path.join(root, ".eggzibit", "A-99")); // half-published: no metadata
    assert.deepEqual(
        store.list().map((a) => a.title),
        ["new", "mid", "old"],
    );
    assert.deepEqual(new Eggzibit(workspace()).list(), []);
});

test("a deleted page's id is never handed out again", () => {
    const root = workspace();
    const store = new Eggzibit(root);
    store.publish({ title: "a", html: "x" });
    store.publish({ title: "b", html: "x" });
    fs.rmSync(path.join(root, ".eggzibit", "A-2"), { recursive: true });
    assert.equal(store.publish({ title: "c", html: "x" }).page.id, "A-3");
});

test("bad input is refused before anything is written", () => {
    const root = workspace();
    const store = new Eggzibit(root);
    assert.throws(() => store.publish({ title: "", html: "x" }), refused("invalid"));
    assert.throws(() => store.publish({ title: "two\nlines", html: "x" }), refused("invalid"));
    assert.throws(() => store.publish({ title: "t".repeat(201), html: "x" }), refused("invalid"));
    assert.throws(() => store.publish({ title: "t", html: "   " }), refused("invalid"));
    assert.throws(() => store.publish({ title: "t", html: "x", description: "d".repeat(2001) }), refused("invalid"));
    assert.equal(fs.existsSync(path.join(root, ".eggzibit")), false, "a refusal created the directory");
    assert.throws(() => store.publish({ id: "A-7", title: "t", html: "x" }), refused("not_found"));
    assert.throws(() => store.get("A-7"), refused("not_found"));
});

test("an id is A-<n> and nothing that could name a path", () => {
    for (const bad of ["../A-1", "A-1/../../etc", "A-01", "A-0", "a-1", "A-", "A-1 ", ".eggzibit", ""]) {
        assert.equal(isPageId(bad), false, bad);
        assert.throws(() => new Eggzibit(workspace()).get(bad), refused("bad_id"), bad);
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
    const store = new Eggzibit(root);
    const a = store.publish({ title: "t", html: "<p>x</p>", keywords: [" Lap ", "merge", "LAP", "  design   note ", ""] });
    assert.deepEqual(a.page.keywords, ["lap", "merge", "design note"]);
    const meta = JSON.parse(fs.readFileSync(path.join(root, ".eggzibit", "A-1", "page.json"), "utf8"));
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
    const store = new Eggzibit(root);
    store.publish({ title: "t", html: "<p>1</p>", keywords: ["lap"] });
    assert.deepEqual(store.publish({ id: "A-1", title: "t", html: "<p>2</p>" }).page.keywords, ["lap"]);
    assert.deepEqual(store.publish({ id: "A-1", title: "t", html: "<p>3</p>", keywords: ["kb"] }).page.keywords, ["kb"]);
    assert.deepEqual(store.publish({ id: "A-1", title: "t", html: "<p>4</p>", keywords: [] }).page.keywords, []);

    /* written before keywords existed */
    fs.mkdirSync(path.join(root, ".eggzibit", "A-9"));
    fs.writeFileSync(path.join(root, ".eggzibit", "A-9", "index.html"), "<p>old</p>");
    fs.writeFileSync(path.join(root, ".eggzibit", "A-9", "page.json"),
        JSON.stringify({ id: "A-9", title: "old", description: "", createdAt: "2026-01-01T00:00:00Z", updatedAt: "2026-01-01T00:00:00Z" }));
    assert.deepEqual(store.get("A-9").page.keywords, []);
});

test("a store from before the rename is read where it is, and moved on the first write, ids and dates kept", () => {
    const root = workspace();
    const old = path.join(root, LEGACY_DIR);
    for (const [id, title, at] of [["A-1", "first", "2026-01-01T00:00:00Z"], ["A-3", "third", "2026-02-01T00:00:00Z"]]) {
        fs.mkdirSync(path.join(old, id), { recursive: true });
        fs.writeFileSync(path.join(old, id, "index.html"), `<p>${title}</p>`);
        fs.writeFileSync(path.join(old, id, LEGACY_META), JSON.stringify({ id, title, description: "", createdAt: at, updatedAt: at }));
    }
    fs.writeFileSync(path.join(old, "next"), "4\n");
    const nested = path.join(root, "src");
    fs.mkdirSync(nested);
    assert.equal(findEggzibit(nested), root, "the old folder is found walking up");
    const store = new Eggzibit(root);
    assert.deepEqual(store.list().map((p) => p.id), ["A-3", "A-1"], "read where it is");
    assert.equal(store.get("A-1").page.createdAt, "2026-01-01T00:00:00Z");
    assert.equal(fs.existsSync(path.join(root, ".eggzibit")), false, "reading moves nothing");

    const made = store.publish({ title: "fourth", html: "<p>4</p>" }, new Date("2026-03-01T00:00:00Z"));
    assert.equal(made.page.id, "A-4", "the counter came along");
    assert.equal(fs.existsSync(old), false, "the old folder is gone");
    for (const id of ["A-1", "A-3"]) {
        assert.ok(fs.existsSync(path.join(root, ".eggzibit", id, "page.json")), `${id}'s metadata is page.json`);
        assert.equal(fs.existsSync(path.join(root, ".eggzibit", id, LEGACY_META)), false);
    }
    assert.deepEqual(
        store.list().map((p) => [p.id, p.title, p.createdAt]),
        [["A-4", "fourth", "2026-03-01T00:00:00Z"], ["A-3", "third", "2026-02-01T00:00:00Z"], ["A-1", "first", "2026-01-01T00:00:00Z"]],
    );
});
