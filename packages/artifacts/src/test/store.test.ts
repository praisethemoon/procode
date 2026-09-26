import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { ArtifactError, Artifacts, defaultRoot, findArtifacts, isArtifactId } from "../store";

function workspace(): string {
    return fs.mkdtempSync(path.join(os.tmpdir(), "artifacts-"));
}

const refused = (code: string) => (e: unknown) => e instanceof ArtifactError && e.code === code;

test("publishing creates A-1, A-2 … with the page and its metadata", () => {
    const root = workspace();
    const store = new Artifacts(root);
    const t0 = new Date("2026-09-26T10:00:00.123Z");
    const a = store.publish({ title: " IOCP vs io_uring ", html: "<h1>x</h1>", description: "a comparison" }, t0);
    assert.equal(a.artifact.id, "A-1");
    assert.equal(a.artifact.title, "IOCP vs io_uring");
    assert.equal(a.artifact.createdAt, "2026-09-26T10:00:00Z");
    assert.equal(a.artifact.bytes, 10);
    assert.equal(a.path, path.join(root, ".artifact", "A-1", "index.html"));
    assert.equal(fs.readFileSync(a.path, "utf8"), "<h1>x</h1>");
    const meta = JSON.parse(fs.readFileSync(path.join(root, ".artifact", "A-1", "artifact.json"), "utf8"));
    assert.deepEqual(meta, {
        id: "A-1",
        title: "IOCP vs io_uring",
        description: "a comparison",
        createdAt: "2026-09-26T10:00:00Z",
        updatedAt: "2026-09-26T10:00:00Z",
    });
    assert.equal(store.publish({ title: "second", html: "<p>2</p>" }).artifact.id, "A-2");
    assert.equal(findArtifacts(path.join(root, ".artifact", "A-1")), root);
});

test("republishing by id replaces the page and keeps createdAt", () => {
    const store = new Artifacts(workspace());
    store.publish({ title: "v1", html: "<p>1</p>" }, new Date("2026-01-01T00:00:00Z"));
    const b = store.publish({ id: "A-1", title: "v2", html: "<p>two</p>" }, new Date("2026-02-01T00:00:00Z"));
    assert.equal(b.artifact.createdAt, "2026-01-01T00:00:00Z");
    assert.equal(b.artifact.updatedAt, "2026-02-01T00:00:00Z");
    const got = store.get("A-1");
    assert.equal(got.html, "<p>two</p>");
    assert.equal(got.artifact.title, "v2");
});

test("the list is newest update first, and skips what is not an artifact", () => {
    const root = workspace();
    const store = new Artifacts(root);
    store.publish({ title: "old", html: "x" }, new Date("2026-01-01T00:00:00Z"));
    store.publish({ title: "new", html: "x" }, new Date("2026-03-01T00:00:00Z"));
    store.publish({ title: "mid", html: "x" }, new Date("2026-02-01T00:00:00Z"));
    fs.mkdirSync(path.join(root, ".artifact", "notes"));
    fs.mkdirSync(path.join(root, ".artifact", "A-99")); // half-published: no metadata
    assert.deepEqual(
        store.list().map((a) => a.title),
        ["new", "mid", "old"],
    );
    assert.deepEqual(new Artifacts(workspace()).list(), []);
});

test("a deleted artifact's id is never handed out again", () => {
    const root = workspace();
    const store = new Artifacts(root);
    store.publish({ title: "a", html: "x" });
    store.publish({ title: "b", html: "x" });
    fs.rmSync(path.join(root, ".artifact", "A-2"), { recursive: true });
    assert.equal(store.publish({ title: "c", html: "x" }).artifact.id, "A-3");
});

test("bad input is refused before anything is written", () => {
    const root = workspace();
    const store = new Artifacts(root);
    assert.throws(() => store.publish({ title: "", html: "x" }), refused("invalid"));
    assert.throws(() => store.publish({ title: "two\nlines", html: "x" }), refused("invalid"));
    assert.throws(() => store.publish({ title: "t".repeat(201), html: "x" }), refused("invalid"));
    assert.throws(() => store.publish({ title: "t", html: "   " }), refused("invalid"));
    assert.throws(() => store.publish({ title: "t", html: "x", description: "d".repeat(2001) }), refused("invalid"));
    assert.equal(fs.existsSync(path.join(root, ".artifact")), false, "a refusal created the directory");
    assert.throws(() => store.publish({ id: "A-7", title: "t", html: "x" }), refused("not_found"));
    assert.throws(() => store.get("A-7"), refused("not_found"));
});

test("an id is A-<n> and nothing that could name a path", () => {
    for (const bad of ["../A-1", "A-1/../../etc", "A-01", "A-0", "a-1", "A-", "A-1 ", ".artifact", ""]) {
        assert.equal(isArtifactId(bad), false, bad);
        assert.throws(() => new Artifacts(workspace()).get(bad), refused("bad_id"), bad);
    }
    assert.equal(isArtifactId("A-12"), true);
});

test("a first artifact goes to the repository root, not a subdirectory", () => {
    const root = workspace();
    fs.mkdirSync(path.join(root, ".git"));
    fs.mkdirSync(path.join(root, "src", "deep"), { recursive: true });
    assert.equal(defaultRoot(path.join(root, "src", "deep")), root);
    const outside = workspace();
    assert.equal(defaultRoot(outside), outside);
});
