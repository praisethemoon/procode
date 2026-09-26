/* A Kanban board's columns: one per ticket status, in the order work moves
 * through them, each holding its tickets most urgent first. Pure, shared by
 * the tab that draws it and the tests. */

import type { Summary } from "coboard";
import { PRIORITIES, TICKET_STATUSES } from "coboard/model";

export type ViewMode = "list" | "kanban";

export interface Column {
    readonly status: string;
    readonly tickets: readonly Summary[];
}

const rank = (p: string | undefined) => {
    const i = PRIORITIES.indexOf((p ?? "") as (typeof PRIORITIES)[number]);
    return i < 0 ? -1 : i;
};

export function columns(tickets: readonly Summary[]): Column[] {
    return TICKET_STATUSES.map((status) => ({
        status,
        tickets: tickets
            .filter((t) => t.status === status)
            .sort((a, b) => rank(b.priority) - rank(a.priority) || Number(a.id.slice(2)) - Number(b.id.slice(2))),
    }));
}

/* Whether dropping a card on a column changes anything. */
export function moves(ticket: Summary, status: string): boolean {
    return ticket.status !== status && (TICKET_STATUSES as readonly string[]).includes(status);
}
