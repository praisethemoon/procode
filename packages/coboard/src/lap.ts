/* The link between a ticket and the work done for it.
 *
 * lap records edits in sessions, and a session can carry metadata. A session
 * started with `lap session start "purpose" --meta ticket=T-12` belongs to
 * T-12, and `lap session list --meta ticket=T-12` finds every such session —
 * so the board stores nothing about sessions at all. lap's log is the one
 * record of what changed; this only asks it.
 *
 * Everything here runs the `lap` binary (LAP_BIN, else `lap` on PATH) in the
 * board's root, and answers "no sessions" rather than failing when lap is
 * missing or the directory is not a lap repository: a board works without it.
 */

import { execFile } from "node:child_process";

import type { LapSummary } from "./summary";

export { summaryParts } from "./summary";
export type { LapSummary } from "./summary";

export interface LapSession {
    readonly id: string;
    readonly msg: string;
    readonly started: string;
    readonly ended: string | null;
    readonly commits: number;
    readonly active: boolean;
    readonly user?: string;
    /* How it ended; absent from a lap before summaries. */
    readonly summary?: LapSummary | null;
    /* The hash of its session_start record, and when lap merge adopted it,
     * the hash of the branch's own session_start it came from. */
    readonly hash?: string;
    readonly from?: string;
    /* A session still only in a branch folder: that branch's name, and for
     * a branch of a branch, the name of the branch it started from. */
    readonly branch?: string;
    readonly via?: string;
    /* An adopted session: the branch it came from, and the files that
     * branch's merge stopped, with the first commit to each not adopted. */
    readonly adoptedFrom?: string;
    readonly stops?: readonly { readonly file: string; readonly at: string }[];
}

/* What every commit says of itself: why the edit exists (edits serving one
 * goal share it), what it makes the code do, and whether its author bypassed
 * lap's message checks to record it. */
export interface LapMessage {
    readonly intent: string;
    readonly behavior: string;
    readonly forced?: true;
    /* How often lap amend corrected the text, which is then the latest. */
    readonly amended?: number;
}

/* A commit's text that a later lap amend replaced. */
export interface LapEarlierText {
    readonly intent: string;
    readonly behavior: string;
    readonly user: string | null;
    readonly ts: string;
}

export interface LapCommit extends LapMessage {
    readonly id: string;
    /* 64 lowercase hex digits; its first 7 are the short hash. */
    readonly hash: string;
    readonly ts: string;
    readonly user: string;
    readonly session: string | null;
    readonly file: string;
    readonly op: string;
}

export interface LapResult<T> {
    readonly ok: boolean;
    readonly value: T;
    /* Why lap could not answer, when it could not. */
    readonly error?: string;
}

function lapBin(): string {
    return process.env["LAP_BIN"] || "lap";
}

function run(cwd: string, args: string[]): Promise<Record<string, unknown>> {
    return new Promise((resolve, reject) => {
        execFile(lapBin(), [...args, "--json"], { cwd, timeout: 15_000, maxBuffer: 32 * 1024 * 1024 }, (err, stdout) => {
            let payload: Record<string, unknown> | null = null;
            try {
                payload = JSON.parse(String(stdout)) as Record<string, unknown>;
            } catch {
                payload = null;
            }
            if (payload && payload["ok"] === true) {
                resolve(payload);
            } else if (payload) {
                reject(new Error(String(payload["message"] ?? payload["error"] ?? "lap refused")));
            } else {
                reject(new Error(err ? err.message : "lap printed no JSON"));
            }
        });
    });
}

/* The sessions tagged with this ticket, oldest first: this folder's, and
 * those of the branches it started that were not adopted yet (lap branch
 * list, lap session list --branch). */
export async function ticketSessions(root: string, ticket: string): Promise<LapResult<LapSession[]>> {
    let main: LapSession[];
    try {
        const p = await run(root, ["session", "list", "--meta", `ticket=${ticket}`]);
        main = (p["sessions"] as LapSession[]) ?? [];
    } catch (e) {
        return { ok: false, value: [], error: (e as Error).message };
    }
    const branches: BranchSessions[] = [];
    /* what could not be read, each named: the sessions read are still
     * given, but never as if they were all of them */
    const errors: string[] = [];
    try {
        const list = await run(root, ["branch", "list"]);
        const rows = (list["branches"] as Record<string, unknown>[]) ?? [];
        /* a name two branches share (given before lap checked names across
         * nested branches) reads neither: each is named by its id */
        const count = new Map<string, number>();
        for (const b of rows) count.set(String(b["name"] ?? ""), (count.get(String(b["name"] ?? "")) ?? 0) + 1);
        const ref = (b: Record<string, unknown>): string =>
            (count.get(String(b["name"] ?? "")) ?? 0) > 1 ? String(b["id"] ?? "") : String(b["name"] ?? "");
        const names = new Map(rows.map((b) => [String(b["id"] ?? ""), ref(b)]));
        for (const b of rows) {
            const name = ref(b);
            const stops = Array.isArray(b["stops"]) ? (b["stops"] as { file: string; at: string }[]) : [];
            const via = typeof b["via"] === "string" ? names.get(b["via"]) : undefined;
            const more = via ? { via } : {};
            try {
                const p = await run(root, ["session", "list", "--meta", `ticket=${ticket}`, "--branch", name]);
                branches.push({ name, stops, sessions: (p["sessions"] as LapSession[]) ?? [], ...more });
            } catch (e) {
                branches.push({ name, stops, sessions: [], ...more });
                errors.push(`branch ${name}: ${(e as Error).message}`);
            }
        }
    } catch (e) {
        /* a lap from before branches knows no such command: this folder's
         * sessions are all there are; anything else is a failure to say */
        const message = (e as Error).message;
        if (!/unknown command/.test(message)) errors.push(`lap branch list: ${message}`);
    }
    const value = mergeSessions(main, branches);
    return errors.length ? { ok: false, value, error: errors.join("; ") } : { ok: true, value };
}

export interface BranchSessions {
    readonly name: string;
    /* for a branch of a branch: the name of the branch it started from */
    readonly via?: string;
    readonly stops: readonly { readonly file: string; readonly at: string }[];
    readonly sessions: readonly LapSession[];
}

/* One list of a ticket's sessions from this folder's and its branches'. A
 * branch's history holds this folder's up to its base, so a session both
 * have is shown once, as this folder's. A branch session lap merge adopted
 * is shown once, as the adopted one, marked with its branch and what that
 * branch's merge stopped. What is left is still only in its branch, and
 * says so. Oldest first. */
export function mergeSessions(main: readonly LapSession[], branches: readonly BranchSessions[]): LapSession[] {
    const out = main.map((s) => ({ ...s }));
    /* sessions already shown: a branch of a branch's history holds the
     * branch it started from up to its base, listed first */
    const ours = new Set(main.map((s) => s.hash).filter(Boolean));
    const adopted = new Map(out.filter((s) => s.from).map((s) => [s.from!, s]));
    /* sessions some branch adopted: shown as that branch's */
    const taken = new Set(branches.flatMap((b) => b.sessions.map((s) => s.from)).filter(Boolean));
    const rest: LapSession[] = [];
    for (const b of branches) {
        for (const s of b.sessions) {
            if (s.hash && ours.has(s.hash)) continue;
            if (s.hash) ours.add(s.hash);
            const a = s.hash ? adopted.get(s.hash) : undefined;
            if (a) {
                Object.assign(a, { adoptedFrom: b.name, stops: b.stops });
                continue;
            }
            if (s.hash && taken.has(s.hash)) continue;
            rest.push({ ...s, branch: b.name, ...(b.via ? { via: b.via } : {}) });
        }
    }
    return [...out, ...rest].sort((x, y) => x.started.localeCompare(y.started));
}

/* The commits one session made, newest first. */
export async function sessionCommits(root: string, session: string, branch?: string): Promise<LapResult<LapCommit[]>> {
    try {
        const p = await run(root, ["log", "--session", session, ...(branch ? ["--branch", branch] : [])]);
        return { ok: true, value: (p["commits"] as LapCommit[]) ?? [] };
    } catch (e) {
        return { ok: false, value: [], error: (e as Error).message };
    }
}

/* A session as a review (`lap rr`): the purpose, every edit in the order it
 * was made with its intent and behavior, and each file's net change over the
 * session. */
export interface LapReviewStep extends LapMessage {
    readonly id: string;
    readonly hash: string;
    readonly ts: string;
    readonly user: string;
    readonly file: string;
    readonly op: string;
    readonly new_start: number;
    readonly new_lines: number;
    /* adopted by lap merge: the original branch commit's hash */
    readonly from?: string;
}

export interface LapReviewFile {
    readonly path: string;
    readonly added: number;
    readonly removed: number;
    readonly deleted: boolean;
    /* A unified diff of the whole session's change to this file. */
    readonly diff: string;
}

export interface LapReview {
    readonly range: string;
    readonly purpose: string;
    readonly summary: LapSummary | null;
    readonly commits: number;
    readonly from: string;
    readonly to: string;
    readonly trajectory: readonly LapReviewStep[];
    readonly files: readonly LapReviewFile[];
}

export async function sessionReview(root: string, session: string, branch?: string): Promise<LapResult<LapReview | null>> {
    try {
        const p = await run(root, ["rr", session, ...(branch ? ["--branch", branch] : [])]);
        return {
            ok: true,
            value: {
                range: String(p["range"] ?? session),
                purpose: String(p["purpose"] ?? ""),
                summary: (p["summary"] as LapSummary | null | undefined) ?? null,
                commits: Number(p["commits"] ?? 0),
                from: String(p["from"] ?? ""),
                to: String(p["to"] ?? ""),
                trajectory: (p["trajectory"] as LapReviewStep[]) ?? [],
                files: (p["files"] as LapReviewFile[]) ?? [],
            },
        };
    } catch (e) {
        return { ok: false, value: null, error: (e as Error).message };
    }
}

/* One edit as the file before and after it: what a diff view needs. */
export interface LapDiff {
    readonly id: string;
    readonly hash: string;
    readonly file: string;
    readonly op: string;
    readonly intent: string;
    readonly behavior: string;
    readonly forced: boolean;
    /* lap amend: intent and behavior are the latest text; these it
     * replaced, oldest first (empty when never amended) */
    readonly earlier: readonly LapEarlierText[];
    readonly ts: string;
    readonly user: string;
    readonly session: string | null;
    readonly before: string;
    readonly after: string;
    /* The edited region, 1-based, as lap records it. */
    readonly oldStart: number;
    readonly oldLines: number;
    readonly newStart: number;
    readonly newLines: number;
    /* 1-based line in `after` where the edit starts, to scroll to. */
    readonly line: number;
}

/* lap gives the file after a commit and the lines the commit replaced; the
 * file before it is the one with those lines put back. `commit` is anything
 * `lap show` takes: an id ("L42"), or a hash or a prefix of at least 7 of its
 * hex digits, with or without "#". */
export async function commitDiff(root: string, commit: string): Promise<LapDiff> {
    const p = await run(root, ["show", commit, "--full-file"]);
    const op = String(p["op"]);
    const oldText = (p["old_text"] as string[]) ?? [];
    const newText = (p["new_text"] as string[]) ?? [];
    const joined = (lines: string[], nl: boolean) => (lines.length ? lines.join("\n") + (nl ? "\n" : "") : "");
    let before: string;
    let after: string;
    if (op === "create") {
        before = "";
        after = String(p["file_content"] ?? joined(newText, true));
    } else if (op === "delete" || p["file_deleted"] === true) {
        before = joined(oldText, true);
        after = "";
    } else {
        after = String(p["file_content"] ?? "");
        const nl = after.endsWith("\n");
        const lines = (nl ? after.slice(0, -1) : after).split("\n");
        const start = Math.max(0, Number(p["new_start"] ?? 1) - 1);
        lines.splice(start, Number(p["new_lines"] ?? newText.length), ...oldText);
        before = joined(lines, nl);
    }
    return {
        id: String(p["id"]),
        hash: String(p["hash"] ?? ""),
        file: String(p["file"]),
        op,
        intent: String(p["intent"] ?? ""),
        behavior: String(p["behavior"] ?? ""),
        forced: p["forced"] === true,
        earlier: Array.isArray(p["earlier"])
            ? (p["earlier"] as Record<string, unknown>[]).map((e) => ({
                  intent: String(e["intent"] ?? ""),
                  behavior: String(e["behavior"] ?? ""),
                  user: typeof e["user"] === "string" ? e["user"] : null,
                  ts: String(e["ts"] ?? ""),
              }))
            : [],
        ts: String(p["ts"] ?? ""),
        user: String(p["user"] ?? ""),
        session: typeof p["session"] === "string" ? p["session"] : null,
        before,
        after,
        oldStart: Number(p["old_start"] ?? 0),
        oldLines: Number(p["old_lines"] ?? 0),
        newStart: Number(p["new_start"] ?? 0),
        newLines: Number(p["new_lines"] ?? 0),
        line: Math.max(1, Number(p["new_start"] ?? 1)),
    };
}

/* Starts a lap session for a ticket, in the lap folder at or above `cwd`
 * (a branch folder records to its own line of history, even when the board
 * is its parent's). Fails when lap does, e.g. because another session is
 * already active. */
export async function startSession(cwd: string, ticket: string, purpose: string): Promise<string> {
    const p = await run(cwd, ["session", "start", purpose, "--meta", `ticket=${ticket}`, ...(await branchArgs(cwd))]);
    return String(p["id"]);
}

/* What lap needs to hear on a session start where branches exist: the
 * line this folder records to, as lap itself decides it (lap branch list's
 * self: a branch folder's id, which lap takes for the name; a lineage file
 * that leaked here through git is not one), or main in a folder with
 * branches. Nothing where there are none, or where lap cannot say (a lap
 * from before branches), and nothing when LAP_BRANCH already says it. */
export async function branchArgs(cwd: string): Promise<string[]> {
    if (process.env["LAP_BRANCH"]) {
        return [];
    }
    try {
        const list = await run(cwd, ["branch", "list"]);
        const self = list["self"] as Record<string, unknown> | null | undefined;
        if (self && typeof self["id"] === "string") return ["--branch", self["id"]];
        const rows = list["branches"];
        return Array.isArray(rows) && rows.length > 0 ? ["--branch", "main"] : [];
    } catch {
        return [];
    }
}

/* The command an agent should run to link its work to a ticket. */
export function sessionCommand(ticket: string, purpose = "<what you are doing>"): string {
    return `lap session start "${ticket}: ${purpose}" --meta ticket=${ticket}`;
}
