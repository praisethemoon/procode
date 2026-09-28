import * as assert from "node:assert/strict";
import { test } from "node:test";

import { FRAME_CSP, FRAME_SANDBOX, THEME_SLOT, frameDocument, themeCss } from "../frame";
import { describeWhen, sortForList } from "../list";
import { VIEWER_CSP, viewerHtml } from "../viewer";

const parts = { tokens: ":root{--bk-color-foreground:var(--vscode-foreground)}", defaults: "@layer artifact{body{margin:0}}" };

test("a fragment is wrapped, with everything injected ahead of the page", () => {
    const doc = frameDocument("<h1>Findings</h1>", parts);
    assert.match(doc, /^<!DOCTYPE html>/);
    const order = ["Content-Security-Policy", 'id="bk-tokens"', 'id="bk-theme"', 'id="bk-defaults"', "bk-theme\"", "<h1>Findings</h1>"];
    let at = -1;
    for (const marker of order.slice(0, 4)) {
        const i = doc.indexOf(marker);
        assert.ok(i > at, `${marker} is out of order`);
        at = i;
    }
    assert.ok(doc.indexOf("<h1>Findings</h1>") > doc.indexOf("</head>"));
    assert.ok(doc.includes(THEME_SLOT));
});

test("a whole document keeps its structure and gets the head first inside <head>", () => {
    const page = '<!doctype html><html lang="en"><head><style>h1{color:red}</style></head><body><h1>x</h1></body></html>';
    const doc = frameDocument(page, parts);
    assert.ok(doc.startsWith('<!doctype html><html lang="en"><head>\n<meta charset'));
    assert.ok(doc.indexOf('id="bk-defaults"') < doc.indexOf("h1{color:red}"), "the page's own styles must come after the defaults");
    assert.equal((doc.match(/<head>/g) ?? []).length, 1);
    const noHead = frameDocument("<html><body>y</body></html>", parts);
    assert.match(noHead, /^<html>\n<head>\n<meta charset/);
});

test("the frame has no network and no same-origin", () => {
    assert.match(FRAME_CSP, /default-src 'none'/);
    assert.doesNotMatch(FRAME_CSP, /https?:|connect-src|\*/);
    assert.equal(FRAME_SANDBOX, "allow-scripts");
    const html = viewerHtml("t");
    assert.match(html, /sandbox="allow-scripts"/);
    assert.doesNotMatch(html, /allow-same-origin/);
    assert.match(VIEWER_CSP, /default-src 'none'/);
    assert.doesNotMatch(VIEWER_CSP, /https?:|connect-src/);
});

test("a stylesheet cannot close its own element", () => {
    const doc = frameDocument("<p>x</p>", { tokens: "/* </style><script>alert(1)</script> */", defaults: "" });
    assert.doesNotMatch(doc, /<\/style><script>alert/);
});

test("only --vscode-* variables reach the frame, and nothing that closes the rule", () => {
    const css = themeCss({
        "--vscode-foreground": "#cccccc",
        "--vscode-font-family": "-apple-system, 'Segoe UI'",
        "--evil": "red",
        "--vscode-x": "red;} body{display:none",
        color: "blue",
    });
    assert.match(css, /--vscode-foreground: #cccccc;/);
    assert.match(css, /--vscode-font-family: -apple-system, 'Segoe UI';/);
    assert.doesNotMatch(css, /--evil|color: blue|display:none\}|\} body/);
    assert.equal((css.match(/}/g) ?? []).length, 1);
});

test("the viewer escapes the title and carries themeCss as source", () => {
    const html = viewerHtml('<img src=x onerror="1">');
    assert.doesNotMatch(html, /<img src=x/);
    assert.match(html, /var themeCss = function themeCss\(vars\)/);
});

test("the list is newest update first, and dates read as words", () => {
    const a = (id: string, updatedAt: string) => ({ id, title: id, description: "", keywords: [], createdAt: updatedAt, updatedAt, bytes: 1 });
    assert.deepEqual(
        sortForList([a("A-1", "2026-01-01T00:00:00Z"), a("A-3", "2026-03-01T00:00:00Z"), a("A-2", "2026-03-01T00:00:00Z")]).map((x) => x.id),
        ["A-3", "A-2", "A-1"],
    );
    const now = Date.parse("2026-09-26T12:00:00Z");
    assert.equal(describeWhen("2026-09-26T11:59:30Z", now), "just now");
    assert.equal(describeWhen("2026-09-26T11:55:00Z", now), "5 min ago");
    assert.equal(describeWhen("2026-09-26T09:00:00Z", now), "3 h ago");
    assert.equal(describeWhen("2026-09-25T10:00:00Z", now), "yesterday");
    assert.equal(describeWhen("2026-09-22T12:00:00Z", now), "4 days ago");
    assert.equal(describeWhen("2026-08-01T12:00:00Z", now), "2026-08-01");
    assert.equal(describeWhen("not a date", now), "not a date");
});
