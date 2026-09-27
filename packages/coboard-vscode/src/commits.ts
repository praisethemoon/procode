/* A session's lap edits as a reader wants them. Pure.
 *
 * ORDER: oldest first, so a session reads as the story of the work — the
 * review page reads the same way. lap lists newest first.
 *
 * GROUPS: every commit says why it exists (its intent, shared by the edits
 * that serve one goal) and what it makes the code do (its behavior).
 * Consecutive edits to the same file with the same intent are one group: the
 * intent once, and under it a line per edit with its behavior.
 */

import type { LapCommit } from "coboard/lap";

export interface CommitGroup {
    readonly file: string;
    /* The first edit's: a group that starts by creating its file reads as a
     * creation. */
    readonly op: string;
    readonly intent: string;
    /* Oldest first. */
    readonly commits: readonly LapCommit[];
}

export function groupCommits(newestFirst: readonly LapCommit[]): CommitGroup[] {
    const out: { file: string; op: string; intent: string; commits: LapCommit[] }[] = [];
    for (const c of [...newestFirst].reverse()) {
        const last = out[out.length - 1];
        if (last && last.file === c.file && last.intent === c.intent) {
            last.commits.push(c);
            continue;
        }
        out.push({ file: c.file, op: c.op, intent: c.intent, commits: [c] });
    }
    return out;
}

/* A commit's hash as lap abbreviates it: its first 7 hex digits. */
export function shortHash(hash: string): string {
    return hash.slice(0, 7);
}

/* A path as a name and the folder it is in, for a row that shows the name and
 * mutes the folder. */
export function splitPath(file: string): { name: string; dir: string } {
    const i = file.lastIndexOf("/");
    return i < 0 ? { name: file, dir: "" } : { name: file.slice(i + 1), dir: file.slice(0, i) };
}
