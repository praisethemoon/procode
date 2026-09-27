/* The branches a folder started, for Lap History's Branches group. Pure: the
 * host hands in what `lap branch list --json` printed and each branch's log
 * (read from its folder, or from its chunks here), and gets back rows the
 * view can draw. See cli/lap-cli/SPEC.md, Branches → Registry. */

import { EMPTY_FILTER, SessionRow, query } from "./history";
import type { LapLog } from "./model";

export type BranchState = "active" | "merged" | "partly merged" | "missing";

const STATES: readonly string[] = ["active", "merged", "partly merged", "missing"];

export interface BranchRow {
    readonly id: string;
    readonly name: string;
    readonly state: BranchState;
    /* its folder is there and still this branch */
    readonly present: boolean;
    readonly path: string;
    /* commits since its base and since the last merge; null when its history
     * is nowhere to be read */
    readonly sinceBase: number | null;
    readonly sinceMerge: number | null;
    /* files a merge stopped, with the first commit not adopted when this
     * folder's log says it */
    readonly stopped: readonly { readonly file: string; readonly at: string | null }[];
}

export interface BranchView {
    readonly row: BranchRow;
    /* its own sessions, newest first, commits inside */
    readonly sessions: readonly SessionRow[];
}

function num(v: unknown): number | null {
    return typeof v === "number" && Number.isFinite(v) ? v : null;
}

/* The branches in `lap branch list --json`'s output; anything that is not
 * that shape reads as none, as lap reads a malformed registry. */
export function parseBranchList(out: unknown, log: LapLog | null = null): BranchRow[] {
    const o = out as { ok?: unknown; branches?: unknown } | null;
    if (!o || o.ok !== true || !Array.isArray(o.branches)) return [];
    const rows: BranchRow[] = [];
    for (const b of o.branches as Record<string, unknown>[]) {
        if (!b || typeof b["id"] !== "string" || typeof b["name"] !== "string") continue;
        const id = b["id"] as string;
        const at = (file: string): string | null => {
            let found: string | null = null;
            for (const m of log?.merges ?? []) {
                if (m.branch !== id) continue;
                const s = m.stopped.find((x) => x.file === file);
                if (s) found = s.at;
            }
            return found;
        };
        const state = String(b["state"]);
        rows.push({
            id,
            name: b["name"] as string,
            state: (STATES.includes(state) ? state : "active") as BranchState,
            present: b["present"] === true,
            path: typeof b["path"] === "string" ? (b["path"] as string) : "",
            sinceBase: num(b["since_base"]),
            sinceMerge: num(b["since_merge"]),
            stopped: (Array.isArray(b["stopped"]) ? (b["stopped"] as unknown[]) : [])
                .filter((f): f is string => typeof f === "string")
                .map((file) => ({ file, at: at(file) })),
        });
    }
    return rows;
}

/* A branch folder's log cut to its own part: what came after its branch
 * record. A log with no branch record is returned as it is. */
export function ownPart(log: LapLog): LapLog {
    const at = log.branchAt;
    if (at === null) return log;
    const commits = log.commits.filter((c) => c.recIndex > at);
    const mine = new Set(commits);
    const sessions = log.sessions
        .filter((s) => s.recIndex > at)
        .map((s) => ({ ...s, commits: s.commits.filter((c) => mine.has(c)) }));
    return {
        ...log,
        commits,
        sessions,
        noSession: log.noSession.filter((c) => mine.has(c)),
    };
}

/* One branch as the view draws it: its row, and when its log could be read,
 * its own sessions. */
export function branchView(row: BranchRow, log: LapLog | null, now: Date): BranchView {
    if (!log) return { row, sessions: [] };
    const page = query(ownPart(log), { ...EMPTY_FILTER, range: "all" }, { grouped: true, page: 0, now });
    return { row, sessions: page.sessions };
}
