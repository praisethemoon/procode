/* Claude Code at user scope: coboard and kb registered once per machine with
 * `claude mcp add-json --scope user`, so every project has them and nothing is
 * written into a repository. A user-scope stdio server is started in the
 * directory Claude Code runs in, which is where each server finds that
 * project's .coboard/ and .kb/.
 *
 * Pure, apart from findClaude's probe of the filesystem: the argv, the shell
 * line offered when there is no CLI, and the signature that tells a current
 * registration from one an update has left pointing at a removed folder. */

import * as path from "node:path";

import { ServerEntry } from "./mcpjson";

/* Claude Code's comments on the board are signed with this name; without it
 * coboard falls back to "agent", which says nothing about who wrote them. */
export const CLAUDE_AUTHOR = "claude";

export function forClaude(s: ServerEntry): ServerEntry {
    return s.name === "coboard" ? { ...s, env: { ...s.env, COBOARD_AUTHOR: CLAUDE_AUTHOR } } : s;
}

export function addArgv(s: ServerEntry): string[] {
    const entry = { type: "stdio", command: s.command, args: s.args, env: s.env };
    return ["mcp", "add-json", "--scope", "user", s.name, JSON.stringify(entry)];
}

/* add-json refuses a name that is already there, so an update removes first. */
export function removeArgv(name: string): string[] {
    return ["mcp", "remove", "--scope", "user", name];
}

/* The same command as a line a person can paste into a POSIX shell. */
export function shellLine(argv: readonly string[]): string {
    const quote = (a: string) => (/^[A-Za-z0-9_./:=-]+$/.test(a) ? a : `'${a.replace(/'/g, `'\\''`)}'`);
    return ["claude", ...argv].map(quote).join(" ");
}

/* What was registered, as one string: equal exactly when the entries are. */
export function signature(servers: readonly ServerEntry[]): string {
    return JSON.stringify(servers.map((s) => [s.name, s.command, s.args, s.env]));
}

/* The `claude` CLI: on PATH first, then where its installers put it. A VS Code
 * started from the Dock on macOS does not inherit the login shell's PATH, so
 * ~/.local/bin is looked at even when PATH does not name it. */
export function findClaude(
    pathVar: string,
    home: string,
    isFile: (p: string) => boolean,
    platform: NodeJS.Platform = process.platform,
): string | null {
    const names = platform === "win32" ? ["claude.exe", "claude.cmd"] : ["claude"];
    const dirs = [
        ...pathVar.split(path.delimiter).filter((d) => d.length > 0),
        path.join(home, ".local", "bin"),
        path.join(home, ".claude", "local"),
        "/opt/homebrew/bin",
        "/usr/local/bin",
    ];
    for (const dir of dirs) {
        for (const name of names) {
            const candidate = path.join(dir, name);
            if (isFile(candidate)) {
                return candidate;
            }
        }
    }
    return null;
}
