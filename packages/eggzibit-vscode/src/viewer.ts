/* The webview that holds a page's frame. Pure: the host hands it the
 * frame's document with a message, and it does three things — fills the
 * theme slot from its own --vscode-* variables, loads the frame, and sends the
 * frame new variables when the theme changes.
 *
 * THE WEBVIEW'S POLICY ALLOWS INLINE SCRIPT, AND HAS TO. A srcdoc frame
 * inherits its parent's content security policy, so a nonce-only script-src
 * here would stop every script in every page, the bridge included. What
 * the webview itself runs is only this file's script; the page's markup
 * never becomes part of this document — it travels in a message and is set as
 * the frame's srcdoc.
 *
 * MESSAGES FROM THE FRAME ARE IGNORED. The frame can post to its parent; the
 * handler drops anything whose source is the frame, so a page cannot make the
 * webview load something else.
 */

import { FRAME_SANDBOX, THEME_SLOT, themeCss } from "./frame";

export const VIEWER_CSP = [
    "default-src 'none'",
    "style-src 'unsafe-inline'",
    "script-src 'unsafe-inline'",
    "img-src data:",
    "media-src data:",
    "font-src data:",
    "frame-src 'self' about: data:",
].join("; ");

/* The same themeCss the tests exercise, carried into the webview as source so
 * there is one implementation of what may reach the frame. */
const THEME_CSS_SOURCE = themeCss.toString();

export function viewerHtml(title: string): string {
    const esc = (s: string) => s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c] as string);
    return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="${VIEWER_CSP}">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>${esc(title)}</title>
<style>
html, body { margin: 0; padding: 0; height: 100%; overflow: hidden; background: var(--vscode-editor-background); }
iframe { display: block; border: 0; width: 100%; height: 100%; background: transparent; }
</style>
</head>
<body>
<iframe id="page" title="${esc(title)}" sandbox="${FRAME_SANDBOX}"></iframe>
<script>
(function () {
  var vscode = acquireVsCodeApi();
  var frame = document.getElementById("page");
  var SLOT = ${JSON.stringify(THEME_SLOT)};
  var themeCss = ${THEME_CSS_SOURCE};
  var doc = null;
  function vars() {
    var style = document.documentElement.style, out = {};
    for (var i = 0; i < style.length; i++) {
      var name = style[i];
      if (name.indexOf("--vscode-") === 0) out[name] = style.getPropertyValue(name);
    }
    return out;
  }
  function kind() { return document.body.getAttribute("data-vscode-theme-kind") || ""; }
  function render() {
    if (doc === null) return;
    frame.srcdoc = doc.split(SLOT).join(themeCss(vars()));
  }
  function retheme() {
    if (frame.contentWindow) frame.contentWindow.postMessage({ type: "bk-theme", css: themeCss(vars()), kind: kind() }, "*");
  }
  window.addEventListener("message", function (e) {
    if (e.source === frame.contentWindow) return;
    var m = e.data;
    if (m && m.type === "load" && typeof m.doc === "string") { doc = m.doc; render(); }
  });
  var watch = new MutationObserver(retheme);
  watch.observe(document.documentElement, { attributes: true, attributeFilter: ["style", "class"] });
  watch.observe(document.body, { attributes: true, attributeFilter: ["class", "data-vscode-theme-kind"] });
  vscode.postMessage({ type: "ready" });
})();
</script>
</body>
</html>`;
}
