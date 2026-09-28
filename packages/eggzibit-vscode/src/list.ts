/* The list's order and its one-word dates. Pure. */

import type { Page } from "eggzibit";

/* Newest update first, ties by id newest first: the order the store gives,
 * restated here so the view does not depend on it. */
export function sortForList(all: readonly Page[]): Page[] {
    const n = (id: string) => Number(id.slice(2)) || 0;
    return [...all].sort((x, y) =>
        x.updatedAt !== y.updatedAt ? (x.updatedAt < y.updatedAt ? 1 : -1) : n(y.id) - n(x.id),
    );
}

/* "just now", "5 min ago", "3 h ago", "yesterday", "4 days ago", then the
 * date. An unreadable timestamp is shown as it is. */
export function describeWhen(iso: string, now: number): string {
    const t = Date.parse(iso);
    if (Number.isNaN(t)) return iso;
    const s = Math.max(0, Math.round((now - t) / 1000));
    if (s < 60) return "just now";
    if (s < 3600) return `${Math.floor(s / 60)} min ago`;
    if (s < 86400) return `${Math.floor(s / 3600)} h ago`;
    const d = Math.floor(s / 86400);
    if (d === 1) return "yesterday";
    if (d < 7) return `${d} days ago`;
    return iso.slice(0, 10);
}
