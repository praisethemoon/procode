/* References from one commit's text to another: `#` and 7 to 64 hex digits
 * (a hash or a prefix of one, any case), or a plain commit id (`L42`). The
 * view turns each into a link that reveals the commit it names.
 * Pure — the webview parses with it, the host links and resolves with it —
 * so no vscode or node imports.
 */

import { LapLog, mdEscape } from "./model";

/* A run of text, or a reference within it: `ref` is the token as written. */
export interface RefPart {
    readonly text: string;
    readonly ref?: string;
}

/* A token stands alone: no word character (or `#`) on either side, so
 * `HTML5`, `a#fa9cebd`, `#fa9cebdz` and a 65-digit run are not references. */
const REF = /(?<![\w#])(?:#[0-9a-fA-F]{7,64}|L\d+)(?!\w)/g;

export function parseRefs(text: string): RefPart[] {
    const parts: RefPart[] = [];
    let at = 0;
    for (const m of text.matchAll(REF)) {
        const i = m.index ?? 0;
        if (i > at) parts.push({ text: text.slice(at, i) });
        parts.push({ text: m[0], ref: m[0] });
        at = i + m[0].length;
    }
    if (at < text.length) parts.push({ text: text.slice(at) });
    return parts;
}

/* Multiline prose as escaped markdown with hard line breaks (as mdProse),
 * each reference a link to `target(ref)`. */
export function mdLinked(text: string, target: (ref: string) => string): string {
    return text
        .split("\n")
        .map((line) =>
            parseRefs(line)
                .map((p) => (p.ref === undefined ? mdEscape(p.text) : `[${mdEscape(p.text)}](${target(p.ref)})`))
                .join(""),
        )
        .join("  \n");
}

export type Resolved =
    | { readonly ok: true; readonly id: string }
    | { readonly ok: false; readonly error: "unknown_ref" | "ambiguous_ref"; readonly matches: readonly string[] };

/* A reference resolved against the log the way `lap show` does: an id, or a
 * hash prefix of at least 7 hex digits, with or without the `#`. */
export function resolveRef(log: LapLog, ref: string): Resolved {
    const r = ref.trim().replace(/^#/, "");
    if (/^L\d+$/i.test(r)) {
        const c = log.commits.find((x) => x.id.toLowerCase() === r.toLowerCase());
        return c ? { ok: true, id: c.id } : { ok: false, error: "unknown_ref", matches: [] };
    }
    if (!/^[0-9a-f]{7,64}$/i.test(r)) return { ok: false, error: "unknown_ref", matches: [] };
    const prefix = r.toLowerCase();
    const matches = log.commits.filter((c) => c.hash.startsWith(prefix)).map((c) => c.id);
    if (matches.length === 1) return { ok: true, id: matches[0] };
    return { ok: false, error: matches.length === 0 ? "unknown_ref" : "ambiguous_ref", matches };
}
