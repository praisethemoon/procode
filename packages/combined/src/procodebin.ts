/* ~/.procode/bin: the one place lap and kb are found whatever the install.
 *
 * A platform package carries the CLIs in its bin/, a folder whose name
 * changes with every version; the skills Claude reads, and the .mcp.json it
 * starts servers from, need a path that does not. So on activation each CLI
 * is placed in ~/.procode/bin: the package's own, copied; or, when a setting
 * names the user's own build (or there is only one on PATH), a link to it,
 * so the path the skills name works for them too. A copy is written only
 * when the bytes differ, through a temporary file and a rename, so a lap
 * that is running is never half-written; a file that cannot be replaced
 * (Windows locks a running .exe) is left for the next activation.
 * Windows gets copies rather than links, which need privileges there.
 *
 * PROCODE_BIN_DIR moves the folder, for tests that must not touch the real
 * one. */

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

export function procodeBinDir(): string {
    return process.env["PROCODE_BIN_DIR"] || path.join(os.homedir(), ".procode", "bin");
}

/* A CLI's file name on this platform. */
export function exeName(cli: string, platform: NodeJS.Platform = process.platform): string {
    return platform === "win32" ? `${cli}.exe` : cli;
}

/* Where ~/.procode/bin's copy of a CLI comes from: a setting the user set
 * (their own build, linked), else the package's bin/ (copied), else one on
 * PATH (linked). Each argument is a file already resolved, or null. */
export type Source = { readonly kind: "copy" | "link"; readonly file: string } | null;

export function sourceOf(fromSetting: string | null, bundled: string | null, onPath: string | null): Source {
    if (fromSetting) return { kind: "link", file: fromSetting };
    if (bundled) return { kind: "copy", file: bundled };
    if (onPath) return { kind: "link", file: onPath };
    return null;
}

export type Placed = "placed" | "current" | "busy";

function sameBytes(a: string, b: string): boolean {
    try {
        if (fs.lstatSync(b).isSymbolicLink()) return false;
        if (fs.statSync(a).size !== fs.statSync(b).size) return false;
        return fs.readFileSync(a).equals(fs.readFileSync(b));
    } catch {
        return false;
    }
}

function linksTo(link: string, target: string): boolean {
    try {
        return fs.lstatSync(link).isSymbolicLink() && fs.readlinkSync(link) === target;
    } catch {
        return false;
    }
}

/* Puts `source` at `to`: "current" when it already is, "busy" when `to`
 * could not be replaced, "placed" otherwise. `rename` is fs.renameSync but
 * for tests, which cannot hold a Windows file open. */
export function place(
    source: NonNullable<Source>,
    to: string,
    platform: NodeJS.Platform = process.platform,
    rename: (from: string, to: string) => void = fs.renameSync,
): Placed {
    const kind = platform === "win32" ? "copy" : source.kind;
    if (kind === "link" ? linksTo(to, source.file) : sameBytes(source.file, to)) return "current";
    fs.mkdirSync(path.dirname(to), { recursive: true });
    const tmp = `${to}.${process.pid}.new`;
    fs.rmSync(tmp, { force: true });
    if (kind === "link") {
        fs.symlinkSync(source.file, tmp);
    } else {
        fs.copyFileSync(source.file, tmp);
        fs.chmodSync(tmp, 0o755);
    }
    try {
        rename(tmp, to);
        return "placed";
    } catch (e) {
        fs.rmSync(tmp, { force: true });
        const code = (e as NodeJS.ErrnoException).code;
        if (code === "EBUSY" || code === "EPERM" || code === "EACCES") return "busy";
        throw e;
    }
}

/* The command to run a CLI: the setting the user set, else ~/.procode/bin's
 * copy when there is one, else the bare name for PATH. */
export function commandFor(cli: string, fromSetting: string | undefined, exists: (p: string) => boolean): string {
    if (fromSetting) return fromSetting;
    const own = path.join(procodeBinDir(), exeName(cli));
    return exists(own) ? own : cli;
}
