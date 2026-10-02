/* Finishing a ticket in one step (the tickets skill's close): end its lap
 * session with a summary, comment on the ticket, set its status, and hand
 * back the git commit that records the work.
 *
 * GIT IS NEVER RUN HERE. A commit is the person's: their conventions,
 * signing, hooks and whatever they have staged. What this adds is what lap
 * knows and a person easily gets wrong: exactly which files the session
 * touched. The command handed back stages those and nothing else.
 *
 * Every refusal comes before anything is written: no active session for the
 * ticket, or a file the session touched with edits lap has not recorded
 * (committed later, those edits would land in git outside any lap commit).
 * Pending edits elsewhere are named, not refused: they are not this
 * ticket's.
 *
 * Pure but for the lap it is handed (and the board the caller writes), so
 * the tests drive it with a stand-in. */

import { BoardError } from "./store";

/* Runs one lap command with --json in the lap folder; resolves with its
 * answer, rejects with lap's message. */
export type LapRun = (args: string[]) => Promise<Record<string, unknown>>;

export interface FinishInput {
    readonly ticket: string;
    readonly done: string;
    readonly decided?: string;
    readonly left?: string;
    /* the git commit's subject; the ticket id is added */
    readonly subject: string;
    readonly tests?: string;
    readonly notVerified?: string;
}

export interface Ended {
    /* the session as people name it: S<n>, or <branch>/S<n> */
    readonly session: string;
    readonly comment: string;
    readonly git: GitCommand;
    /* pending edits in files the session did not touch: not this ticket's */
    readonly otherPending: readonly string[];
}

export interface GitCommand {
    /* the files to stage, relative to the lap folder, in the order the
     * session first touched them, then lap's log (and the board's) */
    readonly paths: readonly string[];
    readonly message: string;
    /* both commands, ready to paste into a shell in the lap folder */
    readonly command: string;
}

/* A path or message as one shell word: bare when it is plain, else in
 * single quotes. */
export function shellWord(s: string): string {
    return /^[A-Za-z0-9_./@%+=:,-]+$/.test(s) ? s : `'${s.replace(/'/g, `'\\''`)}'`;
}

/* The commit's message: the subject with the ticket id at the end, unless it
 * already carries it. */
export function commitMessage(subject: string, ticket: string): string {
    const s = subject.trim().replace(/\s+/g, " ");
    return new RegExp(`\\(${ticket}\\)$`).test(s) ? s : `${s} (${ticket})`;
}

/* The files a session's commits leave for git to record: each file, once, in
 * the order the session first touched it, except one whose last commit in
 * the session is an untrack (lap stopping following a file says nothing
 * about git). */
export function sessionFiles(commits: readonly { file: string; op: string }[]): string[] {
    const last = new Map<string, string>();
    for (const c of commits) last.set(c.file, c.op);
    const order: string[] = [];
    const seen = new Set<string>();
    for (const c of commits) {
        if (seen.has(c.file)) continue;
        seen.add(c.file);
        if (last.get(c.file) !== "untrack") order.push(c.file);
    }
    return order;
}

export function gitCommand(files: readonly string[], ticket: string, subject: string, boardHere: boolean): GitCommand {
    const paths = [...files, ".lap/log", ...(boardHere ? [".coboard/log.jsonl"] : [])];
    const message = commitMessage(subject, ticket);
    const command = `git add -- ${paths.map(shellWord).join(" ")} && git commit -m ${shellWord(message)}`;
    return { paths, message, command };
}

/* The ticket's closing comment: what a reader of the board needs, from the
 * session's summary and what only the caller knows. */
export function finishComment(session: string, input: FinishInput, git: GitCommand): string {
    const parts = [`Done in lap ${session}; git: "${git.message}" (\`git log --grep ${input.ticket}\`).`];
    parts.push(`**Done.** ${input.done.trim()}`);
    if (input.decided?.trim()) parts.push(`**Decided.** ${input.decided.trim()}`);
    if (input.left?.trim()) parts.push(`**Left.** ${input.left.trim()}`);
    if (input.tests?.trim()) parts.push(`**Tests.** ${input.tests.trim()}`);
    if (input.notVerified?.trim()) parts.push(`**Not verified.** ${input.notVerified.trim()}`);
    return parts.join("\n\n");
}

interface SessionRow {
    readonly id: string;
    readonly ref?: string;
    readonly active?: boolean;
}

/* Ends the ticket's session. Throws BoardError (no_session, pending_edits,
 * lap) before anything is written; after the session ends, returns what the
 * caller writes on the board and the git command. */
export async function endTicketSession(lap: LapRun, input: FinishInput, boardHere: boolean): Promise<Ended> {
    const ticket = input.ticket;
    const ask = async (args: string[]) => {
        try {
            return await lap(args);
        } catch (e) {
            throw new BoardError("lap", `lap ${args.slice(0, 2).join(" ")}: ${(e as Error).message}`);
        }
    };
    const list = await ask(["session", "list", "--meta", `ticket=${ticket}`]);
    const active = ((list["sessions"] as SessionRow[]) ?? []).find((s) => s.active);
    if (!active) {
        throw new BoardError(
            "no_session",
            `${ticket} has no active lap session to finish; its work is recorded in one started with: lap session start "${ticket}: …" --meta ticket=${ticket}`,
        );
    }
    const session = active.ref ?? active.id;
    const log = await ask(["log", "--session", active.id]);
    const commits = (log["commits"] as { file: string; op: string }[]) ?? [];
    // lap log lists newest first; the session's order is the other way
    const files = sessionFiles([...commits].reverse());
    const touched = new Set(commits.map((c) => c.file));

    const status = await ask(["status"]);
    const pending = ((status["files"] as { path: string }[]) ?? []).map((f) => f.path);
    const mine = pending.filter((p) => touched.has(p));
    if (mine.length) {
        throw new BoardError(
            "pending_edits",
            `${mine.join(", ")} ${mine.length === 1 ? "has" : "have"} edits lap has not recorded in ${session}: commit them (lap commit) before finishing ${ticket}`,
        );
    }

    const git = gitCommand(files, ticket, input.subject, boardHere);
    const end = ["session", "end", "--done", input.done.trim()];
    if (input.decided?.trim()) end.push("--decided", input.decided.trim());
    if (input.left?.trim()) end.push("--left", input.left.trim());
    await ask(end);
    return {
        session,
        comment: finishComment(session, input, git),
        git,
        otherPending: pending.filter((p) => !touched.has(p)),
    };
}
