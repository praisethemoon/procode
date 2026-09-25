/* The board's three kinds of item and the words each field may take.
 *
 * Epics contain milestones; milestones contain tickets. A ticket always
 * belongs to an epic and may sit in one of that epic's milestones, or in none
 * — a "dangling" ticket, shown under its epic directly.
 *
 * Identifiers are E-<n>, M-<n>, T-<n>: public, per-kind, monotonic, never
 * reused. They are what a person types, what an agent passes, and what a lap
 * session carries in its metadata (`--meta ticket=T-12`).
 */

export type Kind = "epic" | "milestone" | "ticket";

export const PREFIX: Readonly<Record<Kind, string>> = { epic: "E", milestone: "M", ticket: "T" };

export const EPIC_STATUSES = ["open", "done"] as const;
export const MILESTONE_STATUSES = ["open", "done"] as const;
export const TICKET_STATUSES = ["todo", "doing", "blocked", "review", "done"] as const;
export const SIZES = ["xs", "s", "m", "l", "xl"] as const;
export const PRIORITIES = ["low", "medium", "high", "urgent"] as const;

export function statusesOf(kind: Kind): readonly string[] {
    return kind === "ticket" ? TICKET_STATUSES : kind === "milestone" ? MILESTONE_STATUSES : EPIC_STATUSES;
}

export interface Comment {
    readonly author: string;
    readonly body: string; // markdown
    readonly at: string; // ISO-8601 UTC
}

interface Base {
    readonly id: string;
    readonly title: string;
    readonly description: string; // markdown
    readonly status: string;
    readonly created: string;
    readonly updated: string;
}

export interface Epic extends Base {
    readonly kind: "epic";
}

export interface Milestone extends Base {
    readonly kind: "milestone";
    readonly epic: string;
}

export interface Ticket extends Base {
    readonly kind: "ticket";
    readonly epic: string;
    readonly milestone: string | null;
    readonly size: string | null;
    readonly priority: string;
    readonly assignee: string | null;
    readonly labels: readonly string[];
    readonly comments: readonly Comment[];
}

export type Item = Epic | Milestone | Ticket;

export function kindOf(id: string): Kind | null {
    const m = /^([EMT])-[1-9][0-9]*$/.exec(id);
    if (!m) {
        return null;
    }
    return m[1] === "E" ? "epic" : m[1] === "M" ? "milestone" : "ticket";
}

export function idNumber(id: string): number {
    return kindOf(id) ? Number(id.slice(2)) : 0;
}

/* Tickets per status, for an epic's or a milestone's progress. */
export type Counts = Record<string, number>;

export function countByStatus(tickets: readonly Ticket[]): Counts {
    const c: Counts = {};
    for (const s of TICKET_STATUSES) {
        c[s] = 0;
    }
    for (const t of tickets) {
        c[t.status] = (c[t.status] ?? 0) + 1;
    }
    return c;
}
