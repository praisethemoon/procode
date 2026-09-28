/* The prebuilt CLIs a platform package carries, and what each binary is.
 *
 * A package for one vsce target carries lap and kb built for that target,
 * and nothing is worse than one that installs and cannot run: so each
 * binary's header is read, and its OS and architecture must be the
 * target's. Only the three formats the targets use are known: ELF (linux,
 * alpine), Mach-O thin or universal (darwin), PE (win32). */

import * as fs from "node:fs";
import * as path from "node:path";

export const CLIS = ["lap", "kb"];

/* vsce's targets, each as the OS its binaries are for and their arch. */
export const TARGETS = {
    "darwin-arm64": { os: "darwin", arch: "arm64" },
    "darwin-x64": { os: "darwin", arch: "x64" },
    "linux-arm64": { os: "linux", arch: "arm64" },
    "linux-x64": { os: "linux", arch: "x64" },
    "linux-armhf": { os: "linux", arch: "arm" },
    "alpine-arm64": { os: "linux", arch: "arm64" },
    "alpine-x64": { os: "linux", arch: "x64" },
    "win32-arm64": { os: "win32", arch: "arm64" },
    "win32-x64": { os: "win32", arch: "x64" },
};

/* A CLI's file name for a target. */
export const exe = (cli, target) => (target.startsWith("win32-") ? `${cli}.exe` : cli);

const ELF_MACHINE = { 0x3e: "x64", 0xb7: "arm64", 0x28: "arm" };
const MACHO_CPU = { 0x01000007: "x64", 0x0100000c: "arm64" };
const PE_MACHINE = { 0x8664: "x64", 0xaa64: "arm64" };

/* { os, archs } for an executable's first bytes, or null when it is none of
 * the three formats. A universal Mach-O lists every arch it holds. */
export function identify(buf) {
    if (buf.length >= 20 && buf.readUInt32BE(0) === 0x7f454c46) {
        const little = buf[5] === 1;
        const machine = little ? buf.readUInt16LE(18) : buf.readUInt16BE(18);
        return { os: "linux", archs: [ELF_MACHINE[machine] ?? `elf:${machine}`] };
    }
    if (buf.length >= 8 && buf.readUInt32LE(0) === 0xfeedfacf) {
        const cpu = buf.readUInt32LE(4);
        return { os: "darwin", archs: [MACHO_CPU[cpu] ?? `macho:${cpu}`] };
    }
    if (buf.length >= 8 && buf.readUInt32BE(0) === 0xcafebabe) {
        const n = buf.readUInt32BE(4);
        const archs = [];
        for (let i = 0; i < n && 8 + i * 20 + 4 <= buf.length; i++) {
            const cpu = buf.readUInt32BE(8 + i * 20);
            archs.push(MACHO_CPU[cpu] ?? `macho:${cpu}`);
        }
        return { os: "darwin", archs };
    }
    if (buf.length >= 64 && buf.readUInt16LE(0) === 0x5a4d) {
        const pe = buf.readUInt32LE(60);
        if (pe + 6 <= buf.length && buf.readUInt32LE(pe) === 0x00004550) {
            const machine = buf.readUInt16LE(pe + 4);
            return { os: "win32", archs: [PE_MACHINE[machine] ?? `pe:${machine}`] };
        }
    }
    return null;
}

/* The CLIs in `dir` for `target`, as { cli, file } with the file's full
 * path; throws naming every problem when one is missing or built for
 * another platform. */
export function prebuilt(dir, target) {
    const want = TARGETS[target];
    if (!want) throw new Error(`unknown target ${target}; one of ${Object.keys(TARGETS).join(", ")}`);
    const problems = [];
    const found = [];
    for (const cli of CLIS) {
        const file = path.join(dir, exe(cli, target));
        if (!fs.existsSync(file)) {
            problems.push(`${file} is missing`);
            continue;
        }
        const head = Buffer.alloc(4096);
        const fd = fs.openSync(file, "r");
        const read = fs.readSync(fd, head, 0, head.length, 0);
        fs.closeSync(fd);
        const is = identify(head.subarray(0, read));
        if (!is) problems.push(`${file} is not an executable procode knows`);
        else if (is.os !== want.os || !is.archs.includes(want.arch)) {
            problems.push(`${file} is for ${is.os}-${is.archs.join("+")}, not ${target}`);
        } else found.push({ cli, file });
    }
    if (problems.length > 0) throw new Error(`the CLIs for ${target}:\n  ${problems.join("\n  ")}`);
    return found;
}
