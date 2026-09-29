/* The search page (`kb:/search`): what it asks the store, the collections it
 * asks, and how it shows a hit: the query marked, its section, its score.
 * Pure, so it is tested without a webview.
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

export interface Marked {
    readonly text: string;
    readonly hit: boolean;
}

function escapeRegExp(s: string): string {
    return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

/* A snippet cut where the query's words appear, each piece saying whether it
 * is one of them, for the search page to mark (§5). Case is ignored; words of
 * one letter are not marked, since they would mark half the text. */
export function markTerms(text: string, q: string): Marked[] {
    if (text === "") {
        return [];
    }
    const words = Array.from(new Set(q.toLowerCase().split(/\s+/).filter((w) => w.length > 1)));
    if (words.length === 0) {
        return [{ text, hit: false }];
    }
    words.sort((a, b) => b.length - a.length);
    const re = new RegExp(`(${words.map(escapeRegExp).join("|")})`, "gi");
    return text
        .split(re)
        .filter((part) => part !== "")
        .map((part) => ({ text: part, hit: words.includes(part.toLowerCase()) }));
}

/* The section a hit was found in: its chunk's heading on one line, or null
 * when the chunk has none (text before a document's first heading). */
export function sectionOf(heading: string | null): string | null {
    if (heading === null) {
        return null;
    }
    const one = heading.replace(/\s+/g, " ").trim();
    if (one === "") {
        return null;
    }
    return one.length > 160 ? `${one.slice(0, 159)}…` : one;
}

/* A hit's score as the page shows it: the one it was ranked by (the
 * reranker's when it ran, else the fused one), and every score kb gave, for
 * the hover. */
export function scoreLabel(scores: {
    readonly bm25?: number;
    readonly vector?: number;
    readonly fused: number;
    readonly rerank?: number;
}): { text: string; detail: string } {
    const n = (v: number): string => (Math.abs(v) >= 10 ? v.toFixed(1) : v.toFixed(2));
    const parts: string[] = [];
    if (scores.bm25 !== undefined) parts.push(`keyword ${n(scores.bm25)}`);
    if (scores.vector !== undefined) parts.push(`semantic ${n(scores.vector)}`);
    parts.push(`fused ${n(scores.fused)}`);
    if (scores.rerank !== undefined) parts.push(`rerank ${n(scores.rerank)}`);
    const text = scores.rerank !== undefined ? `rerank ${n(scores.rerank)}` : `score ${n(scores.fused)}`;
    return { text, detail: parts.join(" · ") };
}
