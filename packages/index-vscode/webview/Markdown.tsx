/* index-ui.md §3.2: "Markdown as Markdown".
 *
 * `react-markdown` WITH `remark-gfm` AND NOTHING ELSE, which is what
 * `coboard-vscode` already does and therefore the dependency this package is
 * allowed to have. Tables, nested lists, task lists, autolinks and
 * strikethrough all work; a hand-rolled parser would not do any of them and
 * this corpus is documentation.
 *
 * THERE IS NO `rehypePlugins` HERE AND ITS ABSENCE IS THE FEATURE. Without the
 * plugin that re-parses raw HTML, react-markdown escapes it — so a markdown
 * document containing `<script>` is a paragraph whose characters happen to
 * spell one. A document in this store was fetched from somewhere else and was
 * written by somebody else; "HTML disabled" has to rest on there being no HTML
 * path at all rather than on a sanitiser's allowlist being right for ever.
 * `guards.test.ts` refuses `rehype-raw` by name.
 *
 * NO ANCHOR EVER REACHES THE DOM, which is why the `a` component is overridden.
 * This webview is not sandboxed, and a `javascript:` URL somebody else wrote
 * would be one click from running in it. A `kb:` reference opens its tab;
 * every other link is followed only to another filed document (`DocLink`),
 * and a link that leads outside the store is shown, not followed.
 *
 * EVERY HEADING CARRIES AN ID, which is the whole of §3.2's navigation: a
 * search result scrolls to the matching chunk's heading, and the id is made
 * from the chunk's `heading` string by the same function on both sides.
 */

import ReactMarkdown, { defaultUrlTransform } from "react-markdown";
import remarkGfm from "remark-gfm";

import { KB_SCHEME, parseTarget } from "../src/uri";
import { headingId, headingText } from "../src/view/headings";
import { DocLink } from "./links";
import { open } from "./rpc";

/* One array, defined once, so every surface renders the same dialect. */
const PLUGINS = [remarkGfm];

/* react-markdown BLANKS AN HREF WHOSE SCHEME IT DOES NOT KNOW, and `kb:` is
 * one of them. `defaultUrlTransform` keeps http, https, mailto and irc and
 * returns an empty string for everything else — so `kb:/D-241` in a document
 * would arrive at the renderer as `""` and render as a control that looks
 * clickable and does nothing, with nothing thrown and nothing logged.
 *
 * So `kb:` is added back and EVERYTHING ELSE STILL GOES THROUGH THE DEFAULT.
 * Returning the URL unchanged would have been shorter and would have handed
 * `javascript:` to the link resolver; it would not follow that either, and
 * two refusals is the right number for a URL out of somebody else's page. */
function urlTransform(url: string): string {
    return url.toLowerCase().startsWith(`${KB_SCHEME}:`) ? url : defaultUrlTransform(url);
}

/* A `kb:` reference is a place in this store and opens its tab. Every other
 * link is resolved against the page it is on and followed inside the store
 * only (`DocLink`); nothing here navigates. */
function Link(props: { href?: string; children?: React.ReactNode }): JSX.Element {
    const href = typeof props.href === "string" ? props.href : "";
    const target = href.toLowerCase().startsWith(`${KB_SCHEME}:`) ? parseTarget(href) : null;
    if (target === null) {
        return <DocLink to={href}>{props.children}</DocLink>;
    }
    return (
        <button
            type="button"
            className="kb-link"
            title={href}
            onClick={(e) => {
                e.stopPropagation();
                open(target.sort === "entity" ? target.id : target.place);
            }}
        >
            {props.children}
        </button>
    );
}

function heading(level: 1 | 2 | 3 | 4 | 5 | 6) {
    return function Heading(props: { children?: React.ReactNode }): JSX.Element {
        const Tag = `h${level}` as "h1";
        const id = headingId(headingText(props.children));
        return <Tag id={id === "" ? undefined : id}>{props.children}</Tag>;
    };
}

const COMPONENTS = {
    a: Link,
    h1: heading(1),
    h2: heading(2),
    h3: heading(3),
    h4: heading(4),
    h5: heading(5),
    h6: heading(6),
};

export function Markdown(props: { source: string }): JSX.Element {
    return (
        <div className="kb-prose">
            <ReactMarkdown
                remarkPlugins={PLUGINS}
                components={COMPONENTS}
                urlTransform={urlTransform}
            >
                {props.source}
            </ReactMarkdown>
        </div>
    );
}
