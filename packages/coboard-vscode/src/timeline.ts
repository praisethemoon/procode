/* A lap session's trajectory as a timeline: what the review page draws under
 * "How it got there". Pure.
 *
 * NODES. Consecutive steps on the same file with the same intent are one
 * node, as they are on a ticket's commit list: the intent drawn once, with a
 * line per step for its behavior.
 *
 * WHERE THE WORK MOVES. A marker goes on the rail when a node is in another
 * area than the one before it: `packages/<name>` or `cli/<name>`, otherwise
 * the top folder. It is where a reviewer's attention shifts.
 *
 * TIME ONLY WHERE IT SAYS SOMETHING. Agents record a session's edits in a
 * burst once the work is done, so their steps share a timestamp and a time
 * axis would be a flat line. A gap is drawn only when two steps are further
 * apart than a few minutes, and a gap also ends a node.
 */

/* What the timeline needs of a step; the review's steps have more. */
export interface TimelineStep {
    readonly id: string;
    readonly ts: string;
    readonly file: string;
    readonly op: string;
    readonly new_start: number;
    readonly new_lines: number;
    readonly intent: string;
}

export type TimelineItem<S extends TimelineStep = TimelineStep> =
    | {
          readonly kind: "node";
          readonly file: string;
          /* "create" when the node starts by creating its file, "delete" when
           * it ends by deleting it, otherwise "edit". */
          readonly op: string;
          readonly intent: string;
          /* Oldest first. */
          readonly steps: readonly S[];
      }
    | { readonly kind: "move"; readonly area: string }
    | { readonly kind: "gap"; readonly ms: number };

export const GAP_MS = 5 * 60 * 1000;

/* The part of the repository a file is in, for the "work moves" marker. */
export function areaOf(file: string): string {
    const parts = file.split("/");
    if (parts.length >= 3 && (parts[0] === "packages" || parts[0] === "cli")) return `${parts[0]}/${parts[1]}`;
    return parts.length >= 2 ? parts[0] : "";
}

function time(ts: string): number | null {
    const n = Date.parse(ts);
    return Number.isNaN(n) ? null : n;
}

export function timeline<S extends TimelineStep>(oldestFirst: readonly S[], gapMs = GAP_MS): TimelineItem<S>[] {
    const out: TimelineItem<S>[] = [];
    let node: { kind: "node"; file: string; op: string; intent: string; steps: S[] } | null = null;
    let area: string | null = null;
    let last: number | null = null;
    for (const s of oldestFirst) {
        const at = time(s.ts);
        const gap = at !== null && last !== null && at - last > gapMs ? at - last : 0;
        if (at !== null) last = at;
        if (gap > 0) {
            out.push({ kind: "gap", ms: gap });
            node = null;
        }
        if (node && node.file === s.file && node.intent === s.intent) {
            node.steps.push(s);
            if (s.op === "delete") node.op = "delete";
            continue;
        }
        const here = areaOf(s.file);
        if (area !== null && here !== area) out.push({ kind: "move", area: here });
        area = here;
        node = { kind: "node", file: s.file, op: s.op === "create" || s.op === "delete" ? s.op : "edit", intent: s.intent, steps: [s] };
        out.push(node);
    }
    return out;
}

/* "12 min later", "2 h later", "3 days later". */
export function gapLabel(ms: number): string {
    const min = Math.round(ms / 60_000);
    if (min < 90) return `${min} min later`;
    const h = Math.round(min / 60);
    if (h < 36) return `${h} h later`;
    return `${Math.round(h / 24)} days later`;
}

/* A step's lines in the file after it: "29", "232–249", or "deleted". */
export function linesOf(s: TimelineStep): string {
    if (s.op === "delete" || s.new_lines === 0) return s.op === "delete" ? "deleted" : `after ${Math.max(0, s.new_start - 1)}`;
    return s.new_lines === 1 ? `${s.new_start}` : `${s.new_start}–${s.new_start + s.new_lines - 1}`;
}
