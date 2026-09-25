import * as assert from "node:assert/strict";
import { test } from "node:test";

import { headingId, lineAt, lineId, revealId, shownHeading } from "../view/headings";

test("a markdown heading as written becomes the heading as shown", () => {
    assert.equal(shownHeading("The `AcceptEx` call", "markdown"), "The AcceptEx call");
    assert.equal(shownHeading("``io_uring`` and **why** it *matters*", "markdown"), "io_uring and why it matters");
    assert.equal(shownHeading("__strong__ and _em_ and ~~gone~~", "markdown"), "strong and em and gone");
    assert.equal(shownHeading("See [the docs](https://example.com) and ![a logo](l.png)", "markdown"), "See the docs and a logo");
    assert.equal(shownHeading("Tom &amp; Jerry &lt;3 &#x41;&#66;", "markdown"), "Tom & Jerry <3 AB");
    assert.equal(shownHeading("A \\*literal\\* star", "markdown"), "A *literal* star");
});

test("identifiers keep their underscores and stars", () => {
    assert.equal(shownHeading("io_uring_prep_recv", "markdown"), "io_uring_prep_recv");
    assert.equal(shownHeading("IORING_SETUP_SQPOLL flags", "markdown"), "IORING_SETUP_SQPOLL flags");
    assert.equal(shownHeading("a * b * c", "markdown"), "a * b * c");
    assert.equal(shownHeading("char *p", "markdown"), "char *p");
});

test("an HTML heading only needs its entities decoded", () => {
    assert.equal(shownHeading("Ports &amp; handles", "html"), "Ports & handles");
    assert.equal(shownHeading("`not markdown`", "html"), "`not markdown`");
});

test("the chunk's shown heading and the rendered heading make one id", () => {
    // What the renderer sees for `# The \`AcceptEx\` call` is the text without
    // backticks; both sides must land on the same id.
    assert.equal(headingId(shownHeading("The `AcceptEx` call", "markdown")), headingId("The AcceptEx call"));
    assert.equal(headingId(shownHeading("Ports &amp; handles", "html")), headingId("Ports & handles"));
});

test("a search hit lands on its heading or on its first line, by how the document is drawn", () => {
    const code = "int a;\n\nint accept_loop(void) {\n}\n";
    const at = (heading: string | null, start: number) => ({ heading, span: { start } });
    assert.equal(revealId(at("The `AcceptEx` call", 0), "markdown", ""), headingId("The AcceptEx call"));
    assert.equal(revealId(at("Ports &amp; handles", 0), "html", ""), headingId("Ports & handles"));
    assert.equal(revealId(at(null, 0), "markdown", ""), "", "a chunk before the first heading stays at the top");
    assert.equal(revealId(at("int accept_loop(void) {", code.indexOf("int accept")), "code", code), "kb-line-3");
    assert.equal(revealId(at(null, 0), "text", "one\ntwo\n"), "kb-line-1");
});

test("a span's byte offset is found on its line, multi-byte text included", () => {
    const text = "first\nsecond\nthird\n";
    assert.equal(lineAt(text, 0), 1);
    assert.equal(lineAt(text, 6), 2);
    assert.equal(lineAt(text, 13), 3);
    // "é" is two bytes and one UTF-16 unit; "𝄞" is four bytes and two units.
    const wide = "é𝄞\nnext\n";
    assert.equal(lineAt(wide, 6), 1, "the newline itself is still line 1");
    assert.equal(lineAt(wide, 7), 2);
    assert.equal(lineAt(text, -5), 1);
    assert.equal(lineAt(text, 10_000), 4);
    assert.equal(lineId(3), "kb-line-3");
});
