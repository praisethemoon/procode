/* What a Knowledge webview may load, and the stylesheets it loads.
 *
 * SPLIT OUT OF `host.ts` BECAUSE A CSP THAT CANNOT BE TESTED IS A CSP THAT IS
 * WRONG IN PRODUCTION. `host.ts` imports `vscode`, so nothing in it can be
 * loaded by the test run; a policy living there as a string literal is a
 * literal nobody can check against the files it governs. That is not
 * hypothetical — `baukasten-base.css` inlines the codicon font as a
 * `data:font/ttf` URI, it is the only shipped stylesheet with a `url()` of any
 * kind, and `font-src <cspSource>` does not permit `data:`. Every baukasten
 * component that draws a codicon then has nothing to draw with, and the only
 * sign of it is a line in a console nobody has open.
 *
 * ONE POLICY, BECAUSE THERE IS ONE KIND OF DOCUMENT. coboard has two: its own
 * chrome, and the surface that embeds an agent-authored page in a
 * `srcdoc` frame, which inherits its embedder's policy and therefore needs a
 * nonce-free one. This package frames nothing. Every document it builds is its
 * own React app over content it renders as ELEMENTS — never as markup — so
 * there is no second document to inherit anything and no reason to weaken
 * `script-src` below a nonce.
 *
 * NO vscode IMPORT: `cspSource` and the nonce arrive as strings.
 */

/* The stylesheets the document links, in the order it links them: baukasten's
 * components, then the token layer that maps `--bk-*` onto the user's theme,
 * then the codicon face, then this package's own. */
export const KNOWLEDGE_STYLESHEETS: readonly string[] = [
    "baukasten-base.css",
    "baukasten-vscode.css",
    "codicon.css",
    "knowledge.css",
];

/* `data:` IS IN `font-src` AND `img-src` AND IN NOTHING ELSE. A `data:` font or
 * image is bytes that are already in the stylesheet — it cannot fetch, cannot
 * reach a network and cannot be a channel — so permitting it grants no
 * capability beyond reading the file the browser has already read.
 * `script-src` is where it would matter, and there it is a nonce and nothing
 * else.
 *
 * THERE IS NO `frame-src` AND NO `connect-src`, which `default-src 'none'`
 * therefore refuses. Nothing here navigates a frame and nothing here reaches a
 * network: §1.4's stores are on disk and the only thing that reads them is the
 * CLI, on the other side of the extension host. An `img-src` that permitted the
 * network would be the one way a rendered document could beacon, and a document
 * in this store is somebody else's page. */
export function knowledgePolicy(cspSource: string, nonce: string): string {
    return [
        "default-src 'none'",
        `style-src ${cspSource} 'unsafe-inline'`,
        `script-src 'nonce-${nonce}'`,
        `font-src ${cspSource} data:`,
        `img-src ${cspSource} data:`,
    ].join("; ");
}

/* The source list a directive carries, or `default-src`'s when it has none of
 * its own — which is the fallback a browser applies and therefore the one a
 * check of a policy has to apply too. */
export function sourcesOf(policy: string, directive: string): string[] {
    const found = new Map<string, string[]>();
    for (const part of policy.split(";")) {
        const tokens = part.trim().split(/\s+/).filter((t) => t.length > 0);
        if (tokens.length > 0) {
            found.set(tokens[0], tokens.slice(1));
        }
    }
    return found.get(directive) ?? found.get("default-src") ?? [];
}

export interface CssAsset {
    url: string;
    /* The directive a browser checks this URL against. */
    directive: "font-src" | "img-src";
}

/* Every `url()` in a stylesheet, with the directive that governs it.
 *
 * The directive follows from the block the URL is in and not from what the
 * bytes turn out to be: a `url()` inside `@font-face` is fetched as a font
 * whatever it points at, and one anywhere else is fetched as an image. That is
 * the distinction a browser makes, so it is the one this makes.
 *
 * `@font-face` blocks are found by brace matching rather than by a regex over
 * the whole rule, because a minified stylesheet has no newlines to anchor to
 * and a `.*?}` would stop at the first brace inside the block. */
export function cssAssets(css: string): CssAsset[] {
    const fontRanges: { from: number; to: number }[] = [];
    const AT_FONT_FACE = /@font-face\s*\{/g;
    for (let m = AT_FONT_FACE.exec(css); m !== null; m = AT_FONT_FACE.exec(css)) {
        let depth = 1;
        let i = m.index + m[0].length;
        while (i < css.length && depth > 0) {
            if (css[i] === "{") {
                depth += 1;
            } else if (css[i] === "}") {
                depth -= 1;
            }
            i += 1;
        }
        fontRanges.push({ from: m.index, to: i });
    }
    const assets: CssAsset[] = [];
    const URL_FN = /url\(\s*(["']?)([^)"']*)\1\s*\)/g;
    for (let m = URL_FN.exec(css); m !== null; m = URL_FN.exec(css)) {
        const at = m.index;
        const inFontFace = fontRanges.some((r) => at >= r.from && at < r.to);
        assets.push({ url: m[2], directive: inFontFace ? "font-src" : "img-src" });
    }
    return assets;
}

/* Whether a policy permits one asset.
 *
 * A RELATIVE URL RESOLVES AGAINST THE STYLESHEET, which is loaded from the
 * webview's resource origin — so it is permitted exactly when the directive
 * carries that origin. `codicon.css`'s `url("./codicon.ttf")` is this case and
 * is why the check is not simply "no `url()` allowed".
 *
 * Absolute URLs are not resolved against a source expression: nothing this
 * package ships has one, and a checker that quietly approximated CSP's host
 * matching would be a second, wrong answer to what the browser does. One
 * turning up fails, and is then a decision somebody makes. */
export function policyPermits(policy: string, asset: CssAsset, cspSource: string): boolean {
    const sources = sourcesOf(policy, asset.directive);
    if (asset.url.startsWith("data:")) {
        return sources.includes("data:");
    }
    if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(asset.url) || asset.url.startsWith("//")) {
        return false;
    }
    return sources.some((s) => cspSource.split(/\s+/).includes(s));
}
