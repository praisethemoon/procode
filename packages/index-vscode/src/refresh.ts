/* Refreshing one document from where it came from (index-ui §3.1's "stale
 * badge with a refresh action").
 *
 * The document's locator says where that is. A URL is fetched again and a
 * file is read again; either way the text is filed back through `kb add`
 * under the same locator, title, collection, mime and meta, and kb's hash
 * comparison decides whether anything changed (§2: "the same text at the same
 * locator re-indexes nothing and updates fetchedAt"). Content an agent handed
 * over directly has an `inline:` locator: there is nowhere to go back to, and
 * the reader is told so instead of being shown a refresh that did nothing.
 *
 * Pure: no vscode, no kb. The host does the reading and the filing.
 */

export type RefreshPlan =
    | { readonly kind: "url"; readonly url: string; readonly host: string }
    | { readonly kind: "file"; readonly path: string }
    | { readonly kind: "none"; readonly why: string };

export function refreshPlan(locator: string): RefreshPlan {
    if (/^https?:\/\//i.test(locator)) {
        try {
            return { kind: "url", url: locator, host: new URL(locator).host };
        } catch {
            return { kind: "none", why: `its locator ${locator} is not a URL that can be fetched.` };
        }
    }
    if (locator.startsWith("inline:") || locator.length === 0) {
        return {
            kind: "none",
            why: "it was filed from content handed over directly, so there is no page or file to read again. Re-file it with kb add to update it.",
        };
    }
    if (locator.startsWith("/") || /^[A-Za-z]:[\\/]/.test(locator)) {
        return { kind: "file", path: locator };
    }
    return { kind: "none", why: `its locator ${locator} is neither a URL nor a file path.` };
}

/* What the reader is told afterwards. `document` is the id kb filed under,
 * which is the same document unless the store decided otherwise. */
export type RefreshOutcome =
    | { readonly outcome: "unchanged"; readonly document: string; readonly fetchedAt: string }
    | { readonly outcome: "updated"; readonly document: string; readonly fetchedAt: string }
    | { readonly outcome: "cannot"; readonly why: string }
    | { readonly outcome: "declined" };

export function outcomeMessage(id: string, r: RefreshOutcome): string {
    switch (r.outcome) {
        case "unchanged":
            return `${id} is unchanged at its source; its fetch date is now ${r.fetchedAt}.`;
        case "updated":
            return r.document === id
                ? `${id} changed at its source and was re-indexed.`
                : `${id}'s source was re-filed as ${r.document}.`;
        case "cannot":
            return `${id} cannot be refreshed: ${r.why}`;
        case "declined":
            return `${id} was not refreshed.`;
    }
}
