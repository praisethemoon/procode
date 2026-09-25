/* index-ui.md §3.2: "the document text, rendered for reading: Markdown as
 * Markdown, HTML as sanitized prose, source code with syntax highlighting,
 * everything else as plain text."
 *
 * FOUR RENDERINGS, ONE DISPATCH, AND THE DISPATCH IS A PURE FUNCTION
 * (`view/mime.ts`). A `switch` in here would be a decision no test could reach,
 * and the failure it would hide is quiet: a source file rendered as prose looks
 * like a source file with bad formatting.
 *
 * NO MATCH HIGHLIGHTING. §3.2 is explicit, and there is nowhere in this file
 * for it to be added — none of the three renderers takes a query, and the
 * search result's only influence on this component is which heading it scrolls
 * to.
 *
 * NOTHING BECOMES MARKUP. Markdown goes through react-markdown with no raw-HTML
 * plugin; HTML goes through `view/html.ts`, which answers a node tree that
 * React makes elements of; code goes through `view/code.ts`, which answers
 * tokens. There is no `dangerouslySetInnerHTML` in this package and
 * `guards.test.ts` refuses it by name.
 */

import { Token, lines, tokenize } from "../src/view/code";
import { HtmlNode, parseHtml } from "../src/view/html";
import { headingId, headingText } from "../src/view/headings";
import { languageFor, renderingFor } from "../src/view/mime";
import { Markdown } from "./Markdown";
import { link } from "./rpc";

/* ---------------------------------------------------------------- code */

/* A line of code, as spans.
 *
 * THE GUTTER IS A SEPARATE COLUMN AND NOT A PREFIX ON THE TEXT. A number
 * printed into the line would be selected and copied with it, which turns
 * "copy this function" into "copy this function with the line numbers stuck to
 * it" — and this surface exists so somebody can take a passage away with
 * them. */
function CodeLine(props: { tokens: readonly Token[]; number: number }): JSX.Element {
    return (
        <div className="kb-code-line">
            <span className="kb-code-gutter" aria-hidden="true">
                {props.number}
            </span>
            <span className="kb-code-text">
                {props.tokens.map((t, i) => (
                    <span key={i} className={`kb-tok kb-tok-${t.kind}`}>
                        {t.text}
                    </span>
                ))}
                {/* A blank line has no token and would collapse to nothing,
                  * taking the row's height with it and putting the gutter's
                  * numbers out of step with the file. */}
                {props.tokens.length === 0 ? " " : null}
            </span>
        </div>
    );
}

function Code(props: { source: string; mime: string }): JSX.Element {
    const rows = lines(tokenize(props.source, languageFor(props.mime)));
    return (
        <pre className="kb-code">
            <code>
                {rows.map((tokens, i) => (
                    <CodeLine key={i} tokens={tokens} number={i + 1} />
                ))}
            </code>
        </pre>
    );
}

/* ---------------------------------------------------------------- html */

/* One node. Recursion over a tree whose depth `view/html.ts` has already
 * capped, so this cannot be the thing that overflows a stack. */
function HtmlChildren(props: { nodes: readonly HtmlNode[] }): JSX.Element {
    return (
        <>
            {props.nodes.map((node, i) => (
                <HtmlPart key={i} node={node} />
            ))}
        </>
    );
}

function HtmlPart(props: { node: HtmlNode }): JSX.Element {
    const node = props.node;
    if (node.kind === "text") {
        return <>{node.text}</>;
    }
    /* An anchor is a button here for the reason it is one in the markdown
     * renderer: nothing in this webview navigates, and a URL out of somebody
     * else's page goes to the host to be checked and confirmed. */
    if (node.tag === "a") {
        const href = node.href ?? "";
        return (
            <button
                type="button"
                className="kb-link"
                title={href}
                onClick={(e) => {
                    e.stopPropagation();
                    if (href !== "") {
                        link(href);
                    }
                }}
            >
                <HtmlChildren nodes={node.children} />
            </button>
        );
    }
    if (node.tag === "br" || node.tag === "hr") {
        const Void = node.tag as "br";
        return <Void />;
    }
    if (/^h[1-6]$/.test(node.tag)) {
        const Tag = node.tag as "h1";
        /* §3.2's scroll target, from the same function the markdown renderer
         * uses — the chunk's `heading` has to make the same id whichever
         * rendering the document happens to take. */
        const id = headingId(headingText(nodeText(node)));
        return (
            <Tag id={id === "" ? undefined : id}>
                <HtmlChildren nodes={node.children} />
            </Tag>
        );
    }
    const Tag = node.tag as "p";
    return (
        <Tag>
            <HtmlChildren nodes={node.children} />
        </Tag>
    );
}

/* The text of a node tree, for a heading's id. `headingText` walks React
 * elements; this walks the parser's own nodes, which is the shape available
 * before anything has been rendered. */
function nodeText(node: HtmlNode): string {
    if (node.kind === "text") {
        return node.text;
    }
    return node.children.map((c) => nodeText(c)).join("");
}

function Html(props: { source: string }): JSX.Element {
    return (
        <div className="kb-prose">
            <HtmlChildren nodes={parseHtml(props.source)} />
        </div>
    );
}

/* --------------------------------------------------------------- plain */

function Plain(props: { source: string }): JSX.Element {
    return <pre className="kb-plain">{props.source}</pre>;
}

/* ------------------------------------------------------------ the body */

export function Body(props: { text: string; mime: string }): JSX.Element {
    switch (renderingFor(props.mime)) {
        case "markdown":
            return <Markdown source={props.text} />;
        case "html":
            return <Html source={props.text} />;
        case "code":
            return <Code source={props.text} mime={props.mime} />;
        default:
            return <Plain source={props.text} />;
    }
}
