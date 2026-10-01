/* The document an option's HTML preview renders as (specs/ask.md §5). Pure:
 * the host reads the stylesheets, the webview fills in the theme.
 *
 * A PREVIEW IS A PICTURE, NOT A PROGRAM. The frame is sandboxed with nothing
 * allowed (PREVIEW_SANDBOX): no scripts, no forms, an opaque origin. The
 * webview's own policy would stop them anyway — a srcdoc frame inherits its
 * parent's content security policy, and the webview allows only its own
 * script — so saying it here too means the two never disagree.
 *
 * Styling is techdocs' (packages/techdocs-vscode/src/frame.ts): baukasten's
 * --bk-* tokens bound to --vscode-*, the --vscode-* values themselves (a frame
 * does not inherit them), and page.css for plain elements, all ahead of the
 * preview so it can override any of it. With no bridge script, a theme change
 * re-renders the frame.
 */

export const PREVIEW_CSP = ["default-src 'none'", "img-src data:", "font-src data:", "style-src 'unsafe-inline'"].join("; ");

export const PREVIEW_SANDBOX = "";

export interface PreviewParts {
    /** baukasten-vscode.css */
    readonly tokens: string;
    /** techdocs' page.css */
    readonly defaults: string;
}

/* A `</style>` inside a stylesheet would end it early. */
function inert(text: string): string {
    return text.replace(/<\/style/gi, "<\\/style");
}

/** `:root{…}` from the webview's own variables: only --vscode-* names, and
 *  values stripped of anything that could close the rule. */
export function themeCss(vars: Readonly<Record<string, string>>): string {
    const decls: string[] = [];
    for (const [name, value] of Object.entries(vars)) {
        if (!/^--vscode-[A-Za-z0-9_-]+$/.test(name)) continue;
        const v = value.replace(/[{}<>;]/g, "").trim();
        if (v) decls.push(`${name}: ${v};`);
    }
    return `:root {\n${decls.join("\n")}\n}`;
}

export function previewDocument(preview: string, parts: PreviewParts, theme: string): string {
    const head = [
        `<meta charset="utf-8">`,
        `<meta http-equiv="Content-Security-Policy" content="${PREVIEW_CSP}">`,
        `<style>${inert(parts.tokens)}</style>`,
        `<style>${inert(theme)}</style>`,
        `<style>${inert(parts.defaults)}</style>`,
    ].join("\n");
    return `<!DOCTYPE html>\n<html>\n<head>\n${head}\n</head>\n<body>\n${preview}\n</body>\n</html>\n`;
}
