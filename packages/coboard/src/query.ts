/* Reading the board the way people and agents ask about it: an item with what
 * it contains, a filtered list, and one generic search. Pure functions over
 * `Board.all()`, so the extension, the MCP server and the tests all answer the
 * same question the same way.
 */

import { Counts, Epic, Item, Milestone, Ticket, countByStatus, idNumber, kindOf } from "./model";

/* The one-line form of an item, for lists and for the far end of a link. */
export interface Summary {
    readonly id: string;
    readonly kind: Item["kind"];
    readonly title: string;
    readonly status: string;
    readonly epic?: string;
    readonly milestone?: string | null;
    readonly size?: string | null;
    readonly priority?: string;
    readonly assignee?: string | null;
    readonly labels?: readonly string[];
    readonly updated: string;
}

export function summarize(item: Item): Summary {
    const s = { id: item.id, kind: item.kind, title: item.title, status: item.status, updated: item.updated };
    if (item.kind === "milestone") {
        return { ...s, epic: item.epic };
    }
    if (item.kind === "ticket") {
        return {
            ...s,
            epic: item.epic,
            milestone: item.milestone,
            size: item.size,
            priority: item.priority,
            assignee: item.assignee,
            labels: item.labels,
        };
    }
    return s;
}

export interface EpicView {
    readonly kind: "epic";
    readonly epic: Epic;
    readonly milestones: readonly (Summary & { readonly counts: Counts })[];
    /* Tickets in this epic that are in no milestone. */
    readonly tickets: readonly Summary[];
    readonly counts: Counts;
}

export interface MilestoneView {
    readonly kind: "milestone";
    readonly milestone: Milestone;
    readonly epic: Summary | null;
    readonly tickets: readonly Summary[];
    readonly counts: Counts;
}

export interface TicketView {
    readonly kind: "ticket";
    readonly ticket: Ticket;
    readonly epic: Summary | null;
    readonly milestone: Summary | null;
}

export type View = EpicView | MilestoneView | TicketView;

/* One item with what it contains and what contains it. */
export function view(items: readonly Item[], id: string): View | null {
    const key = id.trim().toUpperCase();
    const item = items.find((i) => i.id === key);
    if (!item) {
        return null;
    }
    const byId = (x: string | null | undefined) => {
        const found = x ? items.find((i) => i.id === x) : undefined;
        return found ? summarize(found) : null;
    };
    const tickets = items.filter((i): i is Ticket => i.kind === "ticket");
    if (item.kind === "epic") {
        const mine = tickets.filter((t) => t.epic === item.id);
        return {
            kind: "epic",
            epic: item,
            milestones: items
                .filter((i): i is Milestone => i.kind === "milestone" && i.epic === item.id)
                .map((m) => ({ ...summarize(m), counts: countByStatus(mine.filter((t) => t.milestone === m.id)) })),
            tickets: mine.filter((t) => t.milestone === null).map(summarize),
            counts: countByStatus(mine),
        };
    }
    if (item.kind === "milestone") {
        const mine = tickets.filter((t) => t.milestone === item.id);
        return { kind: "milestone", milestone: item, epic: byId(item.epic), tickets: mine.map(summarize), counts: countByStatus(mine) };
    }
    return { kind: "ticket", ticket: item, epic: byId(item.epic), milestone: byId(item.milestone) };
}

export interface Filter {
    kind?: Item["kind"] | null;
    status?: string | null;
    epic?: string | null;
    /* A milestone id, or "none" for tickets in no milestone. */
    milestone?: string | null;
    assignee?: string | null;
    label?: string | null;
    limit?: number | null;
}

export interface Hit extends Summary {
    readonly score: number;
    /* Where the query matched: "id", "title", "description", "labels",
     * "assignee" or "comments". */
    readonly matched: readonly string[];
    /* A short piece of the text around the first match in the body, when the
     * match was not in the title. */
    readonly snippet?: string;
}

function keep(item: Item, f: Filter): boolean {
    const up = (s: string | null | undefined) => (s ?? "").trim().toUpperCase();
    if (f.kind && item.kind !== f.kind) return false;
    if (f.status && item.status !== f.status.trim().toLowerCase()) return false;
    if (f.epic) {
        const epicOf = item.kind === "epic" ? item.id : item.epic;
        if (epicOf !== up(f.epic)) return false;
    }
    if (f.milestone) {
        if (item.kind !== "ticket") return false;
        if (f.milestone.trim().toLowerCase() === "none" ? item.milestone !== null : item.milestone !== up(f.milestone)) return false;
    }
    if (f.assignee) {
        if (item.kind !== "ticket" || (item.assignee ?? "").toLowerCase() !== f.assignee.trim().toLowerCase()) return false;
    }
    if (f.label) {
        if (item.kind !== "ticket" || !item.labels.includes(f.label.trim().toLowerCase())) return false;
    }
    return true;
}

function snippetOf(text: string, term: string): string {
    const at = text.toLowerCase().indexOf(term);
    const start = Math.max(0, at - 60);
    const end = Math.min(text.length, at + term.length + 100);
    return (start > 0 ? "…" : "") + text.slice(start, end).replace(/\s+/g, " ").trim() + (end < text.length ? "…" : "");
}

/* The generic search. Every word of the query must appear somewhere in the
 * item — its id, title, description, labels, assignee or comments — and hits
 * are ranked by where: an exact id first, then title matches, then the rest.
 * An empty query lists everything that passes the filters, in id order. */
export function search(items: readonly Item[], query: string, f: Filter = {}): Hit[] {
    const terms = query.toLowerCase().split(/\s+/).filter((t) => t.length > 0);
    const exactId = kindOf(query.trim().toUpperCase()) ? query.trim().toUpperCase() : null;
    const hits: Hit[] = [];
    for (const item of items) {
        if (!keep(item, f)) continue;
        const fields: [string, string, number][] = [
            ["id", item.id.toLowerCase(), 8],
            ["title", item.title.toLowerCase(), 4],
            ["description", item.description.toLowerCase(), 1],
        ];
        if (item.kind === "ticket") {
            fields.push(["labels", item.labels.join(" "), 2]);
            fields.push(["assignee", (item.assignee ?? "").toLowerCase(), 2]);
            fields.push(["comments", item.comments.map((c) => c.body).join("\n").toLowerCase(), 1]);
        }
        let score = item.id === exactId ? 100 : 0;
        const matched = new Set<string>();
        let every = true;
        for (const term of terms) {
            let found = false;
            for (const [name, text, weight] of fields) {
                if (text.includes(term)) {
                    found = true;
                    matched.add(name);
                    score += weight;
                }
            }
            every &&= found || item.id === exactId;
        }
        if (!every) continue;
        let snippet: string | undefined;
        if (terms.length > 0 && !matched.has("title") && !matched.has("id")) {
            const body = item.kind === "ticket" && matched.has("comments") && !matched.has("description")
                ? item.comments.map((c) => c.body).join("\n")
                : item.description;
            snippet = snippetOf(body, terms[0]);
        }
        hits.push({ ...summarize(item), score, matched: [...matched], ...(snippet ? { snippet } : {}) });
    }
    if (terms.length > 0) {
        hits.sort((a, b) => b.score - a.score || idNumber(a.id) - idNumber(b.id));
    }
    return f.limit && f.limit > 0 ? hits.slice(0, f.limit) : hits;
}
