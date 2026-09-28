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
    /* false for a branch known here only by its chunks (brought by git from
     * another clone, or pruned once merged): it has no folder here */
    readonly registered: boolean;
    /* commits since its base and since the last merge; null when its history
     * is nowhere to be read */
    readonly sinceBase: number | null;
    readonly sinceMerge: number | null;
    /* files a merge stopped, with the first commit not adopted when this
     * folder's log says it */
    readonly stopped: readonly { readonly file: string; readonly at: string | null }[];
    /* the branch it started from, for a branch of one of this folder's
     * branches; null for this folder's own */
    readonly via: string | null;
}

export interface BranchView {
    readonly row: BranchRow;
    /* its own sessions, newest first, commits inside */
    readonly sessions: readonly SessionRow[];
    /* why its history cannot be read, in lap's words (then no sessions:
     * never part of a history); null when it can */
    readonly problem: string | null;
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
            registered: b["registered"] !== false, /* absent: a lap from before */
            sinceBase: num(b["since_base"]),
            sinceMerge: num(b["since_merge"]),
            stopped: (Array.isArray(b["stopped"]) ? (b["stopped"] as unknown[]) : [])
                .filter((f): f is string => typeof f === "string")
                .map((file) => ({ file, at: at(file) })),
            via: typeof b["via"] === "string" ? (b["via"] as string) : null,
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
export function branchView(row: BranchRow, log: LapLog | null, now: Date, problem: string | null = null): BranchView {
    if (!log || problem) return { row, sessions: [], problem };
    const page = query(ownPart(log), { ...EMPTY_FILTER, range: "all" }, { grouped: true, page: 0, now });
    return { row, sessions: page.sessions, problem: null };
}

/* Why a lap run gave no answer, in lap's words: its JSON error message, or
 * why it could not run at all (not installed, not executable). null when
 * it answered. */
export function lapFailure(err: { message: string } | null, stdout: string): string | null {
    try {
        const o = JSON.parse(stdout) as { ok?: unknown; message?: unknown; error?: unknown };
        if (o && o.ok === true) return null;
        if (o && (typeof o.message === "string" || typeof o.error === "string")) return String(o.message ?? o.error);
    } catch {
        /* not lap's JSON */
    }
    return err ? `lap could not run: ${err.message}` : "lap printed no answer";
}

/* The lap executable: Lap History's own setting, else the Board's, else
 * the one on PATH. */
export function lapBin(own: string | undefined, board: string | undefined): string {
    return own?.trim() || board?.trim() || "lap";
}

/* Whether lap is asked for the branch list again: when what the list
 * depends on moved (its signature), or when told to. A signature is kept
 * only once lap answered, so a run that failed (no lap, a wrong lap.path,
 * an older lap) is tried again on the next refresh; one still running is
 * not started twice. */
export class ListGate {
    private answered = "\0"; /* nothing asked yet */
    private asking: string | null = null;
    private latest = 0; /* the newest request made */

    /* A request's number: its answer is used only while no newer one was
     * made, so an older answer landing last never overwrites a newer. */
    begin(): number {
        return ++this.latest;
    }

    isLatest(n: number): boolean {
        return n === this.latest;
    }

    ask(sig: string, force: boolean): boolean {
        if (!force && (sig === this.answered || sig === this.asking)) return false;
        this.asking = sig;
        return true;
    }

    /* n: the request's number; an older one's answer is no longer what the
     * list is, and is not remembered as answered */
    done(sig: string, ok: boolean, n = this.latest): void {
        if (this.asking === sig) this.asking = null;
        if (ok && n === this.latest) this.answered = sig;
    }
}

/* Runs lap with args in cwd and hands back its error and stdout. */
export type LapExec = (args: string[], cwd: string, cb: (err: { message: string } | null, stdout: string) => void) => void;

/* Asks lap branch list through gate: cb gets lap's failure in its words
 * (null when it answered) and its JSON (null when none). False, and cb is
 * never called, when nothing moved; nor is it for an answer that lands
 * after a newer request's. */
export function askBranchList(
    gate: ListGate,
    sig: string,
    force: boolean,
    root: string,
    exec: LapExec,
    cb: (error: string | null, out: unknown) => void,
): boolean {
    if (!gate.ask(sig, force)) return false;
    const n = gate.begin();
    exec(["branch", "list", "--json"], root, (err, stdout) => {
        const error = lapFailure(err, stdout);
        gate.done(sig, error === null, n);
        if (!gate.isLatest(n)) return; /* a newer request answers */
        let out: unknown = null;
        try {
            out = JSON.parse(stdout);
        } catch {
            /* no lap, or one from before branches: no branches */
        }
        cb(error, out);
    });
    return true;
}

/* Where a branch is tended (lap branch move, forget): in the folder that
 * started it — for a branch of a branch, that branch's folder, while it is
 * there; else this folder, whose lap then says what it can. */
export function tendFolder(root: string, rows: readonly BranchRow[], name: string): string {
    const r = rows.find((x) => x.name === name);
    if (!r || !r.via) return root;
    const parent = rows.find((x) => x.id === r.via);
    return parent && parent.present && parent.path ? parent.path : root;
}

/* A watcher that calls fn on any change under it: a chunk written or made,
 * and also one removed, as when a branch's folder is deleted. */
export interface ChangeWatcher {
    onDidChange(fn: () => void): unknown;
    onDidCreate(fn: () => void): unknown;
    onDidDelete(fn: () => void): unknown;
}

export function onAnyChange(w: ChangeWatcher, fn: () => void): void {
    w.onDidChange(fn);
    w.onDidCreate(fn);
    w.onDidDelete(fn);
}
