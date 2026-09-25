/* How a lap edit's reason is written next to its diff — the same text and
 * layout Lap History uses (packages/lap-vscode), so an edit reads the same
 * from either extension. Pure: no vscode import, so it is testable. */

import type { LapDiff } from "coboard";

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

/* The UTC timestamp in the viewer's timezone, "<date>:<time>". */
export function localTime(iso: string): string {
    const d = new Date(iso);
    if (isNaN(d.getTime())) {
        return iso;
    }
    const date = d.toLocaleDateString(undefined, { year: "numeric", month: "2-digit", day: "2-digit" });
    const time = d.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" });
    return `${date}:${time}`;
}

/* The comment's author line and Markdown body. */
export function commentText(d: LapDiff, sessionMsg: string | null): { author: string; body: string } {
    const footer = d.session
        ? `session ${d.session}` + (sessionMsg ? `: ${summaryLine(sessionMsg)}` : "")
        : "committed outside any session (--no-session)";
    return {
        author: `${d.id} @ ${localTime(d.ts)}` + (d.user ? ` ${d.user}` : "") + ":",
        body: `${mdProse(d.msg)}\n\n---\n\n*${mdEscape(footer)}*\n\n&nbsp;`,
    };
}
