/* A session's lap edits as a reader wants them. Pure.
 *
 * ORDER: oldest first, so a session reads as the story of the work — the
 * review page reads the same way. lap lists newest first.
 *
 * GROUPS: a restructure is committed as numbered fragments sharing one
 * message, "(1/8) … (8/8)", because lap records one contiguous edit per
 * commit. Consecutive edits to the same file with the same message, once
 * that prefix is set aside, are one group: one row that expands to its edits.
 */

import type { LapCommit } from "coboard/lap";

export interface CommitGroup {
    readonly file: string;
    /* The first edit's: a group that starts by creating its file reads as a
     * creation. */
    readonly op: string;
    /* The message without its "(i/n) " prefix. */
    readonly msg: string;
    /* Oldest first. */
    readonly commits: readonly LapCommit[];
}

const FRAGMENT = /^\(\d+\/\d+\)\s+/;

export function baseMessage(msg: string): string {
    return msg.replace(FRAGMENT, "");
}

export function groupCommits(newestFirst: readonly LapCommit[]): CommitGroup[] {
    const out: { file: string; op: string; msg: string; commits: LapCommit[] }[] = [];
    for (const c of [...newestFirst].reverse()) {
        const msg = baseMessage(c.msg);
        const last = out[out.length - 1];
        if (last && last.file === c.file && last.msg === msg) {
            last.commits.push(c);
            continue;
        }
        out.push({ file: c.file, op: c.op, msg, commits: [c] });
    }
    return out;
}

/* A path as a name and the folder it is in, for a row that shows the name and
 * mutes the folder. */
export function splitPath(file: string): { name: string; dir: string } {
    const i = file.lastIndexOf("/");
    return i < 0 ? { name: file, dir: "" } : { name: file.slice(i + 1), dir: file.slice(0, i) };
}
