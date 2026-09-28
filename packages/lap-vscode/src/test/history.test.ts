/* The History view's filter and pages, over logs built from real records. */

import * as assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { test } from "node:test";

import { EMPTY_FILTER, HistoryFilter, PAGE_SIZE, endSummaryParts, fieldCount, isFiltering, pageOf, query, rangeStart, sessionLine, treeRowKey } from "../history";
import { parseLog } from "../model";

const sha = (line: string) => createHash("sha256").update(line, "utf8").digest("hex");

const NOW = new Date("2026-09-26T12:00:00Z");

function session(id: string, msg: string, ts: string): string {
    return JSON.stringify({ type: "session_start", id, msg, ts });
}
function end(id: string, ts: string): string {
    return JSON.stringify({ type: "session_end", id, ts });
}
function commit(
    id: string, session: string | null, file: string, intent: string, ts: string, op = "edit", user = "claude",
    behavior = `step ${id.slice(1)}`, forced = false,
): string {
    return JSON.stringify({
        type: "commit", id, session, file, op, user,
        old_start: 1, old_lines: 1, new_start: 1, new_lines: 1, eof_nl: true, old_text: ["a"], new_text: ["b"],
        intent, behavior, ...(forced ? { forced: true } : {}), ts,
    });
}
const log = (...lines: string[]) => parseLog(lines.join("\n") + "\n", sha);

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

/* A filter over all time unless a test says otherwise: most tests are about
 * matching, and the default range is its own test below. */
const f = (over: Partial<HistoryFilter>): HistoryFilter => ({ ...EMPTY_FILTER, range: "all", ...over });
const ids = (p: ReturnType<typeof query>) => p.sessions.map((s) => `${s.id ?? "none"}:${s.commits.map((c) => c.id).join(",")}`);
const grouped = (filter: HistoryFilter, page = 0) => query(LOG, filter, { grouped: true, page, now: NOW });

const ALL = f({});

test("All is every session, newest first, commits newest first, the no-session group last", () => {
    assert.deepEqual(ids(grouped(ALL)), ["S3:", "S2:L4,L3", "S1:L2,L1", "none:L5"]);
    const p = grouped(ALL);
    assert.deepEqual(p.sessions.map((s) => s.state), ["active", "ended", "ended", "none"]);
    assert.equal(isFiltering(EMPTY_FILTER), false, "Most recent is the default, not a filter");
    assert.equal(isFiltering(ALL), true);
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
    assert.equal(rangeStart("all", NOW), null);
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
    const raw = query(LOG, ALL, { grouped: false, page: 0, now: NOW });
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
    const p0 = query(big, f({ range: "all" }), { grouped: true, page: 0, now: NOW });
    assert.deepEqual([p0.total, p0.pages, p0.sessions.length, p0.pageSize], [60, 3, PAGE_SIZE.grouped, 25]);
    assert.equal(p0.sessions[0].id, "S60");
    const last = query(big, f({ range: "all" }), { grouped: true, page: 99, now: NOW });
    assert.deepEqual([last.page, last.sessions.length, last.sessions.at(-1)!.id], [2, 10, "S1"]);
    const raw = query(big, f({ range: "all" }), { grouped: false, page: 1, now: NOW });
    assert.deepEqual([raw.total, raw.pages, raw.commits.length, raw.commits[0].id], [60, 2, 10, "L10"]);
    const none = query(big, f({ text: "nothing like this" }), { grouped: true, page: 5, now: NOW });
    assert.deepEqual([none.total, none.pages, none.page, none.sessions.length], [0, 1, 0, 0]);
});

test("Most recent is the last day anything happened, even days ago", () => {
    assert.deepEqual(ids(grouped(EMPTY_FILTER)), ["S3:", "S2:L4,L3", "none:L5"], "the 26th, not S1's 23rd");
    const later = query(LOG, EMPTY_FILTER, { grouped: true, page: 0, now: new Date("2026-10-02T12:00:00Z") });
    assert.equal(later.total, 3, "a week later, Most recent still shows the 26th");
    const today = query(LOG, f({ range: "today" }), { grouped: true, page: 0, now: new Date("2026-10-02T12:00:00Z") });
    assert.equal(today.total, 0, "while Today is empty");
    assert.equal(query(parseLog("", sha), EMPTY_FILTER, { grouped: true, page: 0, now: NOW }).total, 0);
});

/* Two commits for one goal, each with its own behavior; the second forced,
 * the third a converted one whose behavior was never recorded. */
const L6 = commit("L6", "S4", "src/graph.ts", "Show the call graph\nso a reader sees who calls what", "2026-09-26T11:00:00Z", "edit", "claude", "Render Graph when the view opens");
const L7 = commit("L7", "S4", "src/graph.ts", "Show the call graph\nso a reader sees who calls what", "2026-09-26T11:01:00Z", "edit", "claude", "Import Graph for the view (needs #" + sha(L6).slice(0, 7) + ")", true);
const L8 = commit("L8", "S4", "src/old.ts", "Converted from a message", "2026-09-26T11:02:00Z", "edit", "claude", "The data of this field is not present");
const REFS = log(session("S4", "T-11: the graph", "2026-09-26T10:59:00Z"), L6, L7, L8);
const raw = (filter: HistoryFilter) => query(REFS, filter, { grouped: false, page: 0, now: NOW }).commits.map((c) => c.id);

test("a commit row is the intent's first line, and carries the intent, behavior, forced and hash", () => {
    const [l8, l7, l6] = query(REFS, ALL, { grouped: false, page: 0, now: NOW }).commits;
    assert.equal(l6.summary, "Show the call graph");
    assert.equal(l6.intent, "Show the call graph\nso a reader sees who calls what");
    assert.equal(l6.behavior, "Render Graph when the view opens");
    assert.equal(l6.forced, false);
    assert.equal(l7.forced, true);
    assert.equal(l6.hash, sha(L6));
    assert.equal(l8.behavior, "The data of this field is not present", "shown as it is");
    assert.equal("msg" in l6, false);
});

test("the text matches intent, behavior and hash, as well as id and file", () => {
    assert.deepEqual(raw(f({ text: "who calls" })), ["L7", "L6"], "the intent, past its first line");
    assert.deepEqual(raw(f({ text: "render graph" })), ["L6"], "the behavior");
    assert.deepEqual(raw(f({ text: "not present" })), ["L8"]);
    assert.deepEqual(raw(f({ text: "old.ts" })), ["L8"]);
    const h = sha(L8);
    assert.deepEqual(raw(f({ text: h.slice(0, 7) })), ["L8"], "a hash prefix of 7");
    assert.deepEqual(raw(f({ text: h.toUpperCase() })), ["L8"], "the whole hash, any case");
    assert.deepEqual(raw(f({ text: "#" + h.slice(0, 9) })), ["L8"], "with #");
    assert.deepEqual(raw(f({ text: h.slice(0, 6) })), [], "6 digits is not a hash prefix");
    const cited = sha(L6).slice(0, 7);
    assert.deepEqual(raw(f({ text: cited })), ["L7", "L6"], "bare, it is also text: L7 cites it");
    assert.deepEqual(raw(f({ text: "#" + cited })), ["L6"], "with #, the commit alone");
    assert.deepEqual(ids(query(REFS, f({ text: "#" + cited }), { grouped: true, page: 0, now: NOW })), ["S4:L6"]);
});

test("pageOf finds the page a commit is on, or null when the filter hides it", () => {
    const lines: string[] = [];
    for (let i = 1; i <= 60; i++) {
        lines.push(session(`S${i}`, `task ${i}`, `2026-09-20T10:${String(i).padStart(2, "0")}:00Z`));
        lines.push(commit(`L${i}`, `S${i}`, "a.ts", `edit ${i}`, `2026-09-20T10:${String(i).padStart(2, "0")}:30Z`));
    }
    const big = log(...lines);
    const at = (grouped: boolean) => ({ grouped, now: NOW });
    assert.equal(pageOf(big, ALL, at(true), "L60"), 0);
    assert.equal(pageOf(big, ALL, at(true), "L35"), 1, "S35 is the 26th session, newest first");
    assert.equal(pageOf(big, ALL, at(false), "L10"), 1, "the 51st commit");
    assert.equal(pageOf(big, f({ text: "edit 5" }), at(false), "L10"), null);
    assert.equal(pageOf(big, EMPTY_FILTER, at(true), "L1"), 2, "Most recent: every commit is on the 20th, the last day");
    assert.equal(pageOf(big, ALL, at(true), "L99"), null);
});

test("a tree row's keys: Enter toggles, ArrowRight opens, ArrowLeft closes, and moving on (Tab, ArrowDown) folds nothing", () => {
    for (const k of ["Tab", "ArrowDown", "ArrowUp", "Shift", "Escape", "a"]) {
        assert.equal(treeRowKey(k, true), null, `${k} leaves an open row open`);
        assert.equal(treeRowKey(k, false), null, `${k} leaves a closed row closed`);
    }
    assert.equal(treeRowKey("ArrowLeft", true), false);
    assert.equal(treeRowKey("ArrowRight", false), true);
    assert.equal(treeRowKey("ArrowRight", true), true, "already open: stays so");
    assert.equal(treeRowKey("Enter", true), false);
    assert.equal(treeRowKey("Enter", false), true);
});

/* Session rows as the view lays them out. */
const started = (id: string, msg: string, ticket?: string) =>
    JSON.stringify({ type: "session_start", id, msg, ...(ticket ? { meta: { ticket } } : {}), ts: "2026-09-26T08:00:00Z" });
const lineOf = (lines: string[], id: string, filtering = false) => {
    const s = query(log(...lines), ALL, { grouped: true, page: 0, now: NOW }).sessions.find((r) => r.id === id);
    assert.ok(s, `${id} is shown`);
    return sessionLine(s, filtering);
};

test("a session with a ticket leads with it; the purpose drops the prefix that repeats it; the session id moves to the far end", () => {
    const lines = [
        started("S3", "T-1: declare time() on Windows\nand the rest", "T-1"),
        commit("L1", "S3", "a.c", "one", "2026-09-26T08:01:00Z"),
        commit("L2", "S3", "a.c", "two", "2026-09-26T08:02:00Z"),
    ];
    const p = lineOf(lines, "S3");
    assert.equal(p.lead, "T-1");
    assert.equal(p.leadIsTicket, true);
    assert.equal(p.title, "declare time() on Windows");
    assert.equal(p.end, "S3 · 2 commits · active");
    assert.equal(p.tooltip, "S3 · T-1: declare time() on Windows\nand the rest\n\nstarted 2026-09-26T08:00:00Z · active");
});

test("a purpose that does not repeat the ticket is kept whole", () => {
    const p = lineOf([started("S4", "tidy the parser (T-12 follow-up)", "T-12"), end("S4", "2026-09-26T09:00:00Z")], "S4");
    assert.equal(p.lead, "T-12");
    assert.equal(p.title, "tidy the parser (T-12 follow-up)");
    assert.equal(p.end, "S4 · 0 commits");
    assert.equal(lineOf([started("S5", "T-12: x", "T-1")], "S5").title, "T-12: x", "T-1 is not a prefix of T-12");
    assert.equal(lineOf([started("S6", "T-7:", "T-7")], "S6").title, "T-7:", "nothing left after the prefix: the purpose stays");
});

test("a session without a ticket leads with its id as before, and does not repeat it at the far end", () => {
    const p = lineOf([started("S2", "T-9: fix the parser"), commit("L1", "S2", "p.ts", "x", "2026-09-26T08:01:00Z"), end("S2", "2026-09-26T09:00:00Z")], "S2");
    assert.equal(p.lead, "S2");
    assert.equal(p.leadIsTicket, false);
    assert.equal(p.title, "T-9: fix the parser");
    assert.equal(p.end, "1 commit");
    assert.equal(p.tooltip, "S2 · T-9: fix the parser\n\nstarted 2026-09-26T08:00:00Z, ended 2026-09-26T09:00:00Z");
});

test("a session ended with a summary carries it: first in the tooltip after the purpose, and as labelled parts", () => {
    const ended = JSON.stringify({ type: "session_end", id: "S2", done: "the parser takes #1a2b3c4's input", left: "error recovery\nand the fuzz run", ts: "2026-09-26T09:00:00Z" });
    const lines = [started("S2", "T-9: fix the parser", "T-9"), commit("L1", "S2", "p.ts", "x", "2026-09-26T08:01:00Z"), ended];
    const row = query(log(...lines), ALL, { grouped: true, page: 0, now: NOW }).sessions.find((r) => r.id === "S2");
    assert.deepEqual(row?.endSummary, { done: "the parser takes #1a2b3c4's input", decided: null, left: "error recovery\nand the fuzz run" });
    assert.deepEqual(endSummaryParts(row!.endSummary), [
        { label: "Done", text: "the parser takes #1a2b3c4's input" },
        { label: "Left", text: "error recovery\nand the fuzz run" },
    ]);
    assert.equal(
        sessionLine(row!, false).tooltip,
        "S2 · T-9: fix the parser\n\nDone: the parser takes #1a2b3c4's input\n\nLeft: error recovery\nand the fuzz run\n\nstarted 2026-09-26T08:00:00Z, ended 2026-09-26T09:00:00Z",
    );
    const plain = query(log(started("S3", "no summary"), end("S3", "2026-09-26T09:00:00Z")), ALL, { grouped: true, page: 0, now: NOW }).sessions[0];
    assert.equal(plain.endSummary, null);
    assert.deepEqual(endSummaryParts(plain.endSummary), []);
});

test("a branch folder's own session is named <branch>/S<n>; the parent's sessions before the branch are not", () => {
    const lines = [
        started("S1", "parent work", "T-3"),
        end("S1", "2026-09-26T08:30:00Z"),
        JSON.stringify({ type: "branch", id: "0123456789ab", name: "feat", parent: "main", base: "b", base_chunk: 1, ts: "2026-09-26T09:00:00Z" }),
        started("S2", "T-5: branch work", "T-5"),
        commit("L1", "S2", "a.c", "one", "2026-09-26T09:01:00Z"),
    ];
    const own = lineOf(lines, "S2");
    assert.equal(own.lead, "T-5");
    assert.equal(own.end, "feat/S2 · 1 commit · active");
    assert.ok(own.tooltip.startsWith("feat/S2 · "));
    assert.equal(lineOf(lines, "S1").end, "S1 · 0 commits");
    const bare = lineOf([...lines.slice(0, 3), started("S2", "no ticket here")], "S2");
    assert.equal(bare.lead, "feat/S2");
});

test("the count says how many of a session's commits a filter shows; the no-session group has no lead", () => {
    const lines = [
        started("S3", "T-1: a", "T-1"),
        commit("L1", "S3", "a.c", "alpha", "2026-09-26T08:01:00Z"),
        commit("L2", "S3", "a.c", "beta", "2026-09-26T08:02:00Z"),
        commit("L3", null, "n.md", "loose", "2026-09-26T08:03:00Z"),
    ];
    const s = query(log(...lines), f({ text: "beta" }), { grouped: true, page: 0, now: NOW }).sessions[0];
    assert.equal(sessionLine(s, true).end, "S3 · 1 of 2 commits · active");
    const none = query(log(...lines), ALL, { grouped: true, page: 0, now: NOW }).sessions.find((r) => r.id === null)!;
    assert.deepEqual([sessionLine(none, false).lead, sessionLine(none, false).end], [null, "1 commit"]);
});

test("the filter finds a session by its ticket even when the purpose does not name it", () => {
    const lines = [started("S1", "polish the sidebar", "T-44"), started("S2", "other", "T-45")];
    assert.deepEqual(ids(query(log(...lines), f({ text: "t-44" }), { grouped: true, page: 0, now: NOW })), ["S1:"]);
});
