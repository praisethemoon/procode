/* Claude Code's `.mcp.json`: the project's MCP servers, one entry per name.
 * Pure functions over the parsed file, so the rules are testable without
 * VS Code: add or replace our entries and keep everything else, and tell
 * when an entry points at a copy of procode that is no longer installed. */

/* A server Claude Code starts (stdio), or one it connects to (HTTP: ask's,
 * which runs inside VS Code). */
export type ServerEntry =
    | { readonly name: string; readonly command: string; readonly args: string[]; readonly env: Record<string, string> }
    | { readonly name: string; readonly url: string; readonly timeout?: number };

/* The path ask's server answers on; an http entry with it is one procode wrote. */
const HTTP_PATH = "/procode/ask/mcp";

type Json = Record<string, unknown>;

/* Servers procode once wrote under a name it no longer uses: `artifacts`,
 * renamed techdocs. An entry of that name that procode wrote (its script is
 * procode's) is replaced by the new one; one set up by hand is left alone. */
export const RETIRED = ["artifacts"] as const;

function writtenByProcode(e: unknown, name: string): boolean {
    const args = typeof e === "object" && e !== null && Array.isArray((e as Json)["args"]) ? ((e as Json)["args"] as unknown[]) : [];
    const script = typeof args[0] === "string" ? args[0].replace(/\\/g, "/") : "";
    return script.endsWith(`/out/mcp/${name}.js`);
}

/* The file with our servers written in; every other key and server is kept
 * exactly as it was. */
export function withServers(file: Json, servers: readonly ServerEntry[]): Json {
    const existing = file["mcpServers"];
    const mcpServers: Json = typeof existing === "object" && existing !== null && !Array.isArray(existing) ? { ...(existing as Json) } : {};
    for (const name of RETIRED) {
        if (writtenByProcode(mcpServers[name], name)) delete mcpServers[name];
    }
    for (const s of servers) {
        mcpServers[s.name] =
            "url" in s ? { type: "http", url: s.url, ...(s.timeout ? { timeout: s.timeout } : {}) } : { command: s.command, args: s.args, env: s.env };
    }
    return { ...file, mcpServers };
}

/* Names of our servers whose entry was written by procode but runs something
 * other than what this install would run — typically the previous version's
 * folder, which an update removed. An entry someone pointed elsewhere by hand
 * (the repository's own kb-mcp, say) is theirs and never flagged, and a
 * server the file does not mention was never set up. */
export function outdated(file: Json, servers: readonly ServerEntry[]): string[] {
    const existing = file["mcpServers"];
    if (typeof existing !== "object" || existing === null) {
        return [];
    }
    const out: string[] = RETIRED.filter((name) => writtenByProcode((existing as Json)[name], name));
    for (const s of servers) {
        const e = (existing as Json)[s.name] as Json | undefined;
        if (!e) {
            continue;
        }
        if ("url" in s) {
            // procode's when its path is ask's; stale when this window serves elsewhere.
            const url = typeof e["url"] === "string" ? e["url"] : "";
            if (url.endsWith(HTTP_PATH) && url !== s.url) out.push(s.name);
            continue;
        }
        const args = Array.isArray(e["args"]) ? (e["args"] as unknown[]) : [];
        const script = typeof args[0] === "string" ? args[0].replace(/\\/g, "/") : "";
        if (!script.endsWith(`/out/mcp/${s.name}.js`)) {
            continue; // not one procode wrote
        }
        if (e["command"] !== s.command || args[0] !== s.args[0]) {
            out.push(s.name);
        }
    }
    return out;
}
