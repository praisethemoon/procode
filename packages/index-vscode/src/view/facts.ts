/* index-ui.md §3.1's provenance block and §4's collection rows, as values.
 *
 * §3.1 IS A LIST OF FACTS AND NOT A PARAGRAPH, and it is built here rather
 * than inside the component for the reason every other pure module in this
 * directory exists: the webview bundle and the test run compile the same
 * sources, so "the indexed date is shown only when it differs from the fetched
 * one" is a property a test can state instead of a rule somebody has to
 * re-read the JSX to check.
 *
 * `meta` IS SHOWN AS IT IS AND NOTHING IS ASSERTED ABOUT IT. §1.2 of
 * index-api.md makes it free-form per document — a paper's authors and year, a
 * page's section path, a file's symbol list — and §3.1 says the header "shows
 * what is there and asserts nothing about what should be". So there is no
 * schema here, no known-key list, and no key that is treated specially. What
 * there IS is a rendering for each JSON shape a value can have, because an
 * array printed as `[object Object]` is a fact that has been hidden rather
 * than shown.
 *
 * NO vscode IMPORT.
 */

import { KbCollection, KbDocument, fetchedAtKey } from "kb-js/pure";

/* ------------------------------------------------------------- formatting */

/* Bytes, as a person reads them.
 *
 * BINARY UNITS WITH THEIR REAL NAMES. A store's disk use is what `du` reports
 * and `du` counts in units of 1024; calling those "KB" is the ambiguity that
 * makes two numbers on one screen disagree by 2.4%. */
const UNITS = ["B", "KiB", "MiB", "GiB", "TiB"];

export function formatBytes(bytes: number): string {
    if (!Number.isFinite(bytes) || bytes < 0) {
        return "—";
    }
    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit < UNITS.length - 1) {
        value /= 1024;
        unit += 1;
    }
    /* Whole bytes have no decimal; everything else gets one, which is enough
     * to tell 1.2 MiB from 1.9 MiB and not enough to look like a measurement. */
    return unit === 0 ? `${value} ${UNITS[0]}` : `${value.toFixed(1)} ${UNITS[unit]}`;
}

/* A date, as a fixed ISO day rather than a locale's.
 *
 * NOT `toLocaleDateString`. A provenance block's whole job is to be compared —
 * against another document, against a changelog, against what somebody
 * remembers — and `01/02/26` is a different day in two countries. The instant
 * is kept in the title attribute for anybody who needs the time. */
export function formatDate(iso: string): string {
    const at = Date.parse(iso);
    if (Number.isNaN(at)) {
        /* An unparseable timestamp is SHOWN rather than blanked: it is what the
         * store holds, and a reader who can see it can go and find out why. */
        return iso.length > 0 ? iso : "—";
    }
    return new Date(at).toISOString().slice(0, 10);
}

/* ------------------------------------------------------- §3.1's facts */

export interface Fact {
    readonly label: string;
    readonly value: string;
    /* The full value for a hover, when the shown one is shortened. */
    readonly title?: string;
}

/* The facts strip, in §3.1's own order, minus the two that are not text: the
 * locator is a link and the stale badge is a badge, and both are drawn by the
 * component because both do something when clicked.
 *
 * THE INDEXED DATE APPEARS ONLY WHEN IT DIFFERS FROM THE FETCHED ONE. §3.1
 * says "fetched date, and indexed date when they differ", and the reason is
 * that they are the same instant for almost every document — a row repeating
 * one date twice teaches a reader to stop reading the strip. They are compared
 * as DAYS rather than as instants, because `kb add` writes them a second apart
 * and a strip that showed both for every document would meet the letter of the
 * rule and defeat it. */
export function documentFacts(document: KbDocument): Fact[] {
    const facts: Fact[] = [
        { label: "Collection", value: document.collection === "" ? "—" : document.collection },
        { label: "Fetched", value: formatDate(document.fetchedAt), title: document.fetchedAt },
    ];
    if (
        document.indexedAt.length > 0 &&
        formatDate(document.indexedAt) !== formatDate(document.fetchedAt)
    ) {
        facts.push({
            label: "Indexed",
            value: formatDate(document.indexedAt),
            title: document.indexedAt,
        });
    }
    facts.push({ label: "Size", value: formatBytes(document.bytes) });
    facts.push({ label: "Type", value: document.mime === "" ? "—" : document.mime });
    facts.push({
        label: "Chunks",
        value: String(document.chunkCount),
        /* §1.1: chunk ids are reserved as a contiguous range at ingest and
         * survive a rebuild, so naming the range says something a count does
         * not — which citation a passage in this document would carry. */
        title:
            document.chunkCount > 0
                ? `C-${document.chunkBase}..C-${document.chunkBase + document.chunkCount - 1}`
                : undefined,
    });
    /* §1.4: which tier this came from is part of its provenance. */
    facts.push({ label: "Store", value: document.store });
    return facts;
}

/* ------------------------------------------------- §3.1's free-form meta */

export interface MetaEntry {
    readonly key: string;
    readonly value: string;
}

/* One value, rendered as the kind of thing it is.
 *
 * A LIST BECOMES A COMMA LIST AND AN OBJECT BECOMES ITS JSON. Both are shapes
 * `meta` genuinely carries — a paper's authors are an array, a page's section
 * path is an array, a file's symbol table is an object — and `String(value)`
 * turns the second into `[object Object]`, which is a fact the header has
 * hidden while appearing to show it. */
export function formatMetaValue(value: unknown): string {
    if (value === null || value === undefined) {
        return "—";
    }
    if (typeof value === "string") {
        return value;
    }
    if (typeof value === "number" || typeof value === "boolean") {
        return String(value);
    }
    if (Array.isArray(value)) {
        return value.map((v) => formatMetaValue(v)).join(", ");
    }
    return JSON.stringify(value);
}

/* `meta`, as a key/value list.
 *
 * SORTED BY KEY, because the order an object's keys arrive in is the order
 * whoever filed the document happened to write them, and two documents from
 * one source would otherwise list the same facts in different places. */
export function metaEntries(meta: Readonly<Record<string, unknown>>): MetaEntry[] {
    return Object.keys(meta)
        .sort()
        .map((key) => ({ key, value: formatMetaValue(meta[key]) }));
}

/* ------------------------------------------------------ §4's collections */

export interface CollectionRow {
    readonly name: string;
    readonly store: string;
    readonly documents: number;
    readonly bytes: number;
    /* §4 asks for the oldest fetch date. `kb collections` does not carry one —
     * see `oldestFetch` — so it is derived, and it is empty when the documents
     * it would have been derived from were not asked for. */
    readonly oldestFetch: string;
}

/* The oldest fetch date per collection, derived from the documents.
 *
 * §4 ASKS FOR IT AND `GET /collections` DOES NOT CARRY IT. index-api.md §7
 * gives the route "names with counts" and §1.3 makes a collection implicit —
 * it has no record of its own, so there is nothing for a fetch date to be
 * stored on. The list of documents is already being read for §2's browse
 * state, so folding it here costs nothing and asserts nothing the store does
 * not already say.
 *
 * KEYED BY NAME AND TIER TOGETHER. §1.4: "`win32-iocp` can exist in both
 * tiers", and they are different scopes with different contents — merging them
 * would report one date for two collections. */
export function collectionKey(name: string, store: string): string {
    return `${store} ${name}`;
}

export function oldestFetch(documents: readonly KbDocument[]): Map<string, string> {
    const out = new Map<string, string>();
    for (const d of documents) {
        if (d.fetchedAt.length === 0) {
            continue;
        }
        const key = collectionKey(d.collection, d.store);
        const held = out.get(key);
        if (held === undefined || fetchedAtKey(d.fetchedAt) < fetchedAtKey(held)) {
            out.set(key, d.fetchedAt);
        }
    }
    return out;
}

export function collectionRows(
    collections: readonly KbCollection[],
    documents: readonly KbDocument[],
): CollectionRow[] {
    const oldest = oldestFetch(documents);
    return [...collections]
        /* By name, then by tier. §4 offers no order to choose and the list is
         * a reference rather than a feed, so alphabetical is what lets a reader
         * find the one they are looking for. */
        .sort((a, b) => a.name.localeCompare(b.name) || a.store.localeCompare(b.store))
        .map((c) => ({
            name: c.name,
            store: c.store,
            documents: c.documents,
            bytes: c.bytes,
            oldestFetch: oldest.get(collectionKey(c.name, c.store)) ?? "",
        }));
}
