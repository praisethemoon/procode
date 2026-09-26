/* The `kb:` scheme (index-ui.md §6), and the only place a reference becomes a
 * URI.
 *
 * ONE STRING DOES BOTH JOBS. `kb:/D-241` is the reference an agent writes in a
 * citation and the URI of the editor it opens, so resolving one is opening the
 * other and nothing else needs to know the scheme exists.
 *
 * THE CANONICAL SPELLING IS WHAT MAKES A TAB UNIQUE. VSCode focuses an existing
 * editor when the resource and the view type both match, and it compares
 * resources as strings — so `d-241` and `D-241` are one document (§1.1 of
 * index-api.md) only because every path goes through `targetUri` first. §6 asks
 * for exactly that: "opening a document that already has a tab focuses it".
 *
 * A CHUNK IS NOT A URI, AND THAT IS DELIBERATE. §3.2 scrolls to the matching
 * chunk's heading when a search result is opened, and the obvious way to carry
 * it — `kb:/D-241?chunk=C-99812` — would make two spellings of one document
 * two different resources and therefore two tabs, which is the one failure §6
 * names by hand. The chunk travels to the tab as a message instead
 * (`editor.ts`), so the URI stays the reference and nothing else.
 *
 * NO vscode IMPORT. This module is the half of the binding that can be tested
 * without an extension host, which is the half worth testing.
 */

export const KB_SCHEME = "kb";

/* §1.1's three prefixes. A chunk id is accepted as a REFERENCE — it is a
 * public identifier and a hit carries one — but it is not a place: §6 gives
 * URIs to a document, a source and the collection list, and a chunk is read
 * inside the document it belongs to. */
const ENTITY = /^([SDC])-([0-9]+)$/i;

export type EntityKind = "source" | "document" | "chunk";

const KIND_OF: Readonly<Record<string, EntityKind>> = {
    S: "source",
    D: "document",
    C: "chunk",
};

/* §6's one place that is not an entity. */
export const PLACES = ["collections", "graph"] as const;
export type Place = (typeof PLACES)[number];

export type Target =
    | { readonly sort: "entity"; readonly kind: EntityKind; readonly id: string }
    | { readonly sort: "place"; readonly place: Place };

/* Accepts every spelling a reference arrives in — `D-241`, `d-241`, `/D-241`,
 * `kb:/D-241`, `kb:///D-241` — and answers the one target they all name, or
 * null.
 *
 * Lenient at the door and strict afterwards: a reference pasted out of a
 * citation, typed into a command or read off a row has to resolve, and the
 * canonical form is what leaves. */
export function parseTarget(raw: string): Target | null {
    let s = raw.trim();
    if (s.length === 0) {
        return null;
    }
    const scheme = `${KB_SCHEME}:`;
    if (s.toLowerCase().startsWith(scheme)) {
        s = s.slice(scheme.length);
    }
    /* `kb:/D-241` and `kb:///D-241` are the same URI: an empty authority is no
     * authority. VSCode's own parser normalises the two together, and so must
     * anything that compares them. */
    s = s.replace(/^\/+/, "");
    if (s.length === 0) {
        return null;
    }
    const lower = s.toLowerCase();
    for (const place of PLACES) {
        if (lower === place) {
            return { sort: "place", place };
        }
    }
    const m = ENTITY.exec(s);
    if (m === null) {
        return null;
    }
    /* `D-007` and `D-7` are not the same id. §1.1's identifiers are monotonic
     * and never padded, so a padded reference names nothing and is refused
     * rather than quietly resolved to its unpadded neighbour — which would be
     * two references for one document, and therefore two tabs.
     *
     * `D-0` IS REFUSED FOR THE SAME REASON AND IS NOT THE SAME CHECK. The
     * store's counters start at one — `kb get D-0` answers `usage`, because
     * `kb_id_num` reads a zero as "not an identifier" — so `D-0` is a
     * reference that names nothing, and a UI that opened a tab for it would
     * open a tab whose whole content is a refusal. */
    if (m[2].startsWith("0")) {
        return null;
    }
    const prefix = m[1].toUpperCase();
    return { sort: "entity", kind: KIND_OF[prefix], id: `${prefix}-${m[2]}` };
}

/* The reference as the UI renders it and as the CLI accepts it (§6): the
 * public identifier and nothing else. */
export function targetRef(t: Target): string {
    return t.sort === "entity" ? t.id : t.place;
}

/* The canonical URI string. One form, always, so two spellings of one
 * reference are one tab. */
export function targetUri(t: Target): string {
    return `${KB_SCHEME}:/${targetRef(t)}`;
}

/* The URI for a reference, or null if it names nothing. */
export function referenceUri(raw: string): string | null {
    const t = parseTarget(raw);
    return t === null ? null : targetUri(t);
}

/* The path a `vscode.Uri` carries, which is the URI without its scheme. Split
 * out because the extension host builds URIs with `Uri.from({scheme, path})`
 * and a leading slash there is mandatory. */
export function targetPath(t: Target): string {
    return `/${targetRef(t)}`;
}

/* Whether a target is something §6 gives a tab to.
 *
 * A CHUNK IS NOT. It resolves as a reference — a hit names one and a citation
 * may carry one — and opening it means opening the document it is in, scrolled
 * to its heading. A tab whose whole content was one chunk would be a passage
 * with its provenance cut off, which is the thing §3.1 exists to prevent. */
export function isPlaceable(t: Target): boolean {
    return t.sort === "place" || t.kind !== "chunk";
}

export const PLACE_TITLES: Readonly<Record<Place, string>> = {
    collections: "Collections",
    graph: "Graph",
};

/* What a tab is called until the store answers. The reference alone — a title
 * read from the store replaces it, so a renamed document renames its tab
 * instead of leaving a stale name sitting there until it is closed. */
export function fallbackTitle(t: Target): string {
    return t.sort === "entity" ? t.id : PLACE_TITLES[t.place];
}
