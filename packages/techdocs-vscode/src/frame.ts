/* The document a page renders as (specs/techdocs.md §5), built from its
 * page. Pure: no vscode, no file system — the host reads the stylesheets and
 * the webview fills in the theme.
 *
 * WHAT GOES IN, AND IN WHAT ORDER. Ahead of anything the page brings:
 *   1. a content security policy with no network — a published page is the
 *      agent's, and nothing it contains may reach anywhere;
 *   2. baukasten's VS Code stylesheet: the --bk-* tokens, bound to --vscode-*;
 *   3. the --vscode-* values themselves, which a frame does not inherit from
 *      the webview around it (THEME_SLOT, filled in by the webview);
 *   4. the default stylesheet for plain elements;
 *   5. a bridge that swaps (3) when the theme changes.
 * The page comes after all of it, so it can override any of it.
 *
 * THE FRAME IS SANDBOXED WITHOUT SAME-ORIGIN (FRAME_SANDBOX). Its scripts run,
 * but it is an opaque origin: no acquireVsCodeApi, no reach into the webview
 * that holds it, no storage shared with anything.
 */

export const FRAME_CSP = [
    "default-src 'none'",
    "img-src data:",
    "media-src data:",
    "font-src data:",
    "style-src 'unsafe-inline'",
    "script-src 'unsafe-inline'",
].join("; ");

export const FRAME_SANDBOX = "allow-scripts";

/* Replaced by the webview with `:root{--vscode-…: …}` before the frame loads,
 * so the first paint is already in the theme. */
export const THEME_SLOT = "/*bk-theme*/";

/* The message the bridge understands. */
export interface ThemeMessage {
    readonly type: "bk-theme";
    readonly css: string;
    readonly kind: string;
}

/* Runs inside the frame. Only a message of exactly this shape is acted on, and
 * all it can do is replace one stylesheet's text and one attribute. */
const BRIDGE = `(function () {
  addEventListener("message", function (e) {
    var m = e.data;
    if (!m || m.type !== "bk-theme" || typeof m.css !== "string") return;
    var s = document.getElementById("bk-theme");
    if (s) s.textContent = m.css;
    document.documentElement.setAttribute("data-vscode-theme-kind", String(m.kind || ""));
  });
})();`;

export interface FrameParts {
    /* baukasten-vscode.css */
    readonly tokens: string;
    /* page.css */
    readonly defaults: string;
}

/* A closing tag inside a stylesheet or script would end the element early and
 * let the rest be read as markup; the stylesheets are ours, but a stray
 * `</style>` in a comment should not break the page either. */
function inert(text: string, tag: "style" | "script"): string {
    return text.replace(new RegExp(`</${tag}`, "gi"), `<\\/${tag}`);
}

export function frameHead(parts: FrameParts): string {
    return [
        `<meta charset="utf-8">`,
        `<meta http-equiv="Content-Security-Policy" content="${FRAME_CSP}">`,
        `<meta name="viewport" content="width=device-width, initial-scale=1">`,
        `<style id="bk-tokens">${inert(parts.tokens, "style")}</style>`,
        `<style id="bk-theme">${THEME_SLOT}</style>`,
        `<style id="bk-defaults">${inert(parts.defaults, "style")}</style>`,
        `<script>${inert(BRIDGE, "script")}</script>`,
    ].join("\n");
}

/* The page with the head injected. A whole document keeps its own structure
 * and gets the head's contents first inside its <head> (one is made when it
 * has <html> but no <head>); a fragment is wrapped. */
export function frameDocument(page: string, parts: FrameParts): string {
    const head = frameHead(parts);
    const headOpen = /<head(\s[^>]*)?>/i.exec(page);
    if (headOpen) {
        const at = headOpen.index + headOpen[0].length;
        return page.slice(0, at) + "\n" + head + "\n" + page.slice(at);
    }
    const htmlOpen = /<html(\s[^>]*)?>/i.exec(page);
    if (htmlOpen) {
        const at = htmlOpen.index + htmlOpen[0].length;
        return page.slice(0, at) + "\n<head>\n" + head + "\n</head>\n" + page.slice(at);
    }
    return `<!DOCTYPE html>\n<html>\n<head>\n${head}\n</head>\n<body>\n${page}\n</body>\n</html>\n`;
}

/* `:root{…}` from the webview's own variables: only --vscode-* names, and
 * values stripped of anything that could close the rule. */
export function themeCss(vars: Readonly<Record<string, string>>): string {
    const decls: string[] = [];
    for (const [name, value] of Object.entries(vars)) {
        if (!/^--vscode-[A-Za-z0-9_-]+$/.test(name)) continue;
        const v = value.replace(/[{}<>;]/g, "").trim();
        if (v) decls.push(`${name}: ${v};`);
    }
    return `:root {\n${decls.join("\n")}\n}`;
}
