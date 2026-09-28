/* The two CLIs procode's parts run, found the way the parts find them: the
 * command their setting names (Knowledge › Cli Path for kb, Board › Lap Path
 * for lap), else ~/.procode/bin (procodebin.ts), else a bare name resolved
 * through PATH. A platform package puts its own in ~/.procode/bin; the
 * package for every platform has none, and the user builds them, so the one
 * thing it can do is say, once, when one cannot be found and how to fix
 * that. Pure apart from the probe it is given. */

import * as path from "node:path";

export interface Cli {
    readonly name: "kb" | "lap";
    /* The setting that names it, as the Settings UI shows it. */
    readonly setting: string;
    readonly settingId: string;
    /* What in procode needs it. */
    readonly usedBy: string;
    /* How to get it, from the repository. */
    readonly build: string;
}

export const CLIS: readonly Cli[] = [
    {
        name: "kb",
        setting: "Knowledge › Cli Path",
        settingId: "knowledge.cliPath",
        usedBy: "Knowledge and the kb MCP server",
        build: "make -C cli/kb-cli && make -C cli/kb-cli install",
    },
    {
        name: "lap",
        setting: "Board › Lap Path",
        settingId: "coboard.lapPath",
        usedBy: "the Board's lap sessions and the coboard MCP server",
        build: "make -C cli/lap-cli && make -C cli/lap-cli install",
    },
];

/* The file a command runs: itself when it names a path, otherwise the first
 * match on PATH (with Windows' executable extensions). Null when nothing is
 * there. */
export function resolveCli(
    command: string,
    pathVar: string,
    isFile: (p: string) => boolean,
    platform: NodeJS.Platform = process.platform,
): string | null {
    const c = command.trim();
    if (c === "") return null;
    const exts = platform === "win32" ? ["", ".exe", ".cmd"] : [""];
    if (c.includes("/") || c.includes("\\")) {
        return exts.map((e) => c + e).find(isFile) ?? null;
    }
    for (const dir of pathVar.split(path.delimiter).filter((d) => d.length > 0)) {
        for (const e of exts) {
            const candidate = path.join(dir, c + e);
            if (isFile(candidate)) return candidate;
        }
    }
    return null;
}

export function missingMessage(cli: Cli, command: string): string {
    return (
        `procode: "${command}" was not found, so ${cli.usedBy} cannot run. ` +
        `Build it from the lap repository (${cli.build}) so it is on PATH, or set ${cli.setting} to its full path.`
    );
}

/* The CLI an MCP server runs, and whether it can start without it: kb's
 * server is nothing but kb's tools; coboard's board works without lap, whose
 * sessions are one tool among many. Null for a server that runs no CLI. */
export function serverNeeds(server: string): { readonly cli: Cli["name"]; readonly required: boolean } | null {
    if (server === "kb") return { cli: "kb", required: true };
    if (server === "coboard") return { cli: "lap", required: false };
    return null;
}
