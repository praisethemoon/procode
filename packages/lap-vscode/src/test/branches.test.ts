/* Branches in Lap History: the rows built from `lap branch list --json`, a
 * branch log cut to its own part, what lap merge writes read by the model
 * (from links, adopted sessions that never become active, merge records),
 * and a branch's files found in its folder or in its parent's chunks. */

import * as assert from "node:assert/strict";
import { createHash } from "node:crypto";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { branchView, lapBin, lapFailure, ownPart, parseBranchList } from "../branches";
import { folderFiles, lineageFiles, readStream } from "../chunks";
import { row } from "../history";
import { parseLog } from "../model";

const hash = (line: string) => createHash("sha256").update(line, "utf8").digest("hex");
const rec = (o: object) => JSON.stringify(o) + "\n";
const ts = "2026-09-27T10:00:00Z";
const commit = (id: string, session: string | null, extra: object = {}) =>
    rec({ type: "commit", id, session, file: "a.ts", op: "edit", user: "claude",
        old_start: 1, old_lines: 1, new_start: 1, new_lines: 1, eof_nl: true, old_text: ["a"], new_text: ["b"],
        intent: `intent of ${id}`, behavior: `behavior of ${id}`, ...extra, ts });

const LIST = {
    ok: true,
    self: null,
    branches: [
        { id: "0123456789ab", name: "busy", state: "active", present: true, path: "/w/busy", base: "b", started: ts, since_base: 3, since_merge: 3, merged: null, stopped: [] },
        { id: "1123456789ab", name: "done", state: "merged", present: false, path: "/w/done", base: "b", started: ts, since_base: 2, since_merge: 0, merged: "h", stopped: [] },
        { id: "2123456789ab", name: "half", state: "partly merged", present: true, path: "/w/half", base: "b", started: ts, since_base: 4, since_merge: 1, merged: "h", stopped: ["src/x.ts"] },
        { id: "3123456789ab", name: "lost", state: "missing", present: false, path: "/w/lost", base: "b", started: ts, since_base: null, since_merge: null, merged: null, stopped: [] },
    ],
};

test("branch list JSON becomes rows, with a stopped file's commit from the merge record", () => {
    const log = parseLog(
        rec({ type: "init", version: 1, ts }) +
            rec({ type: "merge", branch: "2123456789ab", name: "half", head: "h", adopted: 3, left: 1, stopped: [{ file: "src/x.ts", at: "abcdef0123" }], user: "u", ts }),
        hash,
    );
    const rows = parseBranchList(LIST, log);
    assert.deepEqual(rows.map((r) => `${r.name}:${r.state}`), ["busy:active", "done:merged", "half:partly merged", "lost:missing"]);
    assert.equal(rows[0].sinceBase, 3);
    assert.equal(rows[3].sinceBase, null, "an unreadable branch has no counts");
    assert.deepEqual(rows[2].stopped, [{ file: "src/x.ts", at: "abcdef0123" }]);
    assert.deepEqual(parseBranchList(LIST)[2].stopped, [{ file: "src/x.ts", at: null }], "no log: the file without its commit");
    assert.equal(rows[1].present, false);
});

test("anything that is not branch list's shape reads as no branches", () => {
    assert.deepEqual(parseBranchList(null), []);
    assert.deepEqual(parseBranchList({ ok: false, error: "unknown_command" }), []);
    assert.deepEqual(parseBranchList({ ok: true, branches: [{ name: "no id" }, 7] }), []);
    assert.equal(parseBranchList({ ok: true, branches: [{ id: "x", name: "y", state: "odd" }] })[0].state, "active");
});

/* A branch folder's log: the parent's part, the branch record, its own. */
const BRANCH_LOG =
    rec({ type: "init", version: 1, ts }) +
    rec({ type: "session_start", id: "S1", msg: "the parent's work", meta: {}, ts }) +
    commit("L1", "S1") +
    rec({ type: "branch", id: "0123456789ab", name: "busy", parent: "main", base: "b", base_chunk: 1, ts, prev: "b" }) +
    rec({ type: "session_start", id: "S2", msg: "the branch's work", meta: {}, ts }) +
    commit("L2", "S2") +
    commit("L3", null);

test("a branch log is cut to its own part, and its view lists only its sessions", () => {
    const log = parseLog(BRANCH_LOG, hash);
    assert.equal(log.branchAt, 3);
    assert.equal(log.branchName, "busy");
    const own = ownPart(log);
    assert.deepEqual(own.commits.map((c) => c.id), ["L2", "L3"]);
    assert.deepEqual(own.sessions.map((s) => s.id), ["S2"]);
    assert.deepEqual(own.noSession.map((c) => c.id), ["L3"]);
    const v = branchView(parseBranchList(LIST)[0], log, new Date(ts));
    assert.ok(v.sessions.some((s) => s.id === "S2" && s.commits.map((c) => c.id).join() === "L2"));
    assert.ok(!v.sessions.some((s) => s.id === "S1"), "the parent's sessions are not the branch's");
    assert.deepEqual(branchView(parseBranchList(LIST)[3], null, new Date(ts)).sessions, []);
    assert.equal(ownPart(parseLog(rec({ type: "init", version: 1, ts }) + commit("L1", null), hash)).commits.length, 1, "a main log is its own");
});

test("the model reads from links, keeps adopted sessions out of the active one, and collects merges", () => {
    const log = parseLog(
        rec({ type: "session_start", id: "S1", msg: "mine", meta: {}, ts }) +
            rec({ type: "session_start", id: "S2", msg: "theirs", meta: { ticket: "T-7" }, from: "aa", ts }) +
            commit("L1", "S2", { from: "cc" }) +
            rec({ type: "session_end", id: "S2", from: "bb", ts }) +
            rec({ type: "merge", branch: "0123456789ab", name: "busy", head: "cc", adopted: 1, left: 0, stopped: [], user: "u", ts }),
        hash,
    );
    assert.equal(log.activeSessionId, "S1", "an adopted start and end leave the folder's own session active");
    assert.equal(log.sessions[1].from, "aa");
    assert.equal(log.sessions[1].endTs, ts);
    assert.equal(log.commits[0].from, "cc");
    assert.equal(row(log.commits[0]).from, "cc");
    assert.equal(log.merges.length, 1);
    assert.equal(log.merges[0].head, "cc");
    const plain = parseLog(commit("L1", null), hash);
    assert.equal(plain.commits[0].from, null);
    assert.equal(parseLog(BRANCH_LOG.split("\n").slice(0, 4).join("\n") + "\n", hash).activeSessionId, null, "a branch starts with no session open");
});

test("a branch's files come from its folder, or from its chunks in the parent", () => {
    const lap = path.join(fs.mkdtempSync(path.join(os.tmpdir(), "lap-branches-")), ".lap");
    fs.mkdirSync(path.join(lap, "log"), { recursive: true });
    const lines = BRANCH_LOG.split("\n").filter(Boolean).map((l) => l + "\n");
    fs.writeFileSync(path.join(lap, "log", "main.000001.jsonl"), lines.slice(0, 3).join(""));
    fs.writeFileSync(path.join(lap, "log", "main.000002.jsonl"), "after the base\n");
    fs.writeFileSync(path.join(lap, "log", "0123456789ab.000001.jsonl"), lines.slice(3).join(""));
    const files = lineageFiles(lap, "0123456789ab");
    assert.deepEqual(files.map((f) => path.basename(f.path)), ["main.000001.jsonl", "0123456789ab.000001.jsonl"]);
    const size = files.reduce((n, f) => n + f.size, 0);
    assert.equal(readStream(files, 0, size).toString(), BRANCH_LOG);
    assert.deepEqual(lineageFiles(lap, "ffffffffffff"), []);
    // as the branch folder itself: .lap/lineage names it
    assert.deepEqual(folderFiles(lap).map((f) => path.basename(f.path)), ["main.000001.jsonl", "main.000002.jsonl"]);
    fs.writeFileSync(path.join(lap, "lineage"), "0123456789ab\n");
    assert.deepEqual(folderFiles(lap).map((f) => path.basename(f.path)), ["main.000001.jsonl", "0123456789ab.000001.jsonl"]);
    // a first chunk that is not the branch's record is refused
    fs.writeFileSync(path.join(lap, "log", "0123456789ab.000001.jsonl"), commit("L9", null));
    assert.deepEqual(lineageFiles(lap, "0123456789ab"), []);
});

/* main, branch busy from it, and branch sub from busy after busy's first
 * chunk: sub's history is main to busy's base, busy to sub's base, then
 * sub's own. */
const NESTED_SUB =
    rec({ type: "branch", id: "ba9876543210", name: "sub", parent: "0123456789ab", base: "c", base_chunk: 1, ts, prev: "c" }) +
    rec({ type: "session_start", id: "S3", msg: "the nested branch's work", meta: {}, ts }) +
    commit("L4", "S3");

test("a branch of a branch reads its history through the branch between, and shows only its own part", () => {
    const lap = path.join(fs.mkdtempSync(path.join(os.tmpdir(), "lap-branches-")), ".lap");
    fs.mkdirSync(path.join(lap, "log"), { recursive: true });
    const lines = BRANCH_LOG.split("\n").filter(Boolean).map((l) => l + "\n");
    fs.writeFileSync(path.join(lap, "log", "main.000001.jsonl"), lines.slice(0, 3).join(""));
    fs.writeFileSync(path.join(lap, "log", "main.000002.jsonl"), "after busy's base\n");
    fs.writeFileSync(path.join(lap, "log", "0123456789ab.000001.jsonl"), lines.slice(3).join(""));
    fs.writeFileSync(path.join(lap, "log", "0123456789ab.000002.jsonl"), "after sub's base\n");
    fs.writeFileSync(path.join(lap, "log", "ba9876543210.000001.jsonl"), NESTED_SUB);
    const files = lineageFiles(lap, "ba9876543210");
    assert.deepEqual(files.map((f) => path.basename(f.path)), ["main.000001.jsonl", "0123456789ab.000001.jsonl", "ba9876543210.000001.jsonl"]);
    const text = readStream(files, 0, files.reduce((n, f) => n + f.size, 0)).toString();
    assert.equal(text, BRANCH_LOG + NESTED_SUB);
    const log = parseLog(text, hash);
    assert.equal(log.branchAt, 7, "its own part starts at its own branch record, not busy's");
    assert.equal(log.branchName, "sub");
    assert.deepEqual(ownPart(log).commits.map((c) => c.id), ["L4"]);
    // busy's base chunk gone: no history rather than a wrong one
    fs.rmSync(path.join(lap, "log", "0123456789ab.000001.jsonl"));
    assert.deepEqual(lineageFiles(lap, "ba9876543210"), []);
});

test("branch list rows say which branch a nested one started from", () => {
    const rows = parseBranchList({
        ok: true,
        self: null,
        branches: [
            { id: "0123456789ab", name: "busy", state: "active", present: true, path: "/w/busy", since_base: 1, since_merge: 1, stopped: [], via: null },
            { id: "ba9876543210", name: "sub", state: "active", present: true, path: "/w/sub", since_base: 1, since_merge: 1, stopped: [], via: "0123456789ab" },
        ],
    });
    assert.deepEqual(rows.map((r) => `${r.name}:${r.via}`), ["busy:null", "sub:0123456789ab"]);
    assert.equal(parseBranchList(LIST)[0].via, null, "an older lap without via: this folder's own");
});

test("a failing lap is reported in its own words, and a missing one says it could not run", async () => {
    const { execFile } = await import("node:child_process");
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "lapfail-"));
    const standIn = path.join(dir, "lap");
    fs.writeFileSync(standIn, '#!/bin/sh\necho \'{"ok":false,"error":"newer_history","message":"this history needs a newer lap"}\'\nexit 1\n', { mode: 0o755 });
    const run = (bin: string) =>
        new Promise<string | null>((done) => execFile(bin, ["branch", "list", "--json"], (err, stdout) => done(lapFailure(err, String(stdout)))));
    assert.equal(await run(standIn), "this history needs a newer lap");
    assert.match((await run(path.join(dir, "no-such-lap"))) ?? "", /^lap could not run: .*ENOENT/);
    assert.equal(lapFailure(null, '{"ok":true,"branches":[]}'), null);
    assert.equal(lapFailure(null, ""), "lap printed no answer");
});

test("the lap path is Lap History's setting, else the Board's, else lap", () => {
    assert.equal(lapBin("/opt/lap", "/usr/bin/lap"), "/opt/lap");
    assert.equal(lapBin("  ", "/usr/bin/lap"), "/usr/bin/lap");
    assert.equal(lapBin(undefined, undefined), "lap");
    assert.equal(lapBin("", ""), "lap");
});
