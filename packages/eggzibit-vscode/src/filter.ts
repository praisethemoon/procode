/* What the pages view's filter bar keeps. Pure, and free of vscode and the
 * DOM, so the host, the webview and the tests share it.
 *
 * THE TEXT is split on spaces, and every word must appear, in any case, in
 * the page's id, title, description or one of its keywords.
 * THE KEYWORDS chosen under the chevron keep a page carrying any of them,
 * as a Board chip keeps an item with any of its field's values. */

import type { Page as Stored } from "eggzibit";

export interface PageFilter {
    readonly text: string;
    readonly keywords: readonly string[];
}

export const EMPTY: PageFilter = { text: "", keywords: [] };

type Page = Pick<Stored, "id" | "title" | "description" | "keywords">;

export function isActive(f: PageFilter): boolean {
    return f.text.trim() !== "" || f.keywords.length > 0;
}

export function matches(p: Page, f: PageFilter): boolean {
    if (f.keywords.length && !f.keywords.some((k) => p.keywords.includes(k))) return false;
    const hay = [p.id, p.title, p.description, ...p.keywords].join("\n").toLowerCase();
    return f.text
        .toLowerCase()
        .split(/\s+/)
        .filter((w) => w !== "")
        .every((w) => hay.includes(w));
}

/* Every keyword the pages carry, with how many carry it: most used first,
 * then by name. */
export function keywordCounts(pages: readonly Page[]): { keyword: string; count: number }[] {
    const n = new Map<string, number>();
    for (const p of pages) for (const k of p.keywords) n.set(k, (n.get(k) ?? 0) + 1);
    return [...n]
        .map(([keyword, count]) => ({ keyword, count }))
        .sort((a, b) => b.count - a.count || (a.keyword < b.keyword ? -1 : 1));
}

/* A chosen keyword switched on or off. */
export function toggleKeyword(f: PageFilter, keyword: string): PageFilter {
    const on = f.keywords.includes(keyword);
    return { ...f, keywords: on ? f.keywords.filter((k) => k !== keyword) : [...f.keywords, keyword] };
}
