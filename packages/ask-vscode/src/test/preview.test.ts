import * as assert from "node:assert/strict";
import { test } from "node:test";

import { PREVIEW_CSP, PREVIEW_SANDBOX, previewDocument, themeCss } from "../preview";

test("a preview runs nothing and reaches nowhere", () => {
    assert.equal(PREVIEW_SANDBOX, "", "sandboxed with no permissions at all");
    assert.match(PREVIEW_CSP, /default-src 'none'/);
    assert.doesNotMatch(PREVIEW_CSP, /script-src/);
});

test("the styles come first, in order, and a stray </style> cannot end them", () => {
    const doc = previewDocument("<p>hi</p>", { tokens: ":root{--bk-a:1}</style><script>x</script>", defaults: "p{margin:0}" }, ":root{--vscode-x:red}");
    const tokens = doc.indexOf("--bk-a");
    const theme = doc.indexOf("--vscode-x");
    const defaults = doc.indexOf("p{margin:0}");
    const body = doc.indexOf("<p>hi</p>");
    assert.ok(tokens < theme && theme < defaults && defaults < body);
    assert.match(doc, /<\\\/style><script>/);
    assert.equal(doc.match(/<\/style>/g)!.length, 3);
});

test("the theme takes only --vscode-* variables, and nothing that closes the rule", () => {
    const css = themeCss({ "--vscode-editor-background": "#1e1e1e", "--other": "x", "--vscode-bad": "red;} body{display:none" });
    assert.match(css, /--vscode-editor-background: #1e1e1e;/);
    assert.doesNotMatch(css, /--other/);
    assert.match(css, /--vscode-bad: red bodydisplay:none;/);
    assert.equal(css.match(/[{}]/g)!.length, 2);
});
