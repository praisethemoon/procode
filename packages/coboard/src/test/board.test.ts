import * as assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { commitDiff } from "../lap";
import { handle } from "../mcp";
import { search, view } from "../query";
import { Board, BoardError, findBoard } from "../store";

/* Every test gets its own directory under the system temp dir, and only ever
 * deletes that directory. */
function tmp(): string {
    return fs.mkdtempSync(path.join(os.tmpdir(), "coboard-test-"));
}

function fresh(): Board {
    return new Board(tmp());
}

function code(fn: () => unknown): string {
    try {
        fn();
    } catch (e) {
        assert.ok(e instanceof BoardError, String(e));
        return e.code;
    }
    assert.fail("expected a BoardError");
}

test("ids are per kind, monotonic, and never reused after a delete", () => {
    const b = fresh();
    const e1 = b.create({ kind: "epic", title: "Windows backend" });
    const e2 = b.create({ kind: "epic", title: "Linux backend" });
    const m1 = b.create({ kind: "milestone", title: "IOCP", epic: e1.id });
    const t1 = b.create({ kind: "ticket", title: "Accept loop", epic: e1.id });
    assert.deepEqual([e1.id, e2.id, m1.id, t1.id], ["E-1", "E-2", "M-1", "T-1"]);
    b.remove("T-1");
    assert.equal(b.create({ kind: "ticket", title: "again", epic: "E-1" }).id, "T-2");
    assert.equal(code(() => b.get("T-1")), "not_found");
});

test("a ticket must be in an epic, and a milestone implies its epic", () => {
    const b = fresh();
    b.create({ kind: "epic", title: "A" });
    b.create({ kind: "epic", title: "B" });
    const m = b.create({ kind: "milestone", title: "m", epic: "E-1" });
    assert.equal(code(() => b.create({ kind: "ticket", title: "orphan" })), "invalid");
    const t = b.create({ kind: "ticket", title: "t", milestone: m.id });
    assert.equal(t.kind === "ticket" && t.epic, "E-1");
    assert.equal(code(() => b.create({ kind: "ticket", title: "t", epic: "E-2", milestone: "M-1" })), "invalid");
    assert.equal(code(() => b.create({ kind: "milestone", title: "m" })), "invalid");
    assert.equal(code(() => b.create({ kind: "ticket", title: "t", epic: "M-1" })), "invalid");
});

test("fields are checked, trimmed and only changed when given", () => {
    const b = fresh();
    b.create({ kind: "epic", title: "E" });
    const t = b.create({ kind: "ticket", title: "  spaced   title ", epic: "e-1", size: "M", labels: ["Net", "net", " io "] });
    assert.equal(t.title, "spaced title");
    assert.ok(t.kind === "ticket");
    assert.equal(t.size, "m");
    assert.deepEqual(t.labels, ["net", "io"]);
    assert.equal(t.status, "todo");
    assert.equal(t.priority, "medium");
    const u = b.update("T-1", { status: "doing", assignee: "claude" });
    assert.ok(u.kind === "ticket");
    assert.equal(u.status, "doing");
    assert.equal(u.size, "m");
    assert.equal(code(() => b.update("T-1", { status: "nope" })), "invalid");
    assert.equal(code(() => b.update("E-1", { size: "s" })), "invalid");
    assert.equal(code(() => b.update("E-1", { title: "  " })), "invalid");
});

test("moving a ticket keeps epic and milestone consistent", () => {
    const b = fresh();
    b.create({ kind: "epic", title: "A" });
    b.create({ kind: "epic", title: "B" });
    b.create({ kind: "milestone", title: "a1", epic: "E-1" });
    b.create({ kind: "milestone", title: "b1", epic: "E-2" });
    b.create({ kind: "ticket", title: "t", milestone: "M-1" });
    let t = b.move("T-1", { milestone: "M-2" });
    assert.ok(t.kind === "ticket");
    assert.deepEqual([t.epic, t.milestone], ["E-2", "M-2"]);
    t = b.move("T-1", { milestone: null });
    assert.ok(t.kind === "ticket");
    assert.deepEqual([t.epic, t.milestone], ["E-2", null]);
    b.move("T-1", { milestone: "M-2" });
    t = b.move("T-1", { epic: "E-1" });
    assert.ok(t.kind === "ticket");
    assert.deepEqual([t.epic, t.milestone], ["E-1", null], "a new epic leaves the old epic's milestone behind");
    assert.equal(code(() => b.move("T-1", { epic: "E-2", milestone: "M-1" })), "invalid");
    assert.equal(code(() => b.move("E-1", { epic: "E-2" })), "invalid");
});

test("moving a milestone takes its tickets to the new epic", () => {
    const b = fresh();
    b.create({ kind: "epic", title: "A" });
    b.create({ kind: "epic", title: "B" });
    b.create({ kind: "milestone", title: "m", epic: "E-1" });
    b.create({ kind: "ticket", title: "t", milestone: "M-1" });
    b.move("M-1", { epic: "E-2" });
    const t = b.get("T-1");
    assert.ok(t.kind === "ticket");
    assert.deepEqual([t.epic, t.milestone], ["E-2", "M-1"]);
});

test("an epic or milestone that still holds items cannot be deleted", () => {
    const b = fresh();
    b.create({ kind: "epic", title: "A" });
    b.create({ kind: "milestone", title: "m", epic: "E-1" });
    b.create({ kind: "ticket", title: "t", milestone: "M-1" });
    assert.equal(code(() => b.remove("M-1")), "in_use");
    assert.equal(code(() => b.remove("E-1")), "in_use");
    b.remove("T-1");
    b.remove("M-1");
    b.remove("E-1");
    assert.deepEqual(b.all(), []);
});

test("comments are appended in order and survive later updates", () => {
    const b = fresh();
    b.create({ kind: "epic", title: "E" });
    b.create({ kind: "ticket", title: "t", epic: "E-1" });
    b.comment("T-1", "first **md**", "ana");
    b.update("T-1", { status: "review" });
    const t = b.comment("T-1", "second", "");
    assert.deepEqual(t.comments.map((c) => [c.body, c.author]), [["first **md**", "ana"], ["second", "anonymous"]]);
    const again = b.get("T-1");
    assert.ok(again.kind === "ticket");
    assert.equal(again.comments.length, 2);
    assert.equal(again.status, "review");
    assert.equal(code(() => b.comment("E-1", "x", "a")), "invalid");
    assert.equal(code(() => b.comment("T-1", "  ", "a")), "invalid");
});

test("the log is plain JSONL, and a torn last line is ignored", () => {
    const b = fresh();
    b.create({ kind: "epic", title: "E" });
    fs.appendFileSync(b.logPath, '{"op":"put","item":{"id":"E-9"');
    assert.deepEqual(b.all().map((i) => i.id), ["E-1"]);
    const lines = fs.readFileSync(b.logPath, "utf8").split("\n");
    assert.deepEqual(JSON.parse(lines[0]).op, "put");
    assert.equal(fs.readFileSync(path.join(b.dir, ".gitignore"), "utf8"), "lock\n");
    assert.ok(!fs.existsSync(path.join(b.dir, "lock")), "the lock is released after a write");
});

test("two boards over one directory never hand out the same id", () => {
    const dir = tmp();
    const a = new Board(dir);
    const b = new Board(dir);
    a.create({ kind: "epic", title: "E" });
    const ids = new Set<string>();
    for (let i = 0; i < 10; i++) {
        ids.add((i % 2 ? a : b).create({ kind: "ticket", title: `t${i}`, epic: "E-1" }).id);
    }
    assert.equal(ids.size, 10);
});

test("findBoard walks up to the nearest .coboard", () => {
    const dir = tmp();
    fs.mkdirSync(path.join(dir, ".coboard"));
    fs.mkdirSync(path.join(dir, "a", "b"), { recursive: true });
    assert.equal(findBoard(path.join(dir, "a", "b")), dir);
});

test("views: an epic shows its milestones with progress and its dangling tickets", () => {
    const b = fresh();
    b.create({ kind: "epic", title: "E" });
    b.create({ kind: "milestone", title: "m", epic: "E-1" });
    b.create({ kind: "ticket", title: "in m", milestone: "M-1", status: "done" });
    b.create({ kind: "ticket", title: "in m too", milestone: "M-1" });
    b.create({ kind: "ticket", title: "dangling", epic: "E-1" });
    const v = view(b.all(), "e-1");
    assert.ok(v && v.kind === "epic");
    assert.deepEqual(v.milestones.map((m) => [m.id, m.counts["done"], m.counts["todo"]]), [["M-1", 1, 1]]);
    assert.deepEqual(v.tickets.map((t) => t.id), ["T-3"]);
    assert.equal(v.counts["todo"], 2);
    const mv = view(b.all(), "M-1");
    assert.ok(mv && mv.kind === "milestone");
    assert.equal(mv.epic?.id, "E-1");
    assert.deepEqual(mv.tickets.map((t) => t.id), ["T-1", "T-2"]);
    const tv = view(b.all(), "T-1");
    assert.ok(tv && tv.kind === "ticket");
    assert.deepEqual([tv.epic?.id, tv.milestone?.id], ["E-1", "M-1"]);
    assert.equal(view(b.all(), "T-99"), null);
});

test("search: every word must match, an exact id ranks first, filters narrow", () => {
    const b = fresh();
    b.create({ kind: "epic", title: "Networking" });
    b.create({ kind: "ticket", title: "IOCP accept loop", epic: "E-1", labels: ["windows"] });
    b.create({ kind: "ticket", title: "epoll", description: "mirror the IOCP accept loop on linux", epic: "E-1" });
    b.create({ kind: "ticket", title: "docs", epic: "E-1", assignee: "ana" });
    b.comment("T-3", "mention the accept loop", "x");
    const hits = search(b.all(), "accept loop");
    assert.deepEqual(hits.map((h) => h.id), ["T-1", "T-2", "T-3"]);
    assert.ok(hits[1].snippet?.includes("accept loop"));
    assert.deepEqual(hits[2].matched, ["comments"]);
    assert.equal(search(b.all(), "T-3")[0].id, "T-3");
    assert.deepEqual(search(b.all(), "", { label: "windows" }).map((h) => h.id), ["T-1"]);
    assert.deepEqual(search(b.all(), "", { assignee: "ANA" }).map((h) => h.id), ["T-3"]);
    assert.deepEqual(search(b.all(), "", { kind: "epic" }).map((h) => h.id), ["E-1"]);
    assert.deepEqual(search(b.all(), "", { epic: "E-1", milestone: "none" }).length, 3);
    assert.deepEqual(search(b.all(), "accept", { limit: 1 }).length, 1);
    assert.deepEqual(search(b.all(), "accept nonsense"), []);
});

/* ------------------------------------------------------------ MCP */

async function call(cwd: string, name: string, args: object) {
    const out = await handle({ jsonrpc: "2.0", id: 1, method: "tools/call", params: { name, arguments: args } }, { cwd, author: "agent" });
    const result = (out as { result: { content: { text: string }[]; isError?: boolean } }).result;
    return { error: result.isError === true, text: result.content[0].text };
}

test("mcp: the tools create, move, comment, search and get by id", async () => {
    const dir = tmp();
    fs.mkdirSync(path.join(dir, ".git"));
    const list = await handle({ jsonrpc: "2.0", id: 1, method: "tools/list" }, { cwd: dir, author: "a" });
    const names = ((list as { result: { tools: { name: string }[] } }).result.tools).map((t) => t.name);
    assert.deepEqual(names, ["board_list", "board_search", "board_get", "board_create", "board_update", "board_move", "board_comment", "board_sessions"]);

    assert.equal((await call(dir, "board_list", {})).text, "[]", "no board yet reads as empty");
    const e = JSON.parse((await call(dir, "board_create", { kind: "epic", title: "E" })).text);
    assert.equal(e.id, "E-1");
    assert.ok(fs.existsSync(path.join(dir, ".coboard", "log.jsonl")), "the first write creates the board at the git root");
    await call(dir, "board_create", { kind: "milestone", title: "M", epic: "E-1" });
    await call(dir, "board_create", { kind: "ticket", title: "Fix the parser", epic: "E-1" });
    const moved = JSON.parse((await call(dir, "board_move", { id: "T-1", milestone: "M-1" })).text);
    assert.equal(moved.milestone, "M-1");
    await call(dir, "board_comment", { ticket: "T-1", body: "on it" });
    await call(dir, "board_update", { id: "T-1", status: "doing" });
    const got = JSON.parse((await call(dir, "board_get", { id: "t-1" })).text);
    assert.equal(got.ticket.status, "doing");
    assert.equal(got.ticket.comments[0].author, "agent");
    assert.equal(got.milestone.id, "M-1");
    const found = JSON.parse((await call(dir, "board_search", { query: "parser" })).text);
    assert.equal(found[0].id, "T-1");
});

test("mcp: bad input comes back as a tool error, not a crash", async () => {
    const dir = tmp();
    fs.mkdirSync(path.join(dir, ".git"));
    assert.match((await call(dir, "board_get", { id: "T-7" })).text, /^not_found/);
    const r = await call(dir, "board_create", { kind: "ticket", title: "t", store: "global" });
    assert.ok(r.error);
    assert.match(r.text, /takes no "store"/);
    assert.match((await call(dir, "board_create", { kind: "ticket" })).text, /needs "title"/);
});

test("mcp: outside any repository nothing is created", async () => {
    const dir = tmp();
    const r = await call(dir, "board_create", { kind: "epic", title: "E" });
    // The temp dir is not a repository; unless some parent of it is, the
    // write is refused and nothing appears.
    if (!findGitAbove(dir)) {
        assert.ok(r.error);
        assert.match(r.text, /^no_board/);
        assert.ok(!fs.existsSync(path.join(dir, ".coboard")));
    }
});

function findGitAbove(dir: string): boolean {
    for (let d = dir; ; d = path.dirname(d)) {
        if (fs.existsSync(path.join(d, ".git"))) return true;
        if (path.dirname(d) === d) return false;
    }
}

/* ------------------------------------------------------------ lap */

const LAP = path.resolve(__dirname, "../../../../cli/lap-cli/bin/lap");

test("lap: a session tagged with a ticket is found through the board", { skip: !fs.existsSync(LAP) && "lap is not built" }, async () => {
    const dir = tmp();
    fs.mkdirSync(path.join(dir, ".git"));
    process.env["LAP_BIN"] = LAP;
    const lap = (...args: string[]) => execFileSync(LAP, args, { cwd: dir, env: { ...process.env, LAP_USER: "tester" } });
    lap("init");
    await call(dir, "board_create", { kind: "epic", title: "E" });
    await call(dir, "board_create", { kind: "ticket", title: "t", epic: "E-1" });
    lap("session", "start", "T-1: do it", "--meta", "ticket=T-1");
    fs.writeFileSync(path.join(dir, "a.txt"), "hello\n");
    lap("commit", "a.txt", "-m", "the first line");
    lap("session", "end");
    lap("session", "start", "unrelated", "--meta", "ticket=T-9");
    const s = JSON.parse((await call(dir, "board_sessions", { ticket: "T-1" })).text);
    assert.deepEqual(s.sessions.map((x: { id: string }) => x.id), ["S1"]);
    assert.equal(s.sessions[0].commits[0].msg, "the first line");
    const got = JSON.parse((await call(dir, "board_get", { id: "T-1" })).text);
    assert.equal(got.sessions[0].id, "S1");

    // An edit in the middle of a file comes back as the file before and after.
    lap("session", "end");
    lap("session", "start", "T-1: more", "--meta", "ticket=T-1");
    fs.writeFileSync(path.join(dir, "a.txt"), "hello\nkeep\nold one\nold two\ntail\n");
    lap("commit", "a.txt", "-m", "grow it");
    fs.writeFileSync(path.join(dir, "a.txt"), "hello\nkeep\nnew\ntail\n");
    lap("commit", "a.txt", "-m", "replace two lines with one");
    const edit = await commitDiff(dir, "L3");
    assert.equal(edit.before, "hello\nkeep\nold one\nold two\ntail\n");
    assert.equal(edit.after, "hello\nkeep\nnew\ntail\n");
    assert.equal(edit.line, 3);
    const created = await commitDiff(dir, "L1");
    assert.deepEqual([created.op, created.before, created.after], ["create", "", "hello\n"]);
});

/* ------------------------------------------------------------- archiving */

function archivedBoard(): { b: Board; ids: Record<string, string> } {
    const b = fresh();
    const e1 = b.create({ kind: "epic", title: "Shipped" });
    const e2 = b.create({ kind: "epic", title: "Current" });
    const m1 = b.create({ kind: "milestone", title: "Beta", epic: e1.id });
    const t1 = b.create({ kind: "ticket", title: "In the milestone", milestone: m1.id });
    const t2 = b.create({ kind: "ticket", title: "Loose in the epic", epic: e1.id });
    const t3 = b.create({ kind: "ticket", title: "Elsewhere", epic: e2.id });
    return { b, ids: { e1: e1.id, e2: e2.id, m1: m1.id, t1: t1.id, t2: t2.id, t3: t3.id } };
}

test("archiving an epic hides it and everything under it, with one record, and unarchiving brings it all back", () => {
    const { b, ids } = archivedBoard();
    const before = fs.readFileSync(b.logPath, "utf8").split("\n").length;
    const a = b.archive(ids.e1);
    assert.deepEqual(a.archived && { via: a.archived.via }, { via: null });
    const lines = fs.readFileSync(b.logPath, "utf8").trim().split("\n");
    assert.equal(lines.length, before, "exactly one record was appended");
    assert.equal(JSON.parse(lines[lines.length - 1]).op, "archive");

    assert.deepEqual(b.all().map((i) => i.id), [ids.e2, ids.t3], "the default leaves them out");
    assert.deepEqual(b.all({ archived: "only" }).map((i) => i.id), [ids.e1, ids.m1, ids.t1, ids.t2]);
    assert.equal(b.all({ archived: "include" }).length, 6);
    assert.deepEqual(b.get(ids.t1).archived && b.get(ids.t1).archived!.via, ids.e1, "get works and says what it is archived with");
    assert.equal(b.get(ids.t3).archived, undefined);

    b.unarchive(ids.e1);
    assert.equal(b.all().length, 6);
    assert.equal(b.get(ids.t1).archived, undefined);
});

test("a milestone archived on its own takes its tickets, and they come back with it", () => {
    const { b, ids } = archivedBoard();
    b.archive(ids.m1);
    assert.deepEqual(b.all({ archived: "only" }).map((i) => i.id), [ids.m1, ids.t1]);
    assert.equal(b.get(ids.t1).archived?.via, ids.m1);
    assert.equal(b.get(ids.t2).archived, undefined, "a ticket loose in the epic is not in the milestone");
});

test("archive and unarchive refuse what they cannot do, with a reason", () => {
    const { b, ids } = archivedBoard();
    b.archive(ids.e1);
    assert.equal(code(() => b.archive(ids.e1)), "invalid");
    assert.equal(code(() => b.unarchive(ids.e2)), "invalid");
    assert.throws(() => b.unarchive(ids.t1), /archived with E-1; unarchive E-1/);
    assert.equal(code(() => b.archive("T-99")), "not_found");
    // An item archived with its epic may still be archived itself, and then
    // stays archived when the epic comes back.
    b.archive(ids.t2);
    b.unarchive(ids.e1);
    assert.deepEqual(b.all({ archived: "only" }).map((i) => i.id), [ids.t2]);
});

test("nothing new goes into an archived epic or milestone", () => {
    const { b, ids } = archivedBoard();
    b.archive(ids.m1);
    assert.throws(() => b.create({ kind: "ticket", title: "x", milestone: ids.m1 }), /M-1 is archived/);
    assert.throws(() => b.move(ids.t3, { milestone: ids.m1 }), /M-1 is archived/);
    b.unarchive(ids.m1);
    b.archive(ids.e1);
    assert.throws(() => b.create({ kind: "milestone", title: "x", epic: ids.e1 }), /E-1 is archived/);
    assert.throws(() => b.create({ kind: "ticket", title: "x", milestone: ids.m1 }), /M-1 is archived with E-1/);
    assert.throws(() => b.move(ids.t3, { epic: ids.e1 }), /E-1 is archived/);
});

test("an archive survives later writes to the item, and archived ids are never reused", () => {
    const { b, ids } = archivedBoard();
    b.archive(ids.t3);
    const u = b.update(ids.t3, { title: "Renamed while archived" });
    assert.ok(u.archived, "the update's answer is still archived");
    b.comment(ids.t3, "a note", "agent");
    assert.ok(b.get(ids.t3).archived);
    // No put ever carries the archive: it lives in its own records only.
    for (const line of fs.readFileSync(b.logPath, "utf8").trim().split("\n")) {
        const r = JSON.parse(line);
        if (r.op === "put") assert.equal("archived" in r.item, false);
    }
    assert.equal(b.create({ kind: "ticket", title: "next", epic: ids.e2 }).id, "T-4");
});

test("a log from a newer coboard loads: unknown records are skipped, as older readers skip archive records", () => {
    const { b, ids } = archivedBoard();
    fs.appendFileSync(b.logPath, JSON.stringify({ op: "someday", id: ids.t1, at: "2030-01-01T00:00:00Z" }) + "\n");
    assert.equal(b.all().length, 6);
    // What an older reader sees: the fold without the records it does not
    // know. Every item is still there, unchanged.
    b.archive(ids.e1);
    const older = fs
        .readFileSync(b.logPath, "utf8")
        .trim()
        .split("\n")
        .map((l) => JSON.parse(l))
        .filter((r) => r.op === "put");
    assert.equal(new Set(older.map((r) => r.item.id)).size, 6);
});

test("search and view leave archived items out unless asked, and an archived item's view shows its contents", () => {
    const { b, ids } = archivedBoard();
    b.archive(ids.m1);
    const all = b.all({ archived: "include" });
    assert.deepEqual(search(all, "milestone").map((h) => h.id), [], "T-1 is in the archived milestone");
    assert.deepEqual(search(all, "milestone", { archived: "include" }).map((h) => h.id), [ids.t1]);
    assert.equal(search(all, "", { archived: "only" }).every((h) => h.archived === true), true);
    assert.equal(search(all, "current")[0].archived, undefined, "the flag is absent, not false, on a live item");

    const epic = view(all, ids.e1);
    assert.ok(epic && epic.kind === "epic");
    assert.deepEqual(epic.milestones.map((m) => m.id), [], "the live epic leaves its archived milestone out");
    assert.deepEqual(epic.allTickets.map((t) => t.id), [ids.t2]);
    const withArchived = view(all, ids.e1, { archived: "include" });
    assert.ok(withArchived && withArchived.kind === "epic");
    assert.deepEqual(withArchived.milestones.map((m) => m.id), [ids.m1]);
    const ms = view(all, ids.m1);
    assert.ok(ms && ms.kind === "milestone");
    assert.deepEqual(ms.tickets.map((t) => t.id), [ids.t1], "an archived milestone shows what it holds");
});
