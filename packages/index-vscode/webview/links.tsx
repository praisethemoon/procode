/* Links inside a filed document (index-ui.md §3.2): followed inside the store,
 * shown and not followed outside it. `src/view/links.ts` decides; this file
 * draws the decision and carries it out.
 *
 * THE DOCUMENT PROVIDES THE RESOLVER. Only the document tab knows the page's
 * own address and has the store's address table, so the Markdown and HTML
 * renderers ask it through a context; a renderer with no document around it
 * follows nothing.
 */

import { createContext, useContext } from "react";

import { LinkFollow, headingMatches } from "../src/view/links";
import { open } from "./rpc";

export interface DocLinks {
    follow(href: string): LinkFollow;
}

export const DocLinksContext = createContext<DocLinks | null>(null);

/* The heading a fragment names on the page being read, scrolled to; the top
 * of the page for "". False when no heading matches. */
export function scrollToFragment(fragment: string): boolean {
    if (fragment === "") {
        document.querySelector(".kb-scroll")?.scrollTo({ top: 0 });
        return true;
    }
    for (const h of Array.from(document.querySelectorAll<HTMLElement>('[id^="kb-h-"]'))) {
        if (headingMatches(h.textContent ?? "", fragment)) {
            h.scrollIntoView({ block: "start" });
            return true;
        }
    }
    return false;
}

/* A fragment travels to another tab where a chunk id would: `C-n` names a
 * chunk, `#…` a place on the page, and the two cannot be mistaken. */
export function go(f: LinkFollow): void {
    if (f.kind === "here") {
        scrollToFragment(f.fragment);
    } else if (f.kind === "document") {
        open(f.id, f.fragment === "" ? null : `#${f.fragment}`);
    }
}

/* A link out of a filed page. A button when the store has where it points;
 * otherwise its text, with a tooltip saying where it pointed and that it is
 * not followed. Nothing here navigates the webview, and nothing leaves it. */
export function DocLink(props: { to: string; children?: React.ReactNode }): JSX.Element {
    const links = useContext(DocLinksContext);
    const f: LinkFollow = links === null ? { kind: "outside", url: props.to } : links.follow(props.to);
    if (f.kind === "outside") {
        return (
            <span className="kb-link-outside" title={`${f.url || "(no address)"}: outside the store, not followed`}>
                {props.children}
            </span>
        );
    }
    return (
        <button
            type="button"
            className="kb-link"
            title={props.to}
            onClick={(e) => {
                e.stopPropagation();
                go(f);
            }}
        >
            {props.children}
        </button>
    );
}
