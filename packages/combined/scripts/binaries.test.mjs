/* What a binary's header says it is, and which prebuilt CLIs a target accepts. */

import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";
import { identify, prebuilt } from "./binaries.mjs";

/* Just enough of each format's header for identify(). */
function elf(machine) {
    const b = Buffer.alloc(64);
    b.writeUInt32BE(0x7f454c46, 0);
    b[5] = 1; // little-endian
    b.writeUInt16LE(machine, 18);
    return b;
}
function macho(cpu) {
    const b = Buffer.alloc(32);
    b.writeUInt32LE(0xfeedfacf, 0);
    b.writeUInt32LE(cpu, 4);
    return b;
}
function universal(...cpus) {
    const b = Buffer.alloc(8 + cpus.length * 20);
    b.writeUInt32BE(0xcafebabe, 0);
    b.writeUInt32BE(cpus.length, 4);
    cpus.forEach((cpu, i) => b.writeUInt32BE(cpu, 8 + i * 20));
    return b;
}
function pe(machine) {
    const b = Buffer.alloc(256);
    b.writeUInt16LE(0x5a4d, 0);
    b.writeUInt32LE(128, 60);
    b.writeUInt32LE(0x00004550, 128);
    b.writeUInt16LE(machine, 132);
    return b;
}

test("each format's OS and arch are read from its header", () => {
    assert.deepEqual(identify(elf(0x3e)), { os: "linux", archs: ["x64"] });
    assert.deepEqual(identify(elf(0xb7)), { os: "linux", archs: ["arm64"] });
    assert.deepEqual(identify(macho(0x0100000c)), { os: "darwin", archs: ["arm64"] });
    assert.deepEqual(identify(universal(0x01000007, 0x0100000c)), { os: "darwin", archs: ["x64", "arm64"] });
    assert.deepEqual(identify(pe(0x8664)), { os: "win32", archs: ["x64"] });
    assert.deepEqual(identify(pe(0xaa64)), { os: "win32", archs: ["arm64"] });
    assert.equal(identify(Buffer.from("#!/bin/sh\necho lap\n")), null);
});

function dirWith(files) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "procode-bin-"));
    for (const [name, bytes] of Object.entries(files)) fs.writeFileSync(path.join(dir, name), bytes);
    return dir;
}

test("a target takes CLIs built for it, a universal Mach-O for either darwin arch, and .exe names on win32", () => {
    assert.deepEqual(prebuilt(dirWith({ lap: elf(0x3e), kb: elf(0x3e) }), "linux-x64").map((c) => c.cli), ["lap", "kb"]);
    assert.deepEqual(prebuilt(dirWith({ lap: elf(0xb7), kb: elf(0xb7) }), "alpine-arm64").map((c) => c.cli), ["lap", "kb"]);
    const fat = dirWith({ lap: universal(0x01000007, 0x0100000c), kb: universal(0x01000007, 0x0100000c) });
    assert.equal(prebuilt(fat, "darwin-x64").length, 2);
    assert.equal(prebuilt(fat, "darwin-arm64").length, 2);
    assert.equal(prebuilt(dirWith({ "lap.exe": pe(0xaa64), "kb.exe": pe(0xaa64) }), "win32-arm64").length, 2);
});

test("a missing CLI, another OS's or another arch's binary, and an unknown target are refused", () => {
    assert.throws(() => prebuilt(dirWith({ lap: elf(0x3e) }), "linux-x64"), /kb is missing/);
    assert.throws(() => prebuilt(dirWith({ lap: macho(0x0100000c), kb: elf(0x3e) }), "linux-x64"), /lap is for darwin-arm64, not linux-x64/);
    assert.throws(() => prebuilt(dirWith({ lap: elf(0xb7), kb: elf(0xb7) }), "linux-x64"), /is for linux-arm64, not linux-x64/);
    assert.throws(() => prebuilt(dirWith({ lap: elf(0x3e), kb: elf(0x3e) }), "win32-x64"), /lap\.exe is missing/);
    assert.throws(() => prebuilt(dirWith({}), "beos-x64"), /unknown target beos-x64/);
});
