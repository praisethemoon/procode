/* What a row of the Board sidebar's tree shows, apart from how it is drawn:
 * the status icon, the id, the title, the archived marker and the tooltip.
 * The icon carries the status, so the row does not spell it out; the tooltip
 * still does. */

import type { Summary } from "coboard";

const STATUS_ICON: Record<string, string> = {
    todo: "circle-large-outline",
    doing: "play-circle",
    blocked: "error",
    review: "eye",
    done: "pass-filled",
    open: "circle-large-outline",
};

export interface RowParts {
    /* A codicon name. */
    readonly icon: string;
    /* The class that colours the icon: the status for a ticket, the kind otherwise. */
    readonly tone: string;
    readonly id: string;
    readonly title: string;
    readonly archived: boolean;
    readonly tooltip: string;
}

export function iconOf(s: Summary): string {
    if (s.kind === "epic") return s.status === "done" ? "pass" : "project";
    if (s.kind === "milestone") return s.status === "done" ? "pass" : "milestone";
    return STATUS_ICON[s.status] ?? "circle-large-outline";
}

export function rowParts(s: Summary): RowParts {
    return {
        icon: iconOf(s),
        tone: s.kind === "ticket" ? s.status : s.kind,
        id: s.id,
        title: s.title,
        archived: s.archived === true,
        tooltip: `${s.id} — ${s.title}\n${s.kind}, ${s.status}${s.archived ? ", archived" : ""}`,
    };
}

/* The epics a row offers to archive at once: every milestone under the epic
 * is done, and so is every ticket under it outside a milestone, and there is
 * at least one of them. Archived items are out of the way and not counted,
 * and an archived epic is not offered. */
export function quickArchivable(all: readonly Summary[]): Set<string> {
    const out = new Set<string>();
    for (const e of all) {
        if (e.kind !== "epic" || e.archived) continue;
        const children = all.filter(
            (c) => !c.archived && c.epic === e.id && (c.kind === "milestone" || (c.kind === "ticket" && !c.milestone)),
        );
        if (children.length > 0 && children.every((c) => c.status === "done")) out.add(e.id);
    }
    return out;
}
