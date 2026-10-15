/* Codex's `.codex/config.toml`: the project's MCP servers, one table
 * `[mcp_servers.<name>]` each. Pure functions over the file's text, with the
 * rules of mcpjson.ts: write our servers, drop the tables procode wrote for
 * servers that are off, and tell when a table points at a copy of procode
 * that is no longer installed.
 *
 * There is no TOML library here: the file is the person's, comments and
 * layout included, so only our servers' tables are cut out or written in,
 * and every other byte stays where it was. A scanner finds the tables'
 * headers, skipping strings, comments and multi-line arrays. */

import { HTTP_PATH, RETIRED, ServerEntry } from "./mcpjson";

interface Table {
    /* the header's keys; [] for the root, before any header */
    readonly path: string[];
    readonly start: number;
    /* after its last line that is not blank or a comment: a comment just
     * above the next header belongs to that one */
    readonly end: number;
}

function unquote(key: string): string {
    const k = key.trim();
    if (k.startsWith('"')) {
        try {
            return JSON.parse(k) as string;
        } catch {
            return k.slice(1, -1);
        }
    }
    return k.startsWith("'") ? k.slice(1, -1) : k;
}

/* "a.\"b.c\".d" → ["a", "b.c", "d"] */
function splitKey(key: string): string[] {
    const out: string[] = [];
    let cur = "";
    let q: string | null = null;
    for (let i = 0; i < key.length; i++) {
        const c = key[i]!;
        if (q) {
            cur += c;
            if (c === "\\" && q === '"') cur += key[++i] ?? "";
            else if (c === q) q = null;
        } else if (c === '"' || c === "'") {
            q = c;
            cur += c;
        } else if (c === ".") {
            out.push(unquote(cur));
            cur = "";
        } else cur += c;
    }
    out.push(unquote(cur));
    return out;
}

/* The file's tables, in order, the root first. */
export function tables(text: string): Table[] {
    const out: { path: string[]; start: number; end: number }[] = [{ path: [], start: 0, end: 0 }];
    let str: string | null = null; // the open string's quote: " ' """ '''
    let depth = 0; // brackets and braces open in a value
    let i = 0;
    while (i < text.length) {
        const lineEnd = text.indexOf("\n", i) === -1 ? text.length : text.indexOf("\n", i) + 1;
        const lead = /^[ \t]*/.exec(text.slice(i, lineEnd))![0].length;
        if (!str && depth === 0 && text[i + lead] === "[") {
            // A header: [a.b] or [[a.b]], to its closing bracket.
            const array = text[i + lead + 1] === "[";
            let j = i + lead + (array ? 2 : 1);
            let q: string | null = null;
            let key = "";
            for (; j < lineEnd; j++) {
                const c = text[j]!;
                if (q) {
                    if (c === "\\" && q === '"') key += c + (text[++j] ?? "");
                    else {
                        if (c === q) q = null;
                        key += c;
                    }
                } else if (c === '"' || c === "'") {
                    q = c;
                    key += c;
                } else if (c === "]") break;
                else key += c;
            }
            out.push({ path: splitKey(key), start: i, end: lineEnd });
            i = lineEnd;
            continue;
        }
        let content = str !== null || depth > 0;
        for (let j = i; j < lineEnd; j++) {
            const c = text[j]!;
            if (str) {
                if (c === "\\" && (str === '"' || str === '"""')) j++;
                else if (text.startsWith(str, j)) {
                    j += str.length - 1;
                    str = null;
                } else if (c === "\n" && str.length === 1) str = null; // an unclosed one-line string ends here
                continue;
            }
            if (c === "#") break;
            if (c === " " || c === "\t" || c === "\r" || c === "\n") continue;
            content = true;
            if (text.startsWith('"""', j) || text.startsWith("'''", j)) {
                str = text.slice(j, j + 3);
                j += 2;
            } else if (c === '"' || c === "'") str = c;
            else if (c === "[" || c === "{") depth++;
            else if ((c === "]" || c === "}") && depth > 0) depth--;
        }
        if (content) out[out.length - 1]!.end = lineEnd;
        i = lineEnd;
    }
    return out;
}

/* The text of a table, header to end. */
function body(text: string, t: Table): string {
    return text.slice(t.start, t.end);
}

/* A string value of `key` in a table: "basic" or 'literal'. */
function stringValue(src: string, key: string): string | undefined {
    const m = new RegExp(`^[ \\t]*${key}[ \\t]*=[ \\t]*(?:"((?:[^"\\\\\\n]|\\\\.)*)"|'([^'\\n]*)')`, "m").exec(src);
    if (!m) return undefined;
    if (m[2] !== undefined) return m[2];
    try {
        return JSON.parse(`"${m[1]}"`) as string;
    } catch {
        return m[1];
    }
}

/* The first string of `args = [...]`. */
function firstArg(src: string): string | undefined {
    const m = /^[ \t]*args[ \t]*=[ \t]*\[\s*(?:"((?:[^"\\\n]|\\.)*)"|'([^'\n]*)')/m.exec(src);
    if (!m) return undefined;
    if (m[2] !== undefined) return m[2];
    try {
        return JSON.parse(`"${m[1]}"`) as string;
    } catch {
        return m[1];
    }
}

function serverTables(all: Table[], name: string): Table[] {
    return all.filter((t) => t.path[0] === "mcp_servers" && t.path[1] === name && t.path.length >= 2);
}

function mainTable(text: string, all: Table[], name: string): string | undefined {
    const t = all.find((t) => t.path.length === 2 && t.path[0] === "mcp_servers" && t.path[1] === name);
    return t ? body(text, t) : undefined;
}

/* A table procode wrote: running procode's script of that name, or at ask's path. */
function writtenByProcode(src: string | undefined, name: string): boolean {
    if (src === undefined) return false;
    const url = stringValue(src, "url");
    if (url !== undefined) return url.endsWith(HTTP_PATH);
    return (firstArg(src) ?? "").replace(/\\/g, "/").endsWith(`/out/mcp/${name}.js`);
}

const BARE = /^[A-Za-z0-9_-]+$/;
const key = (k: string) => (BARE.test(k) ? k : JSON.stringify(k));
const escape = (k: string) => k.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

/* A server written some other way than its own table — under [mcp_servers],
 * as a dotted key or inline — which a table of the same name would clash
 * with; such a server is left alone. */
function definedOtherwise(text: string, all: Table[], name: string): boolean {
    const n = `(?:${escape(name)}|"${escape(name)}"|'${escape(name)}')`;
    for (const t of all) {
        const src = t.path.length === 0 ? text.slice(0, t.end) : body(text, t).replace(/^[^\n]*\n?/, "");
        if (t.path.length === 0 && new RegExp(`^[ \\t]*mcp_servers[ \\t]*(=|\\.[ \\t]*${n}[ \\t]*[.=])`, "m").test(src)) return true;
        if (t.path.length === 1 && t.path[0] === "mcp_servers" && new RegExp(`^[ \\t]*${n}[ \\t]*[.=]`, "m").test(src)) return true;
    }
    return false;
}

/* The table procode writes for a server. */
export function tableFor(s: ServerEntry): string {
    const lines = [`[mcp_servers.${key(s.name)}]`];
    if ("url" in s) {
        lines.push(`url = ${JSON.stringify(s.url)}`);
        if (s.timeout) lines.push(`tool_timeout_sec = ${Math.ceil(s.timeout / 1000)}`);
    } else {
        lines.push(`command = ${JSON.stringify(s.command)}`, `args = [${s.args.map((a) => JSON.stringify(a)).join(", ")}]`);
        const env = Object.entries(s.env);
        if (env.length) lines.push(`env = { ${env.map(([k, v]) => `${key(k)} = ${JSON.stringify(v)}`).join(", ")} }`);
    }
    return lines.join("\n") + "\n";
}

/* The text with our servers' tables written in (in place of the ones of the
 * same name, else at the end), and the tables procode wrote for the servers
 * named in `off` taken out; everything else is kept byte for byte. skipped
 * names the servers left alone because the file defines them otherwise. */
export function withServers(text: string, servers: readonly ServerEntry[], off: readonly string[] = []): { text: string; skipped: string[] } {
    const all = tables(text);
    const cuts: { start: number; end: number; insert: string }[] = [];
    const skipped: string[] = [];
    // A removed table takes the blank lines after it, so no gap is left.
    const blanksAfter = (end: number) => end + (/^(?:[ \t]*\r?\n)*/.exec(text.slice(end))?.[0].length ?? 0);
    for (const name of [...RETIRED, ...off]) {
        if (!writtenByProcode(mainTable(text, all, name), name)) continue;
        for (const t of serverTables(all, name)) cuts.push({ start: t.start, end: blanksAfter(t.end), insert: "" });
    }
    const appended: string[] = [];
    for (const s of servers) {
        if (definedOtherwise(text, all, s.name)) {
            skipped.push(s.name);
            continue;
        }
        const old = serverTables(all, s.name);
        if (old.length === 0) {
            appended.push(tableFor(s));
            continue;
        }
        old.forEach((t, i) => cuts.push(i === 0 ? { start: t.start, end: t.end, insert: tableFor(s) } : { start: t.start, end: blanksAfter(t.end), insert: "" }));
    }
    let out = "";
    let at = 0;
    for (const c of cuts.sort((a, b) => a.start - b.start)) {
        if (c.start < at) continue; // the same table cut twice (retired and off)
        out += text.slice(at, c.start) + c.insert;
        at = c.end;
    }
    out += text.slice(at);
    for (const block of appended) {
        if (out.length && !out.endsWith("\n")) out += "\n";
        if (out.length && !out.endsWith("\n\n")) out += "\n";
        out += block;
    }
    return { text: out, skipped };
}

/* The names, among `names`, whose table in the file procode wrote. */
export function writtenEntries(text: string, names: readonly string[]): string[] {
    const all = tables(text);
    return names.filter((n) => writtenByProcode(mainTable(text, all, n), n));
}

/* Names of our servers whose table procode wrote but which runs something
 * other than this install would — as mcpjson.ts's outdated. */
export function outdated(text: string, servers: readonly ServerEntry[]): string[] {
    const all = tables(text);
    const out: string[] = RETIRED.filter((n) => writtenByProcode(mainTable(text, all, n), n));
    for (const s of servers) {
        const src = mainTable(text, all, s.name);
        if (!writtenByProcode(src, s.name)) continue;
        if ("url" in s) {
            if (stringValue(src!, "url") !== s.url) out.push(s.name);
        } else if (stringValue(src!, "command") !== s.command || firstArg(src!) !== s.args[0]) {
            out.push(s.name);
        }
    }
    return out;
}
