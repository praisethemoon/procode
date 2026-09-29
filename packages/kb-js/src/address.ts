/* Where a document lives, as one URL, and the way back from a URL to the
 * document filed from it.
 *
 * A LINK INSIDE A FILED PAGE IS RESOLVED AGAINST THE PAGE'S OWN ADDRESS, and
 * followed only when it lands on another filed document. The address is the
 * source's locator for a page fetched from the web, and the locator joined
 * with the document's path for a file of a filed folder (index-api.md §2.1),
 * as a `file:` URL so that a relative link resolves the same way in both.
 *
 * THE LOOKUP IS BUILT FROM ONE `kb ls`. Every row already carries its
 * source's locator and its own path, so a reader clicking through a page
 * costs no call per click.
 *
 * PURE: no process, no file system, nothing from node. The webview imports
 * this through `kb-js/pure`.
 */

import type { DocumentId, KbDocument } from "./types";

/* A local path as a `file:` URL: '/' separators, a drive letter after a
 * leading '/', each segment percent-encoded. */
function fileUrl(path: string): string {
    let p = path.replace(/\\/g, "/");
    if (/^[A-Za-z]:\//.test(p)) {
        p = `/${p}`;
    }
    return `file://${p.split("/").map((s) => encodeURIComponent(s).replace(/%3A/gi, ":")).join("/")}`;
}

/* The document's address as a URL, or null when its locator is neither a
 * web URL nor a local path or `file:` URL (content handed in directly). */
export function documentAddress(d: Pick<KbDocument, "locator" | "path">): string | null {
    const locator = d.locator.trim();
    const rel = d.path.replace(/\\/g, "/").replace(/^\/+/, "");
    if (/^https?:\/\//i.test(locator)) {
        if (rel === "") {
            return locator;
        }
        try {
            return new URL(rel, locator.endsWith("/") ? locator : `${locator}/`).href;
        } catch {
            return null;
        }
    }
    let base: string;
    if (/^file:/i.test(locator)) {
        base = locator;
    } else if (locator.startsWith("/") || /^[A-Za-z]:[\\/]/.test(locator) || locator.startsWith("\\\\")) {
        base = fileUrl(locator);
    } else {
        return null;
    }
    if (rel === "") {
        return base;
    }
    return `${base.replace(/\/+$/, "")}/${rel.split("/").map((s) => encodeURIComponent(s)).join("/")}`;
}

/* A URL reduced to what decides which page it is: no fragment, no trailing
 * '/', and `index.html` taken as its folder, so the ways one page is written
 * compare equal. The query stays: it can pick a different page. Null for
 * anything but http, https and file. */
export function addressKey(url: string): string | null {
    let u: URL;
    try {
        u = new URL(url);
    } catch {
        return null;
    }
    if (u.protocol !== "http:" && u.protocol !== "https:" && u.protocol !== "file:") {
        return null;
    }
    const path = u.pathname.replace(/\/index\.html?$/i, "/").replace(/\/+$/, "");
    return `${u.protocol}//${u.host}${path}${u.search}`;
}

/* Every document with an address, by that address's key. Where two share
 * one, the first listed keeps it. */
export function addressIndex(documents: readonly KbDocument[]): Map<string, DocumentId> {
    const index = new Map<string, DocumentId>();
    for (const d of documents) {
        const address = documentAddress(d);
        const key = address === null ? null : addressKey(address);
        if (key !== null && !index.has(key)) {
            index.set(key, d.id);
        }
    }
    return index;
}
