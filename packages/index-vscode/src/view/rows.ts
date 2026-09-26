/* index-ui.md §2's list, as a pure function of what the store answered.
 *
 * THE LIST HAS TWO SOURCES AND ONE ROW. With no query it is every document in
 * scope, newest first; with a query it is search results. §2 gives both the
 * same anatomy — title, collection, fetched date, a one-line snippet, a stale
 * badge, and which retrieval paths matched — so there is one row type and one
 * renderer, and the difference between the two lists is which fields are
 * populated rather than which component draws them. Two row components would
 * be two places for the badge to stop being drawn.
 *
 * BROWSING IS THE DEFAULT STATE, NOT AN EMPTY PROMPT. §2 is explicit, and it
 * is the property most likely to regress into a "type to search" placeholder
 * the first time somebody simplifies the loading path — so `isSearching` is a
 * function with a test rather than a `q !== ""` inside a render, and
 * `browseRows` is what an empty box produces.
 *
 * ROWS ARE NOT HIGHLIGHTED AND MATCHES ARE NOT MARKED UP. §2 is explicit about
 * that too, and it is the reason `oneLine` returns a STRING rather than a list
 * of spans: there is no shape here for a highlighter to be added to without
 * changing the type, which is a much louder change than adding a `<mark>`.
 *
 * NO vscode IMPORT: the webview bundle and the test run compile these same
 * sources, so the behaviour pinned in a test is the behaviour that ships.
 */

/* `kb-js/pure` AND NOT `kb-js`. This module is compiled into the webview
 * bundle, and the package's main entry reaches `node:child_process` — a
 * browser document cannot spawn anything, and the bundler refuses the import
 * by name. The pure entry is the shapes, the readers and §5's staleness, which
 * is the whole of what a renderer needs. */
import { KbDocument, KbHit, KbStatus, fetchedAtKey, isStale } from "kb-js/pure";

/* What the one-line snippet is actually a line OF.
 *
 * A SEARCH ROW HAS A SNIPPET AND A BROWSED ROW DOES NOT, and this is where
 * the two specifications do not meet. §2 asks every row for "a one-line
 * snippet"; the browse list is `GET /documents` (index-api.md §2), whose
 * `Document` (§1.2) carries no snippet and no text — so a snippet per browsed
 * row would be one `GET /documents/{id}?include=text` per row, which is the
 * "a list must not be able to flood a caller's context" §4 refuses by name.
 *
 * So a browsed row shows its SOURCE LOCATOR in that slot and says so. It is
 * the one line of context a `Document` row actually carries, it is the field
 * §3.1 calls provenance, and it answers the question a reader browsing a store
 * is asking — where did this come from — rather than showing an empty line
 * where a sentence was promised. The kind travels with the text so the surface
 * can style and label them differently, because a locator dressed as a snippet
 * would be a quote nobody wrote. */
export type LineKind = "snippet" | "locator";

export interface Row {
    /* The reference and the key: `D-241`. One string opens the tab (§6). */
    readonly reference: string;
    readonly title: string;
    readonly collection: string;
    readonly fetchedAt: string;
    readonly line: string;
    readonly lineKind: LineKind;
    /* The untruncated text, for the hover (§2). Equal to `line` when nothing
     * was cut, so the surface can decide whether a title attribute is worth
     * setting at all. */
    readonly lineFull: string;
    readonly stale: boolean;
    /* §2: "which retrieval paths matched, when the row came from a search".
     * Empty on a browsed row, which is what makes "when" checkable. */
    readonly matched: readonly string[];
    /* The chunk the hit was in, so opening the row can scroll to its heading
     * (§3.2). Null on a browsed row: there is no matching chunk to scroll to. */
    readonly chunk: string | null;
}

/* How long a clamped line is before the ellipsis.
 *
 * The CSS clamps by WIDTH — one line, ellipsis at the right edge — and this
 * clamps by LENGTH, and both are needed for different reasons. Width alone
 * would hand the DOM a paragraph of four thousand characters per row and clip
 * it; length alone would leave a line longer than a 170-pixel sidebar. The
 * number is generous enough that the ellipsis is almost always the CSS's and
 * small enough that a row cannot carry a document. */
export const LINE_LIMIT = 240;

/* One line of plain text, collapsed and clamped.
 *
 * NEWLINES BECOME SPACES BEFORE ANYTHING ELSE. §2 asks for ONE line, and a
 * snippet spanning a heading boundary genuinely contains them — left alone
 * they either make the row three lines tall or, with the CSS clamp on, hide
 * everything after the first. Control characters go the same way: a snippet is
 * a passage out of somebody else's document and a stray escape sequence in it
 * is a row that renders as garbage.
 *
 * NOTHING IS ESCAPED AND NOTHING IS MARKED UP. React renders this as a text
 * node, so `<script>` in a document is four characters in a row; and §2 says
 * matches are not marked up, so there is no place here for a `<mark>` to be
 * added. */
export function oneLine(text: string, limit: number = LINE_LIMIT): string {
    const flat = text
        /* C0 and DEL, written as escapes so the source has no control bytes in
         * it. `\s` already covers the newline and the tab; what it does not cover
         * is the rest of C0, and a stray escape sequence out of somebody else’s
         * document is a row that renders as garbage. */
        .replace(/[\x00-\x1f\x7f]+/g, " ")
        .replace(/\s+/g, " ")
        .trim();
    if (flat.length <= limit) {
        return flat;
    }
    /* Cut at a word boundary when there is one close to the limit, so the
     * ellipsis follows a word rather than a syllable. */
    const cut = flat.slice(0, limit);
    const space = cut.lastIndexOf(" ");
    return `${(space > limit - 24 ? cut.slice(0, space) : cut).trimEnd()}…`;
}

/* §2's rows for the browse state: every document in scope, newest first. */
export function browseRows(
    documents: readonly KbDocument[],
    now: number,
    staleDays: number,
): Row[] {
    return [...documents]
        .sort((a, b) => fetchedAtKey(b.fetchedAt) - fetchedAtKey(a.fetchedAt))
        .map((d) => {
            const full = d.locator;
            return {
                reference: d.id,
                title: d.title,
                collection: d.collection,
                fetchedAt: d.fetchedAt,
                line: oneLine(full),
                lineKind: "locator" as const,
                lineFull: full,
                stale: isStale(d.fetchedAt, now, staleDays),
                matched: [],
                chunk: null,
            };
        });
}

/* §2's rows for the search state.
 *
 * THE STORE'S ORDER IS KEPT. §4 fuses the two retrieval paths by reciprocal
 * rank; re-sorting here — by date, by title, by anything — would throw away
 * the one thing the search actually computed.
 *
 * THE STORE'S `stale` IS KEPT TOO, for the reason `kb-js`'s `staleOf` exists:
 * the hit carries a verdict computed against the store's own threshold, and a
 * second opinion here would disagree the day that threshold changed. */
export function searchRows(hits: readonly KbHit[], now: number, staleDays: number): Row[] {
    return hits.map((h) => ({
        reference: h.document,
        title: h.title,
        collection: h.collection,
        fetchedAt: h.fetchedAt,
        line: oneLine(h.snippet),
        lineKind: "snippet" as const,
        lineFull: h.snippet,
        stale: typeof h.stale === "boolean" ? h.stale : isStale(h.fetchedAt, now, staleDays),
        matched: h.matched,
        chunk: h.chunk,
    }));
}

/* Whether the box is asking a question. Whitespace is not a query: §2 debounces
 * as the reader types, and a space typed before a word must not turn the list
 * into an empty result set on its way to being one. */
export function isSearching(q: string): boolean {
    return q.trim().length > 0;
}

/* What `GET /documents` is asked for in the browse state.
 *
 * The limit keeps a first paint of a large store cheap; it is not a page, and
 * index-ui §2 wants every document in scope reachable. One row more than is
 * drawn is asked for, so the list knows it was cut and says so, and `all`
 * asks with no limit at all when the reader wants the rest. */
export const BROWSE_LIMIT = 200;
export const SEARCH_K = 50;

export function browseQuery(collection: string, all: boolean = false): Record<string, unknown> {
    const input: Record<string, unknown> = all ? {} : { limit: BROWSE_LIMIT + 1 };
    const scope = collection.trim();
    if (scope.length > 0) {
        input["collection"] = scope;
    }
    return input;
}

/* The rows to draw, and whether the store holds more than that. */
export function browseCut<T>(documents: readonly T[]): { shown: readonly T[]; more: boolean } {
    return documents.length > BROWSE_LIMIT
        ? { shown: documents.slice(0, BROWSE_LIMIT), more: true }
        : { shown: documents, more: false };
}

export function searchQuery(collection: string): Record<string, unknown> {
    const input: Record<string, unknown> = { k: SEARCH_K };
    const scope = collection.trim();
    if (scope.length > 0) {
        input["collection"] = scope;
    }
    return input;
}

/* How many chunks a filing left to embed (index-api §2), off `kb status`.
 *
 * ONLY WHEN THERE IS A MODEL TO EMBED WITH. A keyword-only store has no
 * vector for any chunk, and every one of them counts as missing; offering to
 * "finish" that would be offering a pass that is refused with `model_missing`.
 * A model that is not the one the store recorded is §8's mismatch, which a
 * reindex answers and an embed pass does not, so that is not counted either. */
export function leftToEmbed(status: KbStatus): number {
    const missing = status.vectors?.missing ?? 0;
    if (missing <= 0 || status.model === undefined || status.model.available === null) {
        return 0;
    }
    return status.model.current === false ? 0 : missing;
}

function chunkCount(n: number): string {
    return `${n} chunk${n === 1 ? "" : "s"}`;
}

/* The rail's line for a store with chunks left to embed. */
export function pendingLine(n: number): string {
    return `${chunkCount(n)} ${n === 1 ? "is" : "are"} not embedded yet: searchable by keyword, not yet by meaning.`;
}

/* A search's note when its semantic side could not see every chunk (§4's
 * `unembedded`); null when it saw them all. */
export function unembeddedNote(unembedded: number | undefined): string | null {
    if (unembedded === undefined || unembedded <= 0) {
        return null;
    }
    return `${chunkCount(unembedded)} ${unembedded === 1 ? "is" : "are"} not embedded yet — semantic results may be missing ${unembedded === 1 ? "it" : "them"}.`;
}
