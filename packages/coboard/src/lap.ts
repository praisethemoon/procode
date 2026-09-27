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
import * as fs from "node:fs";
import * as path from "node:path";

export interface LapSession {
    readonly id: string;
    readonly msg: string;
    readonly started: string;
    readonly ended: string | null;
    readonly commits: number;
    readonly active: boolean;
    readonly user?: string;
}

/* What every commit says of itself: why the edit exists (edits serving one
 * goal share it), what it makes the code do, and whether its author bypassed
 * lap's message checks to record it. */
export interface LapMessage {
    readonly intent: string;
    readonly behavior: string;
    readonly forced?: true;
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

/* The sessions tagged with this ticket, oldest first. */
export async function ticketSessions(root: string, ticket: string): Promise<LapResult<LapSession[]>> {
    try {
        const p = await run(root, ["session", "list", "--meta", `ticket=${ticket}`]);
        return { ok: true, value: (p["sessions"] as LapSession[]) ?? [] };
    } catch (e) {
        return { ok: false, value: [], error: (e as Error).message };
    }
}

/* The commits one session made, newest first. */
export async function sessionCommits(root: string, session: string): Promise<LapResult<LapCommit[]>> {
    try {
        const p = await run(root, ["log", "--session", session]);
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
    readonly commits: number;
    readonly from: string;
    readonly to: string;
    readonly trajectory: readonly LapReviewStep[];
    readonly files: readonly LapReviewFile[];
}

export async function sessionReview(root: string, session: string): Promise<LapResult<LapReview | null>> {
    try {
        const p = await run(root, ["rr", session]);
        return {
            ok: true,
            value: {
                range: String(p["range"] ?? session),
                purpose: String(p["purpose"] ?? ""),
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
    const p = await run(cwd, ["session", "start", purpose, "--meta", `ticket=${ticket}`, ...branchArgs(cwd)]);
    return String(p["id"]);
}

/* What lap needs to hear on a session start where branches exist: the
 * branch folder's id (lap takes it for the name), or main in a folder with
 * branches. Nothing where there are none, so a lap from before branches
 * still works, and nothing when LAP_BRANCH already says it. */
export function branchArgs(cwd: string): string[] {
    if (process.env["LAP_BRANCH"]) {
        return [];
    }
    let dir = path.resolve(cwd);
    for (;;) {
        const lapDir = path.join(dir, ".lap");
        if (fs.existsSync(lapDir)) {
            try {
                const id = fs.readFileSync(path.join(lapDir, "lineage"), "utf8").trim();
                if (id) return ["--branch", id];
            } catch {
                /* not a branch folder */
            }
            try {
                const reg: unknown = JSON.parse(fs.readFileSync(path.join(lapDir, "branches.json"), "utf8"));
                if (Array.isArray(reg) && reg.length > 0) return ["--branch", "main"];
            } catch {
                /* no branches, or a registry lap would read as none */
            }
            return [];
        }
        const up = path.dirname(dir);
        if (up === dir) {
            return [];
        }
        dir = up;
    }
}

/* The command an agent should run to link its work to a ticket. */
export function sessionCommand(ticket: string, purpose = "<what you are doing>"): string {
    return `lap session start "${ticket}: ${purpose}" --meta ticket=${ticket}`;
}
