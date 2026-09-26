/* The History view's filter and pages, over logs built from real records. */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import { EMPTY_FILTER, HistoryFilter, PAGE_SIZE, fieldCount, isFiltering, query, rangeStart } from "../history";
import { parseLog } from "../model";

const NOW = new Date("2026-09-26T12:00:00Z");

function session(id: string, msg: string, ts: string): string {
    return JSON.stringify({ type: "session_start", id, msg, ts });
}
function end(id: string, ts: string): string {
    return JSON.stringify({ type: "session_end", id, ts });
}
function commit(id: string, session: string | null, file: string, msg: string, ts: string, op = "edit", user = "claude"): string {
    return JSON.stringify({
        type: "commit", id, session, file, op, user, msg, ts,
        old_start: 1, old_lines: 1, new_start: 1, new_lines: 1, eof_nl: true, old_text: ["a"], new_text: ["b"],
    });
}
const log = (...lines: string[]) => parseLog(lines.join("\n") + "\n");

/* S1 (ended, 3 days ago): two commits. S2 (ended, today): a parser fix and a
 * test. L5 outside any session. S3 (active, today): nothing yet. */
const LOG = log(
    session("S1", "old work on the lexer", "2026-09-23T09:00:00Z"),
    commit("L1", "S1", "src/lexer.ts", "tokens for numbers", "2026-09-23T09:01:00Z", "create"),
    commit("L2", "S1", "src/lexer.ts", "tokens for strings", "2026-09-23T09:02:00Z"),
    end("S1", "2026-09-23T09:03:00Z"),
    session("S2", "T-9: fix the parser", "2026-09-26T08:00:00Z"),
    commit("L3", "S2", "src/parser.ts", "fix precedence", "2026-09-26T08:01:00Z"),
    commit("L4", "S2", "test/parser.test.ts", "a test for precedence", "2026-09-26T08:02:00Z", "create", "ana"),
    end("S2", "2026-09-26T08:03:00Z"),
    commit("L5", null, "notes.md", "notes retired", "2026-09-26T09:00:00Z", "delete"),
    session("S3", "T-10: docs", "2026-09-26T10:00:00Z"),
);

const f = (over: Partial<HistoryFilter>): HistoryFilter => ({ ...EMPTY_FILTER, ...over });
const ids = (p: ReturnType<typeof query>) => p.sessions.map((s) => `${s.id ?? "none"}:${s.commits.map((c) => c.id).join(",")}`);
const grouped = (filter: HistoryFilter, page = 0) => query(LOG, filter, { grouped: true, page, now: NOW });

test("with no filter, every session, newest first, commits newest first, the no-session group last", () => {
    assert.deepEqual(ids(grouped(EMPTY_FILTER)), ["S3:", "S2:L4,L3", "S1:L2,L1", "none:L5"]);
    const p = grouped(EMPTY_FILTER);
    assert.deepEqual(p.sessions.map((s) => s.state), ["active", "ended", "ended", "none"]);
    assert.equal(isFiltering(EMPTY_FILTER), false);
    assert.deepEqual(p.users, ["ana", "claude"]);
});

test("a session found by its own words shows its commits; one found through its commits shows only those", () => {
    assert.deepEqual(ids(grouped(f({ text: "parser" }))), ["S2:L4,L3"], "the message names the parser: all of it");
    assert.deepEqual(ids(grouped(f({ text: "strings" }))), ["S1:L2"], "only the commit that says it");
    assert.deepEqual(ids(grouped(f({ text: "lexer.ts" }))), ["S1:L2,L1"], "a file path matches");
    assert.equal(grouped(f({ text: "parser" })).sessions[0].matchedSelf, true);
    assert.equal(grouped(f({ text: "strings" })).sessions[0].matchedSelf, false);
});

test("an id means exactly that one", () => {
    assert.deepEqual(ids(grouped(f({ text: "L1" }))), ["S1:L1"]);
    assert.deepEqual(ids(grouped(f({ text: "s2" }))), ["S2:L4,L3"]);
});

test("the time range narrows commits, and a session with none keeps its start", () => {
    assert.deepEqual(ids(grouped(f({ range: "today" }))), ["S3:", "S2:L4,L3", "none:L5"]);
    assert.deepEqual(ids(grouped(f({ range: "3d" }))), ["S3:", "S2:L4,L3", "none:L5"], "S1 is 3 days and 3 hours old");
    assert.deepEqual(ids(grouped(f({ range: "week" }))), ["S3:", "S2:L4,L3", "S1:L2,L1", "none:L5"]);
    const start = rangeStart("today", NOW)!;
    assert.equal(new Date(start).getHours(), 0, "today starts at local midnight");
    assert.equal(rangeStart("recent", NOW), null);
    assert.equal(rangeStart("week", NOW), NOW.getTime() - 7 * 86_400_000);
});

test("kinds of change, people and session states combine with the text", () => {
    assert.deepEqual(ids(grouped(f({ ops: ["create"] }))), ["S2:L4", "S1:L1"], "an empty session has no created file");
    assert.deepEqual(ids(grouped(f({ users: ["ana"] }))), ["S2:L4"]);
    assert.deepEqual(ids(grouped(f({ states: ["active"] }))), ["S3:"]);
    assert.deepEqual(ids(grouped(f({ states: ["none"] }))), ["none:L5"]);
    assert.deepEqual(ids(grouped(f({ states: ["ended"], ops: ["create"], text: "test" }))), ["S2:L4"]);
    assert.equal(fieldCount(f({ ops: ["edit"], users: ["ana"], states: ["open"] })), 3);
});

test("the raw list is every matching commit, newest first, with its session's state", () => {
    const raw = query(LOG, EMPTY_FILTER, { grouped: false, page: 0, now: NOW });
    assert.deepEqual(raw.commits.map((c) => c.id), ["L5", "L4", "L3", "L2", "L1"]);
    assert.deepEqual(raw.sessions, []);
    const none = query(LOG, f({ states: ["none"] }), { grouped: false, page: 0, now: NOW });
    assert.deepEqual(none.commits.map((c) => c.id), ["L5"]);
    assert.equal(raw.commits[0].region, "line 1");
    assert.equal("newText" in raw.commits[0], false, "a row carries no edit text");
});

test("pages: 25 sessions or 50 commits, clamped, with the total", () => {
    const lines: string[] = [];
    for (let i = 1; i <= 60; i++) {
        lines.push(session(`S${i}`, `task ${i}`, `2026-09-2${i % 5}T10:00:00Z`));
        lines.push(commit(`L${i}`, `S${i}`, "a.ts", `edit ${i}`, `2026-09-2${i % 5}T10:01:00Z`));
        lines.push(end(`S${i}`, `2026-09-2${i % 5}T10:02:00Z`));
    }
    const big = log(...lines);
    const p0 = query(big, EMPTY_FILTER, { grouped: true, page: 0, now: NOW });
    assert.deepEqual([p0.total, p0.pages, p0.sessions.length, p0.pageSize], [60, 3, PAGE_SIZE.grouped, 25]);
    assert.equal(p0.sessions[0].id, "S60");
    const last = query(big, EMPTY_FILTER, { grouped: true, page: 99, now: NOW });
    assert.deepEqual([last.page, last.sessions.length, last.sessions.at(-1)!.id], [2, 10, "S1"]);
    const raw = query(big, EMPTY_FILTER, { grouped: false, page: 1, now: NOW });
    assert.deepEqual([raw.total, raw.pages, raw.commits.length, raw.commits[0].id], [60, 2, 10, "L10"]);
    const none = query(big, f({ text: "nothing like this" }), { grouped: true, page: 5, now: NOW });
    assert.deepEqual([none.total, none.pages, none.page, none.sessions.length], [0, 1, 0, 0]);
});
