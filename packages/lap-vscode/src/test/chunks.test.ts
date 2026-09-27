/* The history's files: chunks named and ordered as lap names them, the
 * single-file log where lap has not written since chunks came, and reads
 * across chunk boundaries that parse exactly as the one file would. */

import * as assert from "node:assert/strict";
import { createHash } from "node:crypto";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { IncrementalLog, firstLine, hasHistory, historyFiles, historyProblem, lineageChunks, missingChunk, ownFiles, parseChunkName, readStream } from "../chunks";
import { consumableBytes, createReader, parseLog, readerFeed } from "../model";

const hash = (line: string) => createHash("sha256").update(line, "utf8").digest("hex");
const rec = (o: object) => JSON.stringify(o) + "\n";
const ts = "2026-09-27T10:00:00Z";
const commit = (id: string, session: string) =>
    rec({ type: "commit", id, session, file: "a.ts", op: "edit", user: "claude",
        old_start: 1, old_lines: 1, new_start: 1, new_lines: 1, eof_nl: true, old_text: ["a"], new_text: ["b"],
        intent: `intent of ${id}`, behavior: `behavior of ${id}`, ts });
const lines = [
    rec({ type: "init", version: 1, ts }),
    rec({ type: "session_start", id: "S1", msg: "T-1: chunks", meta: { ticket: "T-1" }, ts }),
    commit("L1", "S1"),
    commit("L2", "S1"),
    rec({ type: "session_end", id: "S1", ts }),
    rec({ type: "session_start", id: "S2", msg: "T-2: more", meta: {}, ts }),
    commit("L3", "S2"),
];
const whole = lines.join("");

function lapDir(): string {
    return path.join(fs.mkdtempSync(path.join(os.tmpdir(), "lap-chunks-")), ".lap");
}

function writeChunks(dir: string, parts: string[]): void {
    fs.mkdirSync(path.join(dir, "log"), { recursive: true });
    parts.forEach((p, i) => fs.writeFileSync(path.join(dir, "log", `main.${String(i + 1).padStart(6, "0")}.jsonl`), p));
}

test("chunk names parse into lineage and number, and nothing else does", () => {
    assert.deepEqual(parseChunkName("main.000001.jsonl"), { lineage: "main", n: 1 });
    assert.deepEqual(parseChunkName("7c1e9a02d4b8.000042.jsonl"), { lineage: "7c1e9a02d4b8", n: 42 });
    for (const bad of ["main.000000.jsonl", "main.00001.jsonl", "main.000001.jsonl.tmp.12", "log.jsonl", "7C1E9A02D4B8.000001.jsonl", "feature.000001.jsonl"]) {
        assert.equal(parseChunkName(bad), null, bad);
    }
});

test("a lineage's chunks are ordered by number, other lineages left out, and a gap ends them", () => {
    const names = ["main.000002.jsonl", "0123456789ab.000001.jsonl", "main.000001.jsonl", "notes.txt", "main.000004.jsonl"];
    assert.deepEqual(lineageChunks(names, "main"), ["main.000001.jsonl", "main.000002.jsonl"]);
    assert.deepEqual(lineageChunks(names, "0123456789ab"), ["0123456789ab.000001.jsonl"]);
    assert.deepEqual(lineageChunks([], "main"), []);
});

test("a history split over several chunks parses to the same commits and sessions as the single file", () => {
    const dir = lapDir();
    writeChunks(dir, [lines.slice(0, 3).join(""), lines.slice(3, 4).join(""), "", lines.slice(4).join("")]);
    const files = historyFiles(dir);
    assert.equal(files.length, 4);
    const size = files.reduce((n, f) => n + f.size, 0);
    assert.equal(size, Buffer.byteLength(whole));
    const read = readStream(files, 0, size).toString("utf8");
    assert.equal(read, whole);
    assert.deepEqual(parseLog(read, hash), parseLog(whole, hash));
    assert.equal(parseLog(read, hash).commits.length, 3);
});

test("reads span chunk boundaries at any offset", () => {
    const dir = lapDir();
    writeChunks(dir, ["aaaa\nbbbb\n", "cc\n", "dddd\n"]);
    const files = historyFiles(dir);
    assert.equal(readStream(files, 5, 13).toString(), "bbbb\ncc\n");
    assert.equal(readStream(files, 12, 18).toString(), "\ndddd\n");
    assert.equal(readStream(files, 0, 0).length, 0);
});

test("an incremental reader follows the history as it grows into new chunks", () => {
    const dir = lapDir();
    writeChunks(dir, [lines.slice(0, 3).join("")]);
    const reader = createReader(hash);
    let offset = 0;
    const step = () => {
        const files = historyFiles(dir);
        const size = files.reduce((n, f) => n + f.size, 0);
        const buf = readStream(files, offset, size);
        const take = consumableBytes(buf, buf.length);
        readerFeed(reader, buf.subarray(0, take).toString("utf8"));
        offset += take;
    };
    step();
    assert.equal(reader.log.commits.length, 1);
    // the open chunk grows with a torn line, then is sealed by the next chunk
    fs.appendFileSync(path.join(dir, "log", "main.000001.jsonl"), lines[3]);
    fs.writeFileSync(path.join(dir, "log", "main.000002.jsonl"), lines.slice(4).join("") + '{"type":"comm');
    step();
    assert.equal(reader.log.commits.length, 3);
    assert.deepEqual(reader.log, parseLog(whole, hash));
});

test("the single-file log is read when there is no chunk, and chunks win over it", () => {
    const dir = lapDir();
    assert.equal(hasHistory(dir), false);
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, "log.jsonl"), whole);
    assert.equal(hasHistory(dir), true);
    const legacy = historyFiles(dir);
    assert.deepEqual(legacy.map((f) => path.basename(f.path)), ["log.jsonl"]);
    assert.equal(readStream(legacy, 0, legacy[0].size).toString(), whole);
    writeChunks(dir, [lines.slice(0, 2).join("")]);
    assert.deepEqual(historyFiles(dir).map((f) => path.basename(f.path)), ["main.000001.jsonl"]);
});

test("a chunk missing from the middle is named, as lap names it, never read around", () => {
    assert.equal(missingChunk(["main.000001.jsonl", "main.000002.jsonl"], "main"), null);
    assert.equal(missingChunk(["main.000001.jsonl", "main.000003.jsonl"], "main"), "main.000002.jsonl");
    assert.equal(missingChunk(["main.000002.jsonl"], "main"), "main.000001.jsonl");
    assert.equal(missingChunk([], "main"), null);
    const dir = lapDir();
    writeChunks(dir, [lines.slice(0, 2).join(""), lines.slice(2, 3).join(""), lines.slice(3).join("")]);
    assert.equal(historyProblem(dir), null);
    fs.rmSync(path.join(dir, "log", "main.000002.jsonl"));
    assert.equal(historyProblem(dir), `history chunk main.000002.jsonl is missing from ${path.join(dir, "log")}`);
    // a branch folder: its own lineage is checked too
    fs.writeFileSync(path.join(dir, "log", "main.000002.jsonl"), lines.slice(2, 3).join(""));
    fs.writeFileSync(path.join(dir, "lineage"), "0123456789ab\n");
    fs.writeFileSync(path.join(dir, "log", "0123456789ab.000002.jsonl"), "");
    assert.match(historyProblem(dir) ?? "", /0123456789ab\.000001\.jsonl is missing/);
});

test("a branch is read as it grows: only new whole lines, from its own chunks, again from the start only when it shrank or moved", () => {
    const dir = lapDir();
    const id = "0123456789ab";
    const own = (n: number) => path.join(dir, "log", `${id}.00000${n}.jsonl`);
    const branch = rec({ type: "branch", id, name: "busy", parent: "main", base: "b", base_chunk: 1, ts, prev: "b" });
    fs.mkdirSync(path.join(dir, "log"), { recursive: true });
    fs.writeFileSync(path.join(dir, "log", "main.000001.jsonl"), lines.join("")); // main's: not read
    fs.writeFileSync(own(1), branch + rec({ type: "session_start", id: "S2", msg: "busy work", meta: {}, ts }));
    assert.deepEqual(ownFiles(dir, id).map((f) => path.basename(f.path)), [`${id}.000001.jsonl`]);
    const inc = new IncrementalLog(hash);
    let log = inc.update(ownFiles(dir, id))!;
    assert.equal(log.sessions.length, 1);
    const first = inc.read;
    assert.equal(first, fs.statSync(own(1)).size, "only its own chunk was read");
    // grows, with a line still being written
    fs.appendFileSync(own(1), commit("L9", "S2") + '{"type":"commit","partial');
    log = inc.update(ownFiles(dir, id))!;
    assert.deepEqual(log.commits.map((c) => c.id), ["L9"]);
    assert.equal(inc.read, first + commit("L9", "S2").length, "the unfinished line waits");
    // a new chunk: read on from where it was
    fs.writeFileSync(own(1), branch + rec({ type: "session_start", id: "S2", msg: "busy work", meta: {}, ts }) + commit("L9", "S2"));
    fs.writeFileSync(own(2), commit("L10", "S2"));
    log = inc.update(ownFiles(dir, id))!;
    assert.deepEqual(log.commits.map((c) => c.id), ["L9", "L10"]);
    // shrank: read again from the start
    fs.rmSync(own(2));
    fs.writeFileSync(own(1), branch);
    log = inc.update(ownFiles(dir, id))!;
    assert.equal(log.commits.length, 0);
    assert.equal(inc.update([]), null);
});

test("a chunk's first line is read however long it is, not the whole chunk", () => {
    const f = path.join(fs.mkdtempSync(path.join(os.tmpdir(), "lap-first-")), "c.jsonl");
    const long = "x".repeat(200_000);
    fs.writeFileSync(f, `${long}\nsecond\n`);
    assert.equal(firstLine(f), long);
    fs.writeFileSync(f, "no newline");
    assert.equal(firstLine(f), "no newline");
});
