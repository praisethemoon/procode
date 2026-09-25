/* index-ui.md §3.2's "HTML as sanitized prose", as a tree of nodes rather than
 * as a string of markup.
 *
 * WHY THE OUTPUT IS NODES. Every renderer in this package turns data into
 * ELEMENTS. A sanitiser that answered a cleaned-up HTML STRING would need
 * `dangerouslySetInnerHTML` on the other side, and the whole promise would then
 * rest on an allowlist being right for ever, against every page anybody ever
 * files, with a browser's parser on the far side of it doing something subtly
 * different from this one. Answering a tree means React builds the elements and
 * there is no markup path at all — the same argument `coboard-vscode`'s
 * markdown renderer makes for refusing `rehype-raw`, one document along.
 *
 * WHY NOT `DOMParser`. It is there in a webview, it is a real HTML parser, and
 * it would be the obvious thing — but the tree it produces has to be walked and
 * filtered anyway, the walk is most of this file, and doing it there would put
 * the one piece of security-relevant logic in the package inside a module the
 * test run cannot load. This parser runs under `node --test`, over the pages
 * that broke it, in the same process as everything else it is checked against.
 *
 * WHAT IT REFUSES, AND THE REFUSAL IS BY CONSTRUCTION RATHER THAN BY LIST. A
 * tag that is not in `BLOCK`, `INLINE` or `VOID` produces no element: its
 * CHILDREN are kept and it is not. So an unknown tag degrades to its text, and
 * nothing this file has never heard of can become an element. `RAW` is the
 * short list of elements whose CONTENT is dropped as well — a `<script>` body
 * is not prose, and neither is a stylesheet.
 *
 * ATTRIBUTES DO NOT SURVIVE AT ALL, with one exception: `href` on an anchor,
 * and only `http`, `https` and `mailto`. There is no `style`, no `class`, no
 * `on*`, no `src` — not because each has been considered and refused, but
 * because the parser reads attributes into a map and the builder copies exactly
 * one key out of it.
 *
 * NO vscode IMPORT, NO DOM: a pure function of a string.
 */

export type HtmlNode =
    | { readonly kind: "text"; readonly text: string }
    | {
          readonly kind: "element";
          readonly tag: string;
          /* Only ever set on `a`, and only ever a scheme this file permits. */
          readonly href?: string;
          readonly children: readonly HtmlNode[];
      };

/* The prose elements. A page's structure is headings, paragraphs, lists,
 * quotes, code and tables; everything else in an HTML document is layout, and
 * layout belongs to the surface rendering it rather than to the page. */
const BLOCK = new Set([
    "p", "h1", "h2", "h3", "h4", "h5", "h6",
    "ul", "ol", "li", "dl", "dt", "dd",
    "blockquote", "pre", "figure", "figcaption",
    "table", "thead", "tbody", "tfoot", "tr", "th", "td", "caption",
]);

const INLINE = new Set(["a", "code", "em", "strong", "b", "i", "u", "s", "sup", "sub", "kbd", "abbr", "small"]);

const VOID = new Set(["br", "hr", "img", "meta", "link", "input", "source", "col", "area", "base", "wbr", "embed", "param", "track"]);

/* `br` and `hr` are the two void elements that are prose. The rest are
 * dropped: an `img` with no `src` is an empty box, and `src` is an attribute
 * this file does not copy. */
const VOID_KEPT = new Set(["br", "hr"]);

/* Elements whose CONTENT is not prose and is dropped with them. Everything
 * else that is unknown keeps its children — this is the short list where that
 * would be wrong. */
const RAW = new Set(["script", "style", "template", "noscript", "iframe", "object", "svg", "math", "head", "title"]);

/* A ceiling on nesting. A page with ten thousand unclosed `<div>`s is a page,
 * and a recursive renderer over it is a stack overflow in a webview with no
 * message. The depth is far past anything prose reaches and the overflow
 * degrades to flat text rather than to nothing. */
const MAX_DEPTH = 64;

const NAMED: Readonly<Record<string, string>> = {
    amp: "&",
    lt: "<",
    gt: ">",
    quot: '"',
    apos: "'",
    /* Escaped rather than written literally. A non-breaking space is
     * indistinguishable from a plain one on screen and in most diffs, so a
     * literal here is a character anything that copies this file can silently
     * flatten — which is exactly what happened to it once. */
    nbsp: "\u00a0",
    hellip: "…",
    mdash: "—",
    ndash: "–",
    lsquo: "‘",
    rsquo: "’",
    ldquo: "“",
    rdquo: "”",
    copy: "©",
    reg: "®",
    trade: "™",
    deg: "°",
    times: "×",
    middot: "·",
    bull: "•",
};

/* Entity references, decoded.
 *
 * A REFERENCE THIS FILE DOES NOT KNOW IS LEFT ALONE, AND THAT IS SAFE HERE
 * because the output is a text node: `&thinsp;` renders as those characters
 * rather than as a space, which is a cosmetic loss. Decoding aggressively —
 * guessing at anything between `&` and `;` — is what turns `&lt;script&gt;`
 * back into markup in a pipeline that later treats the text as HTML. Nothing
 * downstream of this does, and the conservative rule costs a thin space. */
export function decodeEntities(text: string): string {
    return text.replace(/&(#[0-9]+|#[xX][0-9a-fA-F]+|[a-zA-Z][a-zA-Z0-9]*);/g, (whole, body: string) => {
        if (body.startsWith("#")) {
            const code = body[1] === "x" || body[1] === "X"
                ? Number.parseInt(body.slice(2), 16)
                : Number.parseInt(body.slice(1), 10);
            /* A code point outside Unicode, or a surrogate half, is not a
             * character — `String.fromCodePoint` throws on the first and
             * produces a lone surrogate for the second. */
            if (!Number.isFinite(code) || code < 0x20 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
                return whole;
            }
            return String.fromCodePoint(code);
        }
        return NAMED[body] ?? NAMED[body.toLowerCase()] ?? whole;
    });
}

/* The schemes an anchor may carry. `javascript:` is the one that matters and
 * it is refused by not being on the list rather than by being on a deny list —
 * `JaVaScRiPt:`, `java script:` and `  javascript:` are all things a deny
 * list has to get right and an allow list does not. */
const SCHEME = /^(https?|mailto):/i;

export function safeHref(raw: string | undefined): string | undefined {
    if (typeof raw !== "string") {
        return undefined;
    }
    /* A browser strips leading whitespace and C0 control characters before it
     * reads the scheme, so this has to as well — or the two disagree about what
     * `\x00javascript:alert(1)` is, and the disagreement is the whole attack. */
    const href = decodeEntities(raw).replace(/^[\x00-\x20]+/u, "").trim();
    if (href.length === 0) {
        return undefined;
    }
    /* A relative URL has no scheme and is permitted: it resolves against
     * nothing in a webview, so it is a link that goes nowhere rather than a
     * link that goes somewhere unexpected, and the host confirms every
     * external open anyway. What must not pass is a scheme that is not one of
     * the three. */
    if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(href)) {
        return SCHEME.test(href) ? href : undefined;
    }
    return href;
}

/* ------------------------------------------------------------ the parser */

interface Tag {
    readonly name: string;
    readonly closing: boolean;
    readonly selfClosing: boolean;
    readonly attrs: Readonly<Record<string, string>>;
    /* Where the input continues after this tag. */
    readonly end: number;
}

/* One tag, starting at `<`. Answers null when what follows is not a tag at all
 * — a bare `<` in prose, which is common and must render as a character. */
function readTag(html: string, at: number): Tag | null {
    const m = /^<(\/?)([a-zA-Z][a-zA-Z0-9:-]*)/.exec(html.slice(at));
    if (m === null) {
        return null;
    }
    const closing = m[1] === "/";
    const name = m[2].toLowerCase();
    let i = at + m[0].length;
    const attrs: Record<string, string> = {};
    while (i < html.length) {
        /* Skip whitespace between attributes. */
        while (i < html.length && /\s/.test(html[i])) {
            i += 1;
        }
        if (i >= html.length) {
            break;
        }
        if (html[i] === ">") {
            return { name, closing, selfClosing: false, attrs, end: i + 1 };
        }
        if (html[i] === "/" && html[i + 1] === ">") {
            return { name, closing, selfClosing: true, attrs, end: i + 2 };
        }
        const nameMatch = /^[^\s=/>]+/.exec(html.slice(i));
        if (nameMatch === null) {
            i += 1;
            continue;
        }
        const key = nameMatch[0].toLowerCase();
        i += nameMatch[0].length;
        while (i < html.length && /\s/.test(html[i])) {
            i += 1;
        }
        if (html[i] !== "=") {
            attrs[key] = "";
            continue;
        }
        i += 1;
        while (i < html.length && /\s/.test(html[i])) {
            i += 1;
        }
        const quote = html[i];
        if (quote === '"' || quote === "'") {
            const close = html.indexOf(quote, i + 1);
            const end = close === -1 ? html.length : close;
            attrs[key] = html.slice(i + 1, end);
            i = end + 1;
        } else {
            const value = /^[^\s>]*/.exec(html.slice(i));
            attrs[key] = value === null ? "" : value[0];
            i += value === null ? 0 : value[0].length;
        }
    }
    /* An unterminated tag runs to the end of the input; treating it as text
     * instead would put a half-written `<a href=` into the prose. */
    return { name, closing, selfClosing: false, attrs, end: html.length };
}

interface Frame {
    tag: string;
    href: string | undefined;
    /* False for a tag that is kept only for its children — an unknown element,
     * a `div`, a `span` with nothing to say. */
    keep: boolean;
    children: HtmlNode[];
}

/* Parse a document into prose.
 *
 * A CLOSING TAG THAT MATCHES NOTHING OPEN IS IGNORED, and an element left open
 * at the end of the input is closed there. Both are what a browser does and
 * both are what real pages contain; the alternative is a parser that throws
 * away the rest of a document over one stray `</p>`. */
export function parseHtml(html: string): HtmlNode[] {
    const root: Frame = { tag: "", href: undefined, keep: false, children: [] };
    const stack: Frame[] = [root];
    const top = (): Frame => stack[stack.length - 1];
    let text = "";

    const flush = (): void => {
        if (text.length === 0) {
            return;
        }
        const decoded = decodeEntities(text);
        if (decoded.length > 0) {
            top().children.push({ kind: "text", text: decoded });
        }
        text = "";
    };

    let i = 0;
    while (i < html.length) {
        const c = html[i];
        if (c !== "<") {
            text += c;
            i += 1;
            continue;
        }
        /* A comment, a doctype or a CDATA block: not prose, and the content is
         * dropped with the wrapper. An unterminated comment swallows the rest
         * of the input, which is what a browser does too. */
        if (html.startsWith("<!--", i)) {
            const close = html.indexOf("-->", i + 4);
            i = close === -1 ? html.length : close + 3;
            continue;
        }
        if (html.startsWith("<!", i) || html.startsWith("<?", i)) {
            const close = html.indexOf(">", i);
            i = close === -1 ? html.length : close + 1;
            continue;
        }
        const tag = readTag(html, i);
        if (tag === null) {
            text += c;
            i += 1;
            continue;
        }
        flush();
        i = tag.end;

        if (RAW.has(tag.name)) {
            if (tag.closing || tag.selfClosing) {
                continue;
            }
            /* Skip to the matching close, taking the content with it. A
             * `<script>` body is not prose and neither is a stylesheet, and
             * keeping the text would put JavaScript into the middle of a
             * paragraph. */
            const close = html.toLowerCase().indexOf(`</${tag.name}`, i);
            i = close === -1 ? html.length : close;
            continue;
        }

        if (VOID.has(tag.name)) {
            if (!tag.closing && VOID_KEPT.has(tag.name)) {
                top().children.push({ kind: "element", tag: tag.name, children: [] });
            }
            continue;
        }

        if (tag.closing) {
            /* Unwind to the matching frame, closing anything left open inside
             * it — `<b><i></b>` closes both, which is what a browser's own
             * recovery amounts to for prose. */
            const found = stack.findIndex((f) => f.tag === tag.name);
            if (found <= 0) {
                continue;
            }
            while (stack.length > found) {
                const frame = stack.pop() as Frame;
                const parent = top();
                if (frame.keep) {
                    parent.children.push(
                        frame.href === undefined
                            ? { kind: "element", tag: frame.tag, children: frame.children }
                            : { kind: "element", tag: frame.tag, href: frame.href, children: frame.children },
                    );
                } else {
                    parent.children.push(...frame.children);
                }
            }
            continue;
        }

        const keep =
            (BLOCK.has(tag.name) || INLINE.has(tag.name)) && stack.length <= MAX_DEPTH;
        const href =
            tag.name === "a" && keep ? safeHref(tag.attrs["href"]) : undefined;
        stack.push({
            tag: tag.name,
            href,
            /* An anchor with no usable href is kept only for its text: a
             * control that looks like a link and goes nowhere is worse than the
             * words on their own. */
            keep: keep && (tag.name !== "a" || href !== undefined),
            children: [],
        });
        if (tag.selfClosing) {
            const frame = stack.pop() as Frame;
            const parent = top();
            if (frame.keep) {
                parent.children.push(
                    frame.href === undefined
                        ? { kind: "element", tag: frame.tag, children: frame.children }
                        : { kind: "element", tag: frame.tag, href: frame.href, children: frame.children },
                );
            }
        }
    }
    flush();
    /* Everything still open is closed here, innermost first. */
    while (stack.length > 1) {
        const frame = stack.pop() as Frame;
        const parent = top();
        if (frame.keep) {
            parent.children.push(
                frame.href === undefined
                    ? { kind: "element", tag: frame.tag, children: frame.children }
                    : { kind: "element", tag: frame.tag, href: frame.href, children: frame.children },
            );
        } else {
            parent.children.push(...frame.children);
        }
    }
    return root.children;
}

/* Every tag that can appear in the output, for the renderer's own map and for
 * the test that checks the two agree. A tag the parser can emit and the
 * renderer cannot draw would be an element silently missing from the page. */
export const RENDERABLE_TAGS: readonly string[] = [...BLOCK, ...INLINE, ...VOID_KEPT].sort();
