/* index-ui.md §3.2: "opening a search result scrolls to the matching chunk's
 * heading and stops there — the chunk already carries its heading and span
 * (index-api.md §1.2), so the navigation is free, while marking up the text is
 * a feature with a maintenance cost and no reader asking for it."
 *
 * THE NAVIGATION IS BY HEADING AND NOT BY SPAN, WHICH IS THE CHEAPER OF THE
 * TWO AND THE ONE §3.2 ASKS FOR. A chunk carries both: `heading` is a string
 * and `span` is a byte range into the stored blob. The span is exact and is
 * useless to a renderer — after markdown has become elements, or HTML has
 * become a node tree, there is no byte offset left to point at, and the only
 * surface where the span would land correctly is the one that shows the raw
 * text. The heading survives every rendering, because every rendering keeps
 * headings.
 *
 * SO THE TWO SIDES HAVE TO AGREE ON ONE FUNCTION. The renderer puts an id on
 * each heading it draws and the tab looks up the id made from the chunk's
 * `heading` string. Two slug functions that differed by a hyphen would produce
 * a scroll that silently did nothing — which looks exactly like a document
 * that happened to open at the top.
 *
 * NO vscode IMPORT.
 */

/* A heading's anchor.
 *
 * DELIBERATELY NOT GitHub's SLUG ALGORITHM. That one strips punctuation and
 * lowercases, which collides `io_uring_prep_recv` with `io-uring-prep-recv`
 * and, in this corpus, with a heading one section away — index-api.md §4 makes
 * the whole point that near-identical identifiers are what this store is full
 * of. What is wanted here is an id that is stable and that two different
 * headings do not share, so the transformation is the smallest one that
 * produces a legal id: trim, collapse whitespace, and encode. Case is kept.
 *
 * IT IS PREFIXED, because an id built from a document's own text lands in the
 * same namespace as everything else in the page, and a heading called `root`
 * would collide with the mount point. */
export function headingId(heading: string): string {
    const text = heading.replace(/\s+/g, " ").trim();
    if (text.length === 0) {
        return "";
    }
    /* `encodeURIComponent` leaves letters, digits and a handful of safe
     * punctuation alone, so a heading reads recognisably in the id — which
     * matters when somebody is looking at the DOM to work out why a scroll did
     * not land. */
    return `kb-h-${encodeURIComponent(text)}`;
}

/* The text of a heading as React hands it back: a string, a number, or an
 * array of those and of elements. Flattened to the text, because that is what
 * the chunk's `heading` is.
 *
 * AN ELEMENT'S CHILDREN ARE FOLLOWED. A heading containing `code` or `em` — a
 * markdown heading with a symbol name in backticks, which this corpus is full
 * of — arrives as an element whose text is one level down, and a flattener that
 * stopped at the element would produce an empty id for exactly the headings
 * that matter most here. */
export function headingText(node: unknown): string {
    if (typeof node === "string") {
        return node;
    }
    if (typeof node === "number") {
        return String(node);
    }
    if (Array.isArray(node)) {
        return node.map((n) => headingText(n)).join("");
    }
    if (typeof node === "object" && node !== null) {
        const props = (node as { props?: { children?: unknown } }).props;
        if (props !== undefined) {
            return headingText(props.children);
        }
    }
    return "";
}
