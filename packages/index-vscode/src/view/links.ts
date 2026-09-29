/* What a link inside a filed document does when clicked (index-ui.md §3.2).
 *
 * A LINK IS FOLLOWED ONLY INSIDE THE STORE. It is resolved against the
 * document's own address (`kb-js`'s `documentAddress`), so `guide.html#setup`
 * and `../api/` mean what they meant on the page they came from. A fragment of
 * this page scrolls; another filed document opens, at the fragment when there
 * is one; anything else is not followed at all. The page was written by
 * somebody else, and the store is what the reader chose to trust.
 *
 * PURE, so it is tested without a webview; the webview imports it.
 */

import { addressKey } from "kb-js/pure";

export type LinkFollow =
    /* a place on the page being read ("" for the page itself) */
    | { readonly kind: "here"; readonly fragment: string }
    /* another filed document, and a place on it */
    | { readonly kind: "document"; readonly id: string; readonly fragment: string }
    /* not in the store: shown, not followed */
    | { readonly kind: "outside"; readonly url: string };

function decodeFragment(hash: string): string {
    const raw = hash.startsWith("#") ? hash.slice(1) : hash;
    try {
        return decodeURIComponent(raw);
    } catch {
        return raw;
    }
}

/* `base` is the document's own address, or null when it has none (content
 * handed in directly), where only absolute links can be looked up. `index`
 * is `kb-js`'s `addressIndex` over the store; `self` is this document's id. */
export function followLink(
    href: string,
    base: string | null,
    index: ReadonlyMap<string, string>,
    self: string,
): LinkFollow {
    const h = href.trim();
    if (h.startsWith("#")) {
        return { kind: "here", fragment: decodeFragment(h) };
    }
    let url: URL;
    try {
        url = base === null ? new URL(h) : new URL(h, base);
    } catch {
        return { kind: "outside", url: h };
    }
    const key = addressKey(url.href);
    const fragment = decodeFragment(url.hash);
    if (key !== null && base !== null && key === addressKey(base)) {
        return { kind: "here", fragment };
    }
    const id = key === null ? undefined : index.get(key);
    if (id !== undefined) {
        return id === self ? { kind: "here", fragment } : { kind: "document", id, fragment };
    }
    return { kind: "outside", url: url.href };
}

/* A heading's text or a fragment as a page's own anchor usually spells it:
 * lower case, runs of spaces and underscores as one '-', punctuation gone. So
 * `#getting-started` finds the heading "Getting Started". */
export function anchorSlug(text: string): string {
    return text
        .normalize("NFKC")
        .toLowerCase()
        .replace(/[^\p{L}\p{N}\s_-]/gu, "")
        .trim()
        .replace(/[\s_]+/g, "-")
        .replace(/-+/g, "-");
}

/* Whether a heading is where a fragment points. */
export function headingMatches(heading: string, fragment: string): boolean {
    const f = anchorSlug(fragment);
    return f !== "" && anchorSlug(heading) === f;
}
