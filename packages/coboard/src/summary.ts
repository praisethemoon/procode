/* How a lap session ended, when it was ended with a summary (`lap session
 * end --done/--decided/--left`). Pure, so a webview can draw it without the
 * code that runs lap. */

/* What the session achieved, what it decided and why, what it left. Each
 * part is null when not given. */
export interface LapSummary {
    readonly done: string | null;
    readonly decided: string | null;
    readonly left: string | null;
}

/* The parts a summary gives, in order, labelled for a reader; none for a
 * session that is open, ended without one, or read from a lap before
 * summaries. */
export function summaryParts(s: LapSummary | null | undefined): { readonly label: string; readonly text: string }[] {
    if (!s) return [];
    const parts: [string, string | null][] = [
        ["Done", s.done],
        ["Decided", s.decided],
        ["Left", s.left],
    ];
    return parts.filter((p): p is [string, string] => typeof p[1] === "string" && p[1] !== "").map(([label, text]) => ({ label, text }));
}
