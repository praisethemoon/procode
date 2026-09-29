/* The search page (`kb:/search`): what it asks the store, and the collections
 * it asks. Pure, so it is tested without a webview.
 */

import { searchQuery } from "./rows";

/* The input of one `search`: the query, and the collections chosen, as the
 * comma list `kb search --collection` takes. None chosen is All: no
 * `collection` at all, rather than an empty one, which would name the
 * collection whose name is "". */
export function searchPageQuery(q: string, chosen: readonly string[]): Record<string, unknown> {
    return { ...searchQuery(chosen.join(",")), q: q.trim() };
}

/* A collection picked or unpicked; the order stays the order picked in. */
export function toggleCollection(chosen: readonly string[], name: string): string[] {
    return chosen.includes(name) ? chosen.filter((c) => c !== name) : [...chosen, name];
}
