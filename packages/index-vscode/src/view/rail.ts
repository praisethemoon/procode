/* index-ui.md §2: where the Knowledge sidebar is, and what it asks the store
 * for there. Pure, so the webview and the tests run the same code.
 *
 * TWO PLACES. With no query the sidebar shows the collections; a collection
 * opens its documents, and a back control returns. The search bar is above
 * both: a query searches the whole store from the collections, or the open
 * collection from inside it.
 *
 * A PAGE AT A TIME, WITH "LOAD MORE". A collection's documents come from
 * `kb ls`, newest first, PAGE at a time: one more than a page is asked for,
 * so the list knows there is more, and the next page starts after the last
 * row shown. The collections all come in one `kb collections` (a store has
 * few), and are shown PAGE at a time the same way. */

import { KbCollection, KbDocument } from "kb-js/pure";

export type Place = { readonly kind: "collections" } | { readonly kind: "collection"; readonly name: string };

export const HOME: Place = { kind: "collections" };
export const PAGE = 50;

export function openCollection(name: string): Place {
    return { kind: "collection", name };
}

/* The collection a query searches in: the open one, or the whole store. */
export function searchScope(place: Place): string {
    return place.kind === "collection" ? place.name : "";
}

/* `kb ls` for one page of a collection's documents, newest first. */
export function documentsQuery(collection: string, after: string | null): Record<string, unknown> {
    const input: Record<string, unknown> = { collection, limit: PAGE + 1, reverse: true };
    if (after !== null) input["after"] = after;
    return input;
}

/* A page as answered: the rows to draw, and the cursor for the next page
 * (null when this was the last). */
export function cut<T extends { readonly id: string }>(page: readonly T[]): { shown: readonly T[]; next: string | null } {
    if (page.length <= PAGE) return { shown: page, next: null };
    const shown = page.slice(0, PAGE);
    return { shown, next: shown[shown.length - 1].id };
}

/* The collections to draw after `pages` presses of Load more, by name. */
export function collectionsShown(all: readonly KbCollection[], pages: number): { shown: KbCollection[]; more: boolean } {
    const sorted = [...all].sort((a, b) => a.name.localeCompare(b.name));
    const n = PAGE * Math.max(1, pages);
    return { shown: sorted.slice(0, n), more: sorted.length > n };
}

/* A document's row in a collection: what it is called, its size, its type,
 * when it was fetched, and a description when its metadata has one. */
export interface DocumentRow {
    readonly reference: string;
    readonly title: string;
    readonly bytes: number;
    readonly mime: string;
    readonly fetchedAt: string;
    readonly description: string | null;
}

/* Documents carry no description of their own (index-api §1.2); a filing's
 * metadata often does — a paper's abstract, a page's summary. The first of
 * these keys holding text, flattened to one line. */
const DESCRIBING = ["description", "abstract", "summary"] as const;

export function descriptionOf(meta: Readonly<Record<string, unknown>> | null | undefined): string | null {
    for (const key of DESCRIBING) {
        const v = meta?.[key];
        if (typeof v === "string") {
            const flat = v.replace(/[\x00-\x1f\x7f]+/g, " ").replace(/\s+/g, " ").trim();
            if (flat !== "") return flat;
        }
    }
    return null;
}

export function documentRow(d: KbDocument): DocumentRow {
    return {
        reference: d.id,
        title: d.title === "" ? d.id : d.title,
        bytes: d.bytes,
        mime: d.mime,
        fetchedAt: d.fetchedAt,
        description: descriptionOf(d.meta),
    };
}
