/* How a lap edit's intent and behavior are written next to its diff — the
 * same text and layout Lap History uses (packages/lap-vscode), so an edit
 * reads the same from either extension. Pure: no vscode import, so it is
 * testable. */

import type { LapDiff } from "coboard";

import { shortHash } from "./commits";
import { commitRefs } from "./linkify";

/* The command a commit named in a comment's text runs: it opens that commit
 * the same way, so the comment's Markdown must trust it. */
export const SHOW_EDIT = "coboard.showEdit";

export function summaryLine(msg: string): string {
    const nl = msg.indexOf("\n");
    return nl < 0 ? msg : msg.slice(0, nl);
}

/* Escapes Markdown while keeping ordinary spaces, so long lines still wrap
 * (MarkdownString.appendText turns every space into &nbsp;). */
export function mdEscape(text: string): string {
    return text.replace(/[\\`*_{}[\]()#+\-.!~<>|]/g, (m) => "\\" + m);
}

/* Multiline prose as escaped Markdown with hard line breaks. */
export function mdProse(text: string): string {
    return text.split("\n").map(mdEscape).join("  \n");
}

/* A commit's text as mdProse, with every commit it names ("#1a2b3c4",
 * "L1029") a link that opens that commit. */
export function mdCommitText(text: string): string {
    return text
        .split("\n")
        .map((line) =>
            commitRefs(line)
                .map((p) =>
                    typeof p === "string" ? mdEscape(p) : `[${mdEscape(p.text)}](command:${SHOW_EDIT}?${encodeURIComponent(JSON.stringify([p.ref]))})`,
                )
                .join(""),
        )
        .join("  \n");
}

/* "line 4", "lines 4-7", "line 3 (deleted)", "lines 1-2 (insertion)". */
export function regionLabel(d: Pick<LapDiff, "oldStart" | "oldLines" | "newStart" | "newLines">): string {
    if (d.newLines === 0 && d.oldLines > 0) {
        return d.oldLines === 1 ? `line ${d.oldStart} (deleted)` : `lines ${d.oldStart}-${d.oldStart + d.oldLines - 1} (deleted)`;
    }
    if (d.oldLines === 0 && d.newLines > 0) {
        return d.newLines === 1 ? `line ${d.newStart} (insertion)` : `lines ${d.newStart}-${d.newStart + d.newLines - 1} (insertion)`;
    }
    return d.newLines === 1 ? `line ${d.newStart}` : `lines ${d.newStart}-${d.newStart + d.newLines - 1}`;
}

/* The changed region in the after-document, 0-based, inclusive. A deletion
 * anchors on the line where the removal happened. */
export function regionLines(d: Pick<LapDiff, "newStart" | "newLines">): { start: number; end: number } {
    const start = Math.max(0, d.newStart - 1);
    return { start, end: d.newLines > 0 ? start + d.newLines - 1 : start };
}

/* The UTC timestamp in the viewer's time zone and locale: the date, a
 * space, and the time to the second. */
export function localTime(iso: string): string {
    const d = new Date(iso);
    if (isNaN(d.getTime())) {
        return iso;
    }
    const date = d.toLocaleDateString(undefined, { year: "numeric", month: "2-digit", day: "2-digit" });
    const time = d.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit", second: "2-digit" });
    return `${date} ${time}`;
}

/* The comment's author line (the id and short hash) and Markdown body: the
 * intent, the behavior, a mark when the message checks were bypassed, and
 * the session. */
export function commentText(d: LapDiff, sessionMsg: string | null): { author: string; body: string } {
    const footer = d.session
        ? `session ${d.session}` + (sessionMsg ? `: ${summaryLine(sessionMsg)}` : "")
        : "committed outside any session (--no-session)";
    const forced = d.forced ? `\n\n**forced**: *${mdEscape("committed with --force-message, past lap's message checks")}*` : "";
    const amended = d.earlier.length
        ? `\n\n**amended**: *${mdEscape(`corrected with lap amend ${d.earlier.length === 1 ? "once" : `${d.earlier.length} times`}; the earlier text${d.earlier.length === 1 ? "" : "s"}, oldest first:`)}*` +
          d.earlier
              .map((e) => {
                  const who = [localTime(e.ts), e.user ?? ""].filter((s) => s !== "").join(" ");
                  const quote = (s: string) => mdCommitText(s).split("\n").join("\n> ");
                  return `\n\n> ${who ? `${mdEscape(who)}  \n> ` : ""}Intent: ${quote(e.intent)}  \n> Behavior: ${quote(e.behavior)}`;
              })
              .join("")
        : "";
    return {
        author: `${d.id} · ${shortHash(d.hash)} @ ${localTime(d.ts)}` + (d.user ? ` ${d.user}` : "") + ":",
        body: `**Intent**: ${mdCommitText(d.intent)}\n\n**Behavior**: ${mdCommitText(d.behavior)}${forced}${amended}\n\n---\n\n*${mdEscape(footer)}*\n\n&nbsp;`,
    };
}
