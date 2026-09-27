/* Turns bare item ids in Markdown into links, so "blocked by T-4" in a
 * description or comment jumps to T-4. Code spans and fenced blocks are left
 * alone, as are ids that are already part of a link. */

const ID = /(^|[^\w\-\[/#])([EMT]-[1-9][0-9]*)(?![\w-])/g;

export function linkifyIds(md: string): string {
    return md
        .split(/(```[\s\S]*?```|`[^`\n]*`|\[[^\]\n]*\]\([^)\n]*\))/)
        .map((part, i) => (i % 2 === 1 ? part : part.replace(ID, (_m, pre: string, id: string) => `${pre}[${id}](#${id})`)))
        .join("");
}

/* The id an in-board link points at, or null for any other href. */
export function linkTarget(href: string | undefined): string | null {
    const m = /^#([EMT]-[1-9][0-9]*)$/.exec(href ?? "");
    return m ? m[1] : null;
}

/* A lap commit named in a commit's text: "#" and 7 to 64 hex digits of its
 * hash, or its id ("L1029"). `ref` is what `lap show` takes to find it — a hash
 * prefix is resolved by lap — and `text` is the name as written. */
export interface CommitRef {
    readonly ref: string;
    readonly text: string;
}

const COMMIT_REF = /(^|[^\w#-])(#([0-9a-fA-F]{7,64})|L[1-9][0-9]*)(?![\w-])/g;

/* Plain text as runs of text and the commits it names, in order. */
export function commitRefs(text: string): (string | CommitRef)[] {
    const out: (string | CommitRef)[] = [];
    let at = 0;
    for (const m of text.matchAll(COMMIT_REF)) {
        const start = (m.index ?? 0) + m[1].length;
        if (start > at) out.push(text.slice(at, start));
        out.push({ ref: m[3] ? m[3].toLowerCase() : m[2], text: m[2] });
        at = start + m[2].length;
    }
    if (at < text.length) out.push(text.slice(at));
    return out;
}
