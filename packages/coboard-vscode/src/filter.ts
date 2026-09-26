/* The Board sidebar's filter: which items a query and a set of field filters
 * keep, and which stay visible around them. Pure — the sidebar webview runs
 * it, the tests exercise it.
 *
 * WHAT A MATCH SHOWS. An item that matches is shown with its epic and
 * milestone above it, so a ticket never floats without context; a matching
 * epic or milestone is shown with everything under it, because searching for
 * an epic's name is asking to see the epic.
 */

import type { Summary } from "coboard";
import { PRIORITIES, SIZES, TICKET_STATUSES } from "coboard/model";

export const FIELDS = ["status", "kind", "priority", "size", "assignee", "label"] as const;
export type Field = (typeof FIELDS)[number];

export const FIELD_LABELS: Readonly<Record<Field, string>> = {
    status: "Status",
    kind: "Kind",
    priority: "Priority",
    size: "Size",
    assignee: "Assignee",
    label: "Label",
};

/* The value that selects tickets nobody is assigned to. */
export const UNASSIGNED = "unassigned";

export interface Filter {
    readonly text: string;
    /* For each field, the values any one of which an item must have; empty
     * means the field does not filter. */
    readonly fields: Readonly<Record<Field, readonly string[]>>;
    /* Hide what is finished: done tickets, and done epics and milestones. A
     * standing preference rather than a query, so clearing leaves it set. */
    readonly openOnly: boolean;
}

export const EMPTY: Filter = {
    text: "",
    fields: { status: [], kind: [], priority: [], size: [], assignee: [], label: [] },
    openOnly: false,
};

export function fieldCount(f: Filter): number {
    return FIELDS.reduce((n, k) => n + f.fields[k].length, 0);
}

/* Text or a field is set: what the ✕ clears. */
export function hasQuery(f: Filter): boolean {
    return f.text.trim() !== "" || fieldCount(f) > 0;
}

export function isActive(f: Filter): boolean {
    return hasQuery(f) || f.openOnly;
}

/* The ✕: the text and every field, but not the open-only preference. */
export function clear(f: Filter): Filter {
    return { ...EMPTY, openOnly: f.openOnly };
}

export function toggle(f: Filter, field: Field, value: string): Filter {
    const now = f.fields[field];
    const next = now.includes(value) ? now.filter((v) => v !== value) : [...now, value];
    return { ...f, fields: { ...f.fields, [field]: next } };
}

const ID = /^[emt]-\d+$/i;

function textMatches(s: Summary, text: string): boolean {
    const q = text.trim().toLowerCase();
    if (q === "") return true;
    // An id asks for that item, not every id that starts with it: T-1 is not T-12.
    if (ID.test(q)) return s.id.toLowerCase() === q;
    return s.id.toLowerCase().includes(q) || s.title.toLowerCase().includes(q);
}

function values(s: Summary, field: Field): readonly string[] {
    switch (field) {
        case "status":
            return [s.status];
        case "kind":
            return [s.kind];
        case "priority":
            return s.priority ? [s.priority] : [];
        case "size":
            return s.size ? [s.size] : [];
        case "assignee":
            return s.kind !== "ticket" ? [] : [s.assignee || UNASSIGNED];
        case "label":
            return s.labels ?? [];
    }
}

export function matches(s: Summary, f: Filter): boolean {
    if (f.openOnly && s.status === "done") return false;
    if (!textMatches(s, f.text)) return false;
    for (const field of FIELDS) {
        const want = f.fields[field];
        if (want.length > 0 && !values(s, field).some((v) => want.includes(v))) return false;
    }
    return true;
}

/* The ids to show, and among them the ones that matched (the rest are there
 * for context). With no filter, everything is shown and nothing is marked. */
export function visible(all: readonly Summary[], f: Filter): { shown: Set<string>; matched: Set<string> } {
    if (!isActive(f)) return { shown: new Set(all.map((s) => s.id)), matched: new Set() };
    const byId = new Map(all.map((s) => [s.id, s]));
    const matched = new Set(all.filter((s) => matches(s, f)).map((s) => s.id));
    const shown = new Set(matched);
    for (const id of matched) {
        const s = byId.get(id)!;
        // Up: its milestone and epic.
        if (s.milestone) shown.add(s.milestone);
        if (s.epic) shown.add(s.epic);
        const m = s.milestone ? byId.get(s.milestone) : undefined;
        if (m?.epic) shown.add(m.epic);
    }
    for (const s of all) {
        // Down: everything under a matching epic or milestone, except what
        // open-only hides.
        if (f.openOnly && s.status === "done") continue;
        if ((s.epic && matched.has(s.epic)) || (s.milestone && matched.has(s.milestone))) shown.add(s.id);
        if (s.kind === "ticket" && s.milestone && s.epic && matched.has(s.epic)) shown.add(s.milestone);
    }
    return { shown, matched };
}

/* What each field can be set to: the fixed vocabularies, and the assignees
 * and labels that actually occur on this board. */
export function options(all: readonly Summary[]): Readonly<Record<Field, readonly string[]>> {
    const assignees = new Set<string>();
    const labels = new Set<string>();
    for (const s of all) {
        if (s.kind === "ticket") assignees.add(s.assignee || UNASSIGNED);
        for (const l of s.labels ?? []) labels.add(l);
    }
    const sorted = (xs: Set<string>) => [...xs].sort((a, b) => (a === UNASSIGNED ? 1 : b === UNASSIGNED ? -1 : a.localeCompare(b)));
    return {
        status: [...TICKET_STATUSES, "open"],
        kind: ["epic", "milestone", "ticket"],
        priority: [...PRIORITIES],
        size: [...SIZES],
        assignee: sorted(assignees),
        label: sorted(labels),
    };
}

/* For each field, how many items each of its values would match, given the
 * text and every other field as they are set: the number a chip promises
 * before it is clicked. A field's own selection is left out of its counts,
 * because values within a field are alternatives and choosing one more only
 * ever adds its items. Counts are of matches, not of the context shown
 * around them. */
export function counts(all: readonly Summary[], f: Filter): Readonly<Record<Field, ReadonlyMap<string, number>>> {
    const out = {} as Record<Field, Map<string, number>>;
    for (const field of FIELDS) {
        const others: Filter = { ...f, fields: { ...f.fields, [field]: [] } };
        const n = new Map<string, number>();
        for (const s of all) {
            if (!matches(s, others)) continue;
            for (const v of values(s, field)) n.set(v, (n.get(v) ?? 0) + 1);
        }
        out[field] = n;
    }
    return out;
}
