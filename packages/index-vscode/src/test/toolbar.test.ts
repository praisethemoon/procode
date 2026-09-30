/* A document tab's toolbar, the parts that are logic: the zoom steps, and the
 * name a document is shown and saved under. */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import { documentName, saveName } from "../view/savename";
import { ZOOM_DEFAULT, ZOOM_STEPS, zoomIn, zoomOut } from "../view/zoom";

test("zoom steps up and down through fixed levels, stops at both ends, and 100% is one of them", () => {
    assert.ok((ZOOM_STEPS as readonly number[]).includes(ZOOM_DEFAULT));
    assert.equal(zoomIn(100), 110);
    assert.equal(zoomOut(100), 90);
    assert.equal(zoomOut(zoomIn(100)), 100, "in then out comes back");
    assert.equal(zoomIn(400), 400, "the largest step stays");
    assert.equal(zoomOut(20), 20, "the smallest step stays");
    // A level between steps lands on the neighbouring steps.
    assert.equal(zoomIn(105), 110);
    assert.equal(zoomOut(105), 100);
    // Walking up from the bottom visits every step once.
    const seen = [20];
    while (seen[seen.length - 1] < 400) seen.push(zoomIn(seen[seen.length - 1]));
    assert.deepEqual(seen, [...ZOOM_STEPS]);
});

const doc = (over: Partial<{ title: string; path: string; locator: string; mime: string }>) => ({
    title: "",
    path: "",
    locator: "",
    mime: "text/plain",
    ...over,
});

test("a document from a file is shown and saved under that file's name", () => {
    const inFolder = doc({ title: "Spill slots", path: "src/jit/arm64/regalloc.c", locator: "file:///work/vm", mime: "text/x-c" });
    assert.equal(documentName(inFolder), "regalloc.c");
    assert.equal(saveName(inFolder), "regalloc.c");
    const oneFile = doc({ title: "Notes", locator: "file:///home/me/notes/iocp.md", mime: "text/markdown" });
    assert.equal(documentName(oneFile), "iocp.md");
    assert.equal(saveName(oneFile), "iocp.md");
});

test("anything else is named after its title, with an extension from its type", () => {
    const page = doc({ title: "io_uring(7) — Linux manual page", locator: "https://man7.org/linux/man-pages/man7/io_uring.7.html", mime: "text/html" });
    assert.equal(documentName(page), "io_uring(7) — Linux manual page");
    assert.equal(saveName(page), "io_uring(7) — Linux manual page.html");
    const handed = doc({ title: "Design: the scheduler", locator: "inline:abc", mime: "text/markdown" });
    assert.equal(saveName(handed), "Design the scheduler.md", "a colon is not kept in a file name");
    assert.equal(saveName(doc({ title: "a/b\\c", mime: "text/plain" })), "a b c.txt");
    assert.equal(saveName(doc({ title: "readme.md", mime: "text/markdown" })), "readme.md", "no second extension");
    assert.equal(saveName(doc({ title: "", mime: "application/json" })), "document.json");
});

test("a PDF saves as text, since the store holds its extracted text", () => {
    const pdf = doc({ title: "Paper", locator: "file:///papers/regalloc.pdf", mime: "application/pdf" });
    assert.equal(documentName(pdf), "regalloc.pdf");
    assert.equal(saveName(pdf), "regalloc.txt");
});
