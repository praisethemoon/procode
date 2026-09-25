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

export interface LapSession {
    readonly id: string;
    readonly msg: string;
    readonly started: string;
    readonly ended: string | null;
    readonly commits: number;
    readonly active: boolean;
    readonly user?: string;
}

export interface LapCommit {
    readonly id: string;
    readonly ts: string;
    readonly user: string;
    readonly file: string;
    readonly op: string;
    readonly msg: string;
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

/* One edit as the file before and after it: what a diff view needs. */
export interface LapDiff {
    readonly id: string;
    readonly file: string;
    readonly op: string;
    readonly msg: string;
    readonly before: string;
    readonly after: string;
    /* 1-based line in `after` where the edit starts, to scroll to. */
    readonly line: number;
}

/* lap gives the file after a commit and the lines the commit replaced; the
 * file before it is the one with those lines put back. */
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
        file: String(p["file"]),
        op,
        msg: String(p["msg"] ?? ""),
        before,
        after,
        line: Math.max(1, Number(p["new_start"] ?? 1)),
    };
}

/* Starts a lap session for a ticket. Fails when lap does, e.g. because
 * another session is already active. */
export async function startSession(root: string, ticket: string, purpose: string): Promise<string> {
    const p = await run(root, ["session", "start", purpose, "--meta", `ticket=${ticket}`]);
    return String(p["id"]);
}

/* The command an agent should run to link its work to a ticket. */
export function sessionCommand(ticket: string, purpose = "<what you are doing>"): string {
    return `lap session start "${ticket}: ${purpose}" --meta ticket=${ticket}`;
}
