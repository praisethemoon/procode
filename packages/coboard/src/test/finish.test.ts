/* ticket_finish: a ticket closed in one step, and the git command handed
 * back that commits exactly the session's files. First its parts against a
 * stand-in lap (the refusals write nothing), then the whole tool through
 * the MCP server against a real lap, with the returned command run in a
 * real git repository. */

import * as assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { commitMessage, endTicketSession, gitCommand, sessionFiles, shellWord } from "../finish";
import { handle } from "../mcp";
import { Board } from "../store";
import { cliBin, noCli } from "./cli-bin";

type Json = Record<string, unknown>;

test("the session's files: first-touch order, once each, without files it only untracked", () => {
    const files = sessionFiles([
        { file: "a.ts", op: "create" },
        { file: "b.ts", op: "edit" },
        { file: "a.ts", op: "edit" },
        { file: "gen/x.txt", op: "untrack" },
        { file: "old.ts", op: "delete" },
    ]);
    assert.deepEqual(files, ["a.ts", "b.ts", "old.ts"], "a delete is git's to record; an untrack is not");
    assert.deepEqual(sessionFiles([{ file: "c", op: "untrack" }, { file: "c", op: "create" }]), ["c"], "tracked again: recorded");
});

test("the git command stages the files, lap's log and (here) the board's, and quotes what needs it", () => {
    const g = gitCommand(["src/a.ts", "docs/my notes.md"], "T-9", "Parser: keep comments", true);
    assert.deepEqual(g.paths, ["src/a.ts", "docs/my notes.md", ".lap/log", ".coboard/log.jsonl"]);
    assert.equal(g.message, "Parser: keep comments (T-9)");
    assert.equal(g.command, "git add -- src/a.ts 'docs/my notes.md' .lap/log .coboard/log.jsonl && git commit -m 'Parser: keep comments (T-9)'");
    assert.deepEqual(gitCommand([], "T-9", "x", false).paths, [".lap/log"], "a board elsewhere is not this folder's to commit");
    assert.equal(commitMessage("  Fix   it (T-9) ", "T-9"), "Fix it (T-9)", "the id is not added twice");
    assert.equal(shellWord("it's"), `'it'\\''s'`);
});

/* A lap that answers from a script, recording what it was asked. */
function fakeLap(answers: Record<string, Json>, asked: string[][]) {
    return async (args: string[]) => {
        asked.push(args);
        const key = args.slice(0, 2).join(" ");
        const a = answers[key];
        if (!a) throw new Error(`unexpected lap ${key}`);
        return a;
    };
}

const input = { ticket: "T-9", done: "keeps comments", subject: "Parser: keep comments", tests: "12/12" };
const sessions = { ok: true, sessions: [{ id: "S1", ref: "S1", active: false }, { id: "S4", ref: "S4", active: true }] };
const log = { ok: true, commits: [{ file: "src/b.ts", op: "edit" }, { file: "src/a.ts", op: "edit" }, { file: "src/a.ts", op: "create" }] };

test("refusals come before anything is written: no active session, or the session's file has unrecorded edits", async () => {
    let asked: string[][] = [];
    await assert.rejects(
        endTicketSession(fakeLap({ "session list": { ok: true, sessions: [{ id: "S1", active: false }] } }, asked), input, true),
        /no_session|no active lap session/,
    );
    asked = [];
    const status = { ok: true, files: [{ path: "src/a.ts", state: "modified" }, { path: "notes.txt", state: "new" }] };
    await assert.rejects(
        endTicketSession(fakeLap({ "session list": sessions, "log --session": log, status }, asked), input, true),
        (e: Error & { code?: string }) => e.code === "pending_edits" && /src\/a\.ts has edits lap has not recorded in S4/.test(e.message),
    );
    assert.ok(!asked.some((a) => a[0] === "session" && a[1] === "end"), "the session was not ended");
});

test("a finish ends the session with its summary, and names pending edits that are not the ticket's", async () => {
    const asked: string[][] = [];
    const status = { ok: true, files: [{ path: "notes.txt", state: "new" }] };
    const ended = await endTicketSession(
        fakeLap({ "session list": sessions, "log --session": log, status, "session end": { ok: true } }, asked),
        { ...input, decided: "kept as tokens", notVerified: "Windows" },
        true,
    );
    assert.deepEqual(asked.find((a) => a[1] === "end"), ["session", "end", "--done", "keeps comments", "--decided", "kept as tokens"]);
    assert.deepEqual(ended.git.paths, ["src/a.ts", "src/b.ts", ".lap/log", ".coboard/log.jsonl"], "oldest first");
    assert.deepEqual(ended.otherPending, ["notes.txt"]);
    assert.equal(ended.session, "S4");
    assert.match(ended.comment, /^Done in lap S4; git: "Parser: keep comments \(T-9\)"/);
    assert.match(ended.comment, /\*\*Decided\.\*\* kept as tokens/);
    assert.match(ended.comment, /\*\*Tests\.\*\* 12\/12/);
    assert.match(ended.comment, /\*\*Not verified\.\*\* Windows/);
    assert.doesNotMatch(ended.comment, /\*\*Left\./, "an empty part is left out");
});

const lapBin = cliBin("lap");

test("through the server, against a real lap: the session ends, the ticket closes, and the command commits exactly its files", { skip: lapBin ? false : noCli("lap") }, async () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "coboard-finish-"));
    const env = { ...process.env, LAP_USER: "test", GIT_AUTHOR_NAME: "t", GIT_AUTHOR_EMAIL: "t@t", GIT_COMMITTER_NAME: "t", GIT_COMMITTER_EMAIL: "t@t" };
    const lap = (...a: string[]) => execFileSync(lapBin, a, { cwd: root, env, encoding: "utf8" });
    const git = (...a: string[]) => execFileSync("git", a, { cwd: root, env, encoding: "utf8" });
    git("init", "-q");
    lap("init");
    fs.writeFileSync(path.join(root, "base.txt"), "base\n");
    git("add", "-A");
    git("commit", "-qm", "base");
    for (const f of [".lapignore", "base.txt"]) lap("commit", f, "-i", `the project starts with ${f}`, "-b", `${f} is recorded`, "--no-session", "--whole-file");

    const board = new Board(root);
    const epic = board.create({ kind: "epic", title: "Work" });
    const t = board.create({ kind: "ticket", epic: epic.id, title: "Add a parser" });
    lap("session", "start", `${t.id}: add a parser`, "--meta", `ticket=${t.id}`);
    fs.writeFileSync(path.join(root, "parser.ts"), "export const p = 1;\n");
    fs.writeFileSync(path.join(root, "my notes.md"), "notes\n");
    lap("commit", "parser.ts", "-i", "a parser for the input", "-b", "parser.ts exports p");
    lap("commit", "my notes.md", "-i", "notes on the parser", "-b", "my notes.md holds the notes");
    fs.writeFileSync(path.join(root, "unrelated.txt"), "not this ticket's\n");

    process.env["LAP_BIN"] = lapBin;
    process.env["LAP_USER"] = "test";
    const call = async (args: Json) => {
        const out = (await handle({ jsonrpc: "2.0", id: 1, method: "tools/call", params: { name: "ticket_finish", arguments: args } }, { cwd: root, author: "claude" })) as Json;
        const r = out["result"] as Json;
        return { isError: r["isError"] === true, text: (r["content"] as { text: string }[])[0].text };
    };

    // An unrecorded edit to the session's file is refused, and nothing changes.
    fs.appendFileSync(path.join(root, "parser.ts"), "export const q = 2;\n");
    const refused = await call({ ticket: t.id, done: "a parser", subject: "Add a parser" });
    assert.equal(refused.isError, true);
    assert.match(refused.text, /^pending_edits: parser\.ts has edits/);
    assert.match(lap("session", "current"), /S1/, "the session is still active");
    assert.equal(board.get(t.id).status, "todo");
    lap("commit", "parser.ts", "-i", "a parser for the input", "-b", "parser.ts also exports q");

    const res = await call({ ticket: t.id, done: "a parser with p and q", subject: "Add a parser", tests: "2 checks", status: "review" });
    assert.equal(res.isError, false, res.text);
    const out = JSON.parse(res.text);
    assert.equal(out.session, "S1");
    assert.deepEqual(out.git.paths, ["parser.ts", "my notes.md", ".lap/log", ".coboard/log.jsonl"]);
    assert.deepEqual(out.otherPending, ["unrelated.txt"]);
    assert.doesNotMatch(lap("session", "current"), /S1/, "the session ended");
    const after = new Board(root).get(t.id) as unknown as { status: string; comments: readonly { body: string }[] };
    assert.equal(after.status, "review");
    assert.match(after.comments.at(-1)!.body, /^Done in lap S1; git: "Add a parser \(T-\d+\)"/);
    const ended = JSON.parse(lap("session", "list", "--json")).sessions[0];
    assert.equal(ended.summary?.done, "a parser with p and q", "the summary is the session's");

    // The command, run as handed back, commits exactly those files.
    execFileSync("sh", ["-c", out.git.command], { cwd: root, env });
    const committed = git("show", "--name-only", "--format=%s", "HEAD").trim().split("\n");
    assert.equal(committed[0], `Add a parser (${t.id})`);
    const files = committed.slice(1).filter(Boolean);
    assert.ok(files.includes("parser.ts") && files.includes("my notes.md") && files.includes(".coboard/log.jsonl"));
    assert.ok(files.some((f) => f.startsWith(".lap/log/")));
    assert.ok(!files.includes("unrelated.txt"), "a file the session did not touch is not committed");
    assert.ok(!files.some((f) => f.startsWith(".lap/") && !f.startsWith(".lap/log/")), "only lap's log, never its caches");
});
