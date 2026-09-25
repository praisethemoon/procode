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
