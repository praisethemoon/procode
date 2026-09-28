import * as assert from "node:assert/strict";
import { test } from "node:test";

import type { KbDocument } from "kb-js/pure";

import { FolderNode, buildTree, folderOpen, hasPaths, showsTree } from "../view/tree";

let n = 0;
const doc = (path: string): KbDocument => ({ id: `D-${++n}`, path, title: path }) as unknown as KbDocument;

/* The tree as lines, `name (count)` for a folder and `name` for a file, indented. */
function draw(f: FolderNode, depth = 0): string[] {
    const pad = "  ".repeat(depth);
    return [
        ...f.folders.flatMap((c) => [`${pad}${c.name}/ (${c.count})`, ...draw(c, depth + 1)]),
        ...f.files.map((x) => `${pad}${x.name}`),
    ];
}

test("folders come before files, each in natural order ignoring case", () => {
    const t = buildTree(["src/b.c", "src/A.c", "src/a10.c", "src/a2.c", "src/lib/x.c", "README", "docs/y.md"].map(doc));
    assert.equal(t.root, "");
    assert.deepEqual(draw(t.top), [
        "docs/ (1)",
        "  y.md",
        "src/ (5)",
        "  lib/ (1)",
        "    x.c",
        "  A.c",
        "  a2.c",
        "  a10.c",
        "  b.c",
        "README",
    ]);
});

test("a chain of one-folder folders is one row, and the folder every path shares is the root, not a row", () => {
    const t = buildTree(
        ["kb-cli/tests/fixtures/syntax/ring.go", "kb-cli/tests/fixtures/syntax/strings.s", "kb-cli/src/main.c"].map(doc),
    );
    assert.equal(t.root, "kb-cli/");
    assert.deepEqual(draw(t.top), ["src/ (1)", "  main.c", "tests/fixtures/syntax/ (2)", "  ring.go", "  strings.s"]);
    const chain = t.top.folders[1];
    assert.equal(chain.path, "tests/fixtures/syntax", "a folder's path is from the top, without the root");
    assert.equal(chain.files[0].path, "kb-cli/tests/fixtures/syntax/ring.go", "a file keeps its full path, for the tooltip");

    const deep = buildTree(["a/b/c/one.txt", "a/b/c/two.txt"].map(doc));
    assert.equal(deep.root, "a/b/c/");
    assert.deepEqual(draw(deep.top), ["one.txt", "two.txt"]);
});

test("documents without a path are kept apart, in the order given; with none, there is no tree", () => {
    const web = doc("");
    const paper = doc("");
    const t = buildTree([web, doc("x/a.c"), paper]);
    assert.deepEqual(t.loose, [web, paper]);
    assert.equal(t.top.count, 1);
    assert.equal(hasPaths([web, paper]), false);
    assert.equal(hasPaths([web, doc("a.c")]), true);
    const none = buildTree([web, paper]);
    assert.deepEqual(draw(none.top), []);
    assert.equal(none.root, "");
});

test("backslashes and ./ in a path are read as the folder they name", () => {
    const t = buildTree(["w\\src\\a.c", "./w/src/b.c", "w/doc.md"].map(doc));
    assert.equal(t.root, "w/");
    assert.deepEqual(draw(t.top), ["src/ (2)", "  a.c", "  b.c", "doc.md"]);
});

test("top-level folders start open and deeper ones closed, until the reader chooses", () => {
    const chosen = new Map<string, boolean>([["src", false], ["src/lib", true]]);
    assert.equal(folderOpen(new Map(), "docs", 0), true);
    assert.equal(folderOpen(new Map(), "src/lib", 1), false);
    assert.equal(folderOpen(chosen, "src", 0), false);
    assert.equal(folderOpen(chosen, "src/lib", 1), true);
});

test("a search inside a collection stays flat; otherwise a path on the newest page makes it a tree", () => {
    const filed = [doc("src/a.c"), doc("")];
    assert.equal(showsTree(false, filed), true);
    assert.equal(showsTree(true, filed), false, "search results are a flat list, with their paths");
    assert.equal(showsTree(false, [doc(""), doc("")]), false, "a collection with no paths looks as before");
    assert.equal(showsTree(false, []), false);
});
