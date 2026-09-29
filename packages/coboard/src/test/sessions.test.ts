/* A ticket's work across branches: sessions still in a branch folder are
 * listed with their branch; once lap merge adopted them they are listed
 * once, as the adopted ones, with the branch and what its merge stopped;
 * sessions a branch shares with its parent (from before its base) are not
 * listed twice. First on its own (mergeSessions), then through lap. */

import * as assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { branchArgs, LapSession, mergeSessions, sessionReview, startSession, summaryParts, ticketSessions } from "../lap";
import { handle } from "../mcp";
import { Board } from "../store";
import { cliBin, noCli } from "./cli-bin";

const s = (id: string, hash: string, started: string, extra: Partial<LapSession> = {}): LapSession => ({
    id,
    hash,
    msg: `${id} work`,
    started,
    ended: null,
    commits: 1,
    active: false,
    ...extra,
});

test("mergeSessions: before a merge, the branch's sessions are listed with their branch", () => {
    const main = [s("S1", "h1", "2026-09-27T10:00:00Z")];
    const out = mergeSessions(main, [
        { name: "parser", stops: [], sessions: [s("S1", "h1", "2026-09-27T10:00:00Z"), s("S2", "h2", "2026-09-27T11:00:00Z")] },
    ]);
    assert.deepEqual(out.map((x) => `${x.branch ?? "main"}/${x.id}`), ["main/S1", "parser/S2"], "the shared S1 is listed once");
});

test("mergeSessions: after a merge, an adopted session is listed once, as the adopted one", () => {
    const main = [s("S1", "h1", "2026-09-27T10:00:00Z"), s("S3", "h3", "2026-09-27T11:00:00Z", { from: "h2" })];
    const out = mergeSessions(main, [
        { name: "parser", stops: [{ file: "a.ts", at: "c9" }], sessions: [s("S2", "h2", "2026-09-27T11:00:00Z")] },
    ]);
    assert.deepEqual(out.map((x) => x.id), ["S1", "S3"]);
    assert.equal(out[1].adoptedFrom, "parser");
    assert.deepEqual(out[1].stops, [{ file: "a.ts", at: "c9" }], "a partly merged branch's stops go with its adopted session");
    assert.equal(out[1].branch, undefined);
    assert.equal(main[1].adoptedFrom, undefined, "the input is left as it was");
});

test("mergeSessions: a branch of a branch lists its parent branch's sessions once, as that branch's, and its own under both names", () => {
    const main = [s("S1", "h1", "2026-09-27T10:00:00Z")];
    const busy = s("S2", "h2", "2026-09-27T11:00:00Z");
    const out = mergeSessions(main, [
        { name: "busy", stops: [], sessions: [busy] },
        // sub's history holds main's S1 and busy's S2 up to its base
        { name: "sub", via: "busy", stops: [], sessions: [busy, s("S3", "h3", "2026-09-27T12:00:00Z")] },
    ]);
    assert.deepEqual(out.map((x) => `${x.via ?? "-"}/${x.branch ?? "main"}/${x.id}`), ["-/main/S1", "-/busy/S2", "busy/sub/S3"]);
});

test("mergeSessions: a nested branch's session its parent branch adopted is shown once, as the adopted one", () => {
    const main = [s("S1", "h1", "2026-09-27T10:00:00Z")];
    const out = mergeSessions(main, [
        { name: "busy", stops: [], sessions: [s("S4", "h4", "2026-09-27T12:00:00Z", { from: "h3" })] },
        { name: "sub", via: "busy", stops: [], sessions: [s("S3", "h3", "2026-09-27T12:00:00Z")] },
    ]);
    assert.deepEqual(out.map((x) => `${x.branch ?? "main"}/${x.id}`), ["main/S1", "busy/S4"]);
});

test("mergeSessions: sessions come out oldest first, and with no branches, as they were", () => {
    const main = [s("S1", "h1", "2026-09-27T10:00:00Z")];
    assert.deepEqual(mergeSessions(main, []), main);
    const out = mergeSessions(main, [{ name: "b", stops: [], sessions: [s("S2", "h0", "2026-09-27T09:00:00Z")] }]);
    assert.deepEqual(out.map((x) => x.id), ["S2", "S1"]);
});

const LAP = cliBin("lap");

test("summaryParts: the parts a session ended with, in order and labelled; none without a summary", () => {
    assert.deepEqual(summaryParts({ done: "renamed it", decided: null, left: "the icons" }), [
        { label: "Done", text: "renamed it" },
        { label: "Left", text: "the icons" },
    ]);
    assert.deepEqual(summaryParts({ done: null, decided: "ids stay", left: null }), [{ label: "Decided", text: "ids stay" }]);
    assert.deepEqual(summaryParts(null), []);
    assert.deepEqual(summaryParts(undefined), [], "a lap from before summaries gives none");
});

test("ticketSessions and sessionReview: through lap, a session's end summary comes with it", { skip: !LAP && noCli("lap") }, async () => {
    const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "coboard-summary-")));
    const saved = process.env["LAP_BIN"];
    process.env["LAP_BIN"] = LAP;
    const lap = (...args: string[]) => execFileSync(LAP, args, { cwd: root, env: { ...process.env, LAP_USER: "tester" } });
    try {
        lap("init");
        fs.writeFileSync(path.join(root, "a.txt"), "a\n");
        lap("session", "start", "T-2: the first file", "--meta", "ticket=T-2");
        lap("commit", "a.txt", "-i", "Seed the project file", "-b", "Records a.txt as it starts");
        lap("session", "end", "--done", "a.txt is recorded", "--left", "b.txt");
        lap("session", "start", "T-2: nothing to say", "--meta", "ticket=T-2");
        fs.writeFileSync(path.join(root, "a.txt"), "a\nb\n");
        lap("commit", "a.txt", "-i", "The file needs a second line", "-b", "a.txt gains b");
        lap("session", "end");
        const sessions = await ticketSessions(root, "T-2");
        assert.ok(sessions.ok, sessions.error);
        assert.deepEqual(
            sessions.value.map((x) => x.summary),
            [{ done: "a.txt is recorded", decided: null, left: "b.txt" }, null],
        );
        const review = await sessionReview(root, "S1");
        assert.deepEqual(review.value?.summary, { done: "a.txt is recorded", decided: null, left: "b.txt" });
        assert.equal((await sessionReview(root, "S2")).value?.summary, null);
    } finally {
        if (saved === undefined) delete process.env["LAP_BIN"];
        else process.env["LAP_BIN"] = saved;
    }
});

test("ticketSessions: through lap, before a merge, after it, and partly merged", { skip: !LAP && noCli("lap") }, async () => {
    const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "coboard-sessions-")));
    const parent = path.join(root, "proj");
    fs.mkdirSync(parent);
    const saved = process.env["LAP_BIN"];
    process.env["LAP_BIN"] = LAP;
    const lap = (cwd: string, ...args: string[]) => execFileSync(LAP, args, { cwd, env: { ...process.env, LAP_USER: "tester" } });
    try {
        lap(parent, "init");
        fs.writeFileSync(path.join(parent, "a.txt"), "one\ntwo\nthree\nfour\nfive\n");
        fs.writeFileSync(path.join(parent, "b.txt"), "b\n");
        lap(parent, "session", "start", "T-1: before the branch", "--meta", "ticket=T-1");
        for (const f of ["a.txt", "b.txt", ".lapignore"]) lap(parent, "commit", f, "-i", "Seed the project files", "-b", `Records ${f} as it starts`);
        lap(parent, "session", "end");
        for (const b of ["feat", "half"]) {
            fs.cpSync(parent, path.join(root, b), { recursive: true });
            lap(path.join(root, b), "branch", "start", b, "--from", parent);
        }
        const work = (b: string, file: string, from: string, to: string) => {
            const dir = path.join(root, b);
            lap(dir, "session", "start", `T-1: work in ${b}`, "--meta", "ticket=T-1", "--branch", b);
            const p = path.join(dir, file);
            fs.writeFileSync(p, fs.readFileSync(p, "utf8").replace(from, to));
            lap(dir, "commit", file, "--branch", b, "-i", `Change ${file} in ${b}`, "-b", `Rewrites ${from.trim()} as ${to.trim()}`);
            lap(dir, "session", "end");
        };
        work("feat", "b.txt", "b\n", "B\n");
        work("half", "a.txt", "two\n", "TWO\n");

        // before any merge: this folder's session, then one per branch
        let r = await ticketSessions(parent, "T-1");
        assert.equal(r.ok, true, r.error);
        assert.deepEqual(r.value.map((x) => `${x.branch ?? "main"}:${x.msg}`), [
            "main:T-1: before the branch",
            "feat:T-1: work in feat",
            "half:T-1: work in half",
        ]);

        // feat merged whole: its session is listed once, adopted
        fs.copyFileSync(path.join(root, "feat", "b.txt"), path.join(parent, "b.txt"));
        lap(parent, "merge", "feat");
        r = await ticketSessions(parent, "T-1");
        const adopted = r.value.filter((x) => x.msg === "T-1: work in feat");
        assert.equal(adopted.length, 1, "no duplicate after adoption");
        assert.equal(adopted[0].adoptedFrom, "feat");
        assert.equal(adopted[0].branch, undefined);

        // half stopped at a conflict: adopted with its stop
        const a = path.join(parent, "a.txt");
        fs.writeFileSync(a, fs.readFileSync(a, "utf8").replace("two\n", "deux\n"));
        lap(parent, "commit", "a.txt", "--branch", "main", "--no-session", "-i", "Translate the second line", "-b", "Rewrites two in French");
        lap(parent, "merge", "half");
        r = await ticketSessions(parent, "T-1");
        const half = r.value.filter((x) => x.msg === "T-1: work in half");
        assert.equal(half.length, 1);
        assert.equal(half[0].adoptedFrom, "half");
        assert.equal(half[0].stops?.[0].file, "a.txt");
        assert.match(half[0].stops?.[0].at ?? "", /^[0-9a-f]{64}$/);

        // feat's folder gone: the next write prunes it from the registry,
        // and its adopted session still names it, from its chunks here
        fs.rmSync(path.join(root, "feat"), { recursive: true });
        lap(parent, "session", "start", "unrelated", "--branch", "main");
        assert.doesNotMatch(fs.readFileSync(path.join(parent, ".lap", "branches.json"), "utf8"), /"feat"/);
        r = await ticketSessions(parent, "T-1");
        const kept = r.value.filter((x) => x.msg === "T-1: work in feat");
        assert.equal(kept.length, 1);
        assert.equal(kept[0].adoptedFrom, "feat");
    } finally {
        if (saved === undefined) delete process.env["LAP_BIN"];
        else process.env["LAP_BIN"] = saved;
    }
});

test("board_sessions: a branch's session and the parent's with the same id each get their own commits", { skip: !LAP && noCli("lap") }, async () => {
    const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "coboard-sessions-")));
    const parent = path.join(root, "proj");
    const feat = path.join(root, "feat");
    fs.mkdirSync(parent);
    const saved = process.env["LAP_BIN"];
    process.env["LAP_BIN"] = LAP;
    const lap = (cwd: string, ...args: string[]) => execFileSync(LAP, args, { cwd, env: { ...process.env, LAP_USER: "tester" } });
    try {
        lap(parent, "init");
        fs.writeFileSync(path.join(parent, "a.txt"), "one\ntwo\nthree\n");
        for (const f of ["a.txt", ".lapignore"]) lap(parent, "commit", f, "--no-session", "-i", "Seed the project files", "-b", `Records ${f} as it starts`);
        fs.cpSync(parent, feat, { recursive: true });
        lap(feat, "branch", "start", "feat", "--from", parent);
        const board = new Board(parent);
        const epic = board.create({ kind: "epic", title: "Work" });
        const inBranch = board.create({ kind: "ticket", title: "Done in the branch", epic: epic.id });
        const inParent = board.create({ kind: "ticket", title: "Done in the parent", epic: epic.id });
        const work = (dir: string, branch: string, ticket: string, to: string) => {
            lap(dir, "session", "start", `${ticket}: work`, "--meta", `ticket=${ticket}`, "--branch", branch);
            fs.writeFileSync(path.join(dir, "a.txt"), `one\n${to}\nthree\n`);
            lap(dir, "commit", "a.txt", "--branch", branch, "-i", `Change a.txt for ${ticket}`, "-b", `Rewrites line two as ${to}`);
        };
        work(feat, "feat", inBranch.id, "BRANCH");
        work(parent, "main", inParent.id, "PARENT");
        const read = async (ticket: string) => {
            const out = await handle({ jsonrpc: "2.0", id: 1, method: "tools/call", params: { name: "board_sessions", arguments: { ticket } } }, { cwd: parent, author: "agent" });
            const text = (out as { result: { content: { text: string }[] } }).result.content[0].text;
            return JSON.parse(text) as { sessions: { id: string; branch?: string; commits: { behavior: string }[] }[] };
        };
        const b = await read(inBranch.id);
        const p = await read(inParent.id);
        assert.equal(b.sessions.length, 1);
        assert.equal(p.sessions.length, 1);
        assert.equal(b.sessions[0].id, p.sessions[0].id, "both folders numbered their session alike");
        assert.equal(b.sessions[0].branch, "feat");
        assert.deepEqual(b.sessions[0].commits.map((c) => c.behavior), ["Rewrites line two as BRANCH"]);
        assert.deepEqual(p.sessions[0].commits.map((c) => c.behavior), ["Rewrites line two as PARENT"]);
    } finally {
        if (saved === undefined) delete process.env["LAP_BIN"];
        else process.env["LAP_BIN"] = saved;
    }
});

test("ticketSessions: two branches sharing a name are each read by their id", { skip: !LAP && noCli("lap") }, async () => {
    const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "coboard-sessions-")));
    const parent = path.join(root, "proj");
    fs.mkdirSync(parent);
    const saved = process.env["LAP_BIN"];
    process.env["LAP_BIN"] = LAP;
    const lap = (cwd: string, ...args: string[]) => execFileSync(LAP, args, { cwd, env: { ...process.env, LAP_USER: "tester" } });
    try {
        lap(parent, "init");
        fs.writeFileSync(path.join(parent, "a.txt"), "one\n");
        for (const f of ["a.txt", ".lapignore"]) lap(parent, "commit", f, "--no-session", "-i", "Seed the project files", "-b", `Records ${f} as it starts`);
        for (const b of ["x", "y"]) {
            const dir = path.join(root, b);
            fs.cpSync(parent, dir, { recursive: true });
            lap(dir, "branch", "start", b, "--from", parent);
            lap(dir, "session", "start", `T-1: work in ${b}`, "--meta", "ticket=T-1", "--branch", b);
        }
        // a name given twice, as lap allowed before it checked nested branches
        const reg = path.join(parent, ".lap", "branches.json");
        fs.writeFileSync(reg, fs.readFileSync(reg, "utf8").replace(/"name": *"y"/, '"name":"x"'));
        const ids = ["x", "y"].map((b) => fs.readFileSync(path.join(root, b, ".lap", "lineage"), "utf8").trim());
        const r = await ticketSessions(parent, "T-1");
        assert.equal(r.ok, true, r.error);
        assert.deepEqual(r.value.map((x) => `${x.branch}:${x.msg}`).sort(), [
            `${ids[0]}:T-1: work in x`,
            `${ids[1]}:T-1: work in y`,
        ].sort());
    } finally {
        if (saved === undefined) delete process.env["LAP_BIN"];
        else process.env["LAP_BIN"] = saved;
    }
});

test("startSession: records where lap says this folder does — main despite a leaked lineage, a branch folder's branch, main among branches", { skip: !LAP && noCli("lap") }, async () => {
    const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "coboard-sessions-")));
    const parent = path.join(root, "proj");
    const feat = path.join(root, "feat");
    fs.mkdirSync(parent);
    const saved = process.env["LAP_BIN"];
    const savedBranch = process.env["LAP_BRANCH"];
    process.env["LAP_BIN"] = LAP;
    delete process.env["LAP_BRANCH"];
    const lap = (cwd: string, ...args: string[]) => execFileSync(LAP, args, { cwd, env: { ...process.env, LAP_USER: "tester" } });
    const current = (cwd: string) => JSON.parse(String(lap(cwd, "session", "current", "--json"))) as { session: { ref?: string; id: string } | null };
    try {
        lap(parent, "init");
        fs.writeFileSync(path.join(parent, "a.txt"), "one\n");
        for (const f of ["a.txt", ".lapignore"]) lap(parent, "commit", f, "--no-session", "-i", "Seed the project files", "-b", `Records ${f} as it starts`);
        // no branches: nothing to say, and the session starts
        assert.deepEqual(await branchArgs(parent), []);
        await startSession(parent, "T-1", "T-1: before branches");
        lap(parent, "session", "end");
        fs.cpSync(parent, feat, { recursive: true });
        lap(feat, "branch", "start", "feat", "--from", parent);
        const id = fs.readFileSync(path.join(feat, ".lap", "lineage"), "utf8").trim();
        // the branch folder records to its branch
        assert.deepEqual(await branchArgs(feat), ["--branch", id]);
        await startSession(feat, "T-2", "T-2: in the branch");
        assert.match(current(feat).session?.ref ?? "", /^feat\//);
        // the parent, with branches, records to main
        assert.deepEqual(await branchArgs(parent), ["--branch", "main"]);
        // ... and still does once the branch's lineage leaked into it
        fs.copyFileSync(path.join(feat, ".lap", "lineage"), path.join(parent, ".lap", "lineage"));
        fs.copyFileSync(path.join(feat, ".lap", "parent"), path.join(parent, ".lap", "parent"));
        assert.deepEqual(await branchArgs(parent), ["--branch", "main"]);
        await startSession(parent, "T-3", "T-3: on main after the leak");
        assert.match(current(parent).session?.ref ?? "", /^S\d+$/, "a main session");
        // LAP_BRANCH already says it
        process.env["LAP_BRANCH"] = "feat";
        assert.deepEqual(await branchArgs(feat), []);
    } finally {
        if (saved === undefined) delete process.env["LAP_BIN"];
        else process.env["LAP_BIN"] = saved;
        if (savedBranch === undefined) delete process.env["LAP_BRANCH"];
        else process.env["LAP_BRANCH"] = savedBranch;
    }
});

test("ticketSessions: a branch whose history cannot be read is named in the error, its sessions not silently dropped", { skip: (!LAP && noCli("lap")) || process.getuid?.() === 0 }, async () => {
    const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "coboard-sessions-")));
    const parent = path.join(root, "proj");
    const feat = path.join(root, "feat");
    fs.mkdirSync(parent);
    const saved = process.env["LAP_BIN"];
    process.env["LAP_BIN"] = LAP;
    const lap = (cwd: string, ...args: string[]) => execFileSync(LAP, args, { cwd, env: { ...process.env, LAP_USER: "tester" } });
    try {
        lap(parent, "init");
        fs.writeFileSync(path.join(parent, "a.txt"), "one\n");
        for (const f of ["a.txt", ".lapignore"]) lap(parent, "commit", f, "--no-session", "-i", "Seed the project files", "-b", `Records ${f} as it starts`);
        lap(parent, "session", "start", "T-1: on main", "--meta", "ticket=T-1", "--branch", "main");
        lap(parent, "session", "end");
        fs.cpSync(parent, feat, { recursive: true });
        lap(feat, "branch", "start", "feat", "--from", parent);
        lap(feat, "session", "start", "T-1: in feat", "--meta", "ticket=T-1", "--branch", "feat");
        fs.chmodSync(path.join(feat, ".lap", "log"), 0o000);
        const r = await ticketSessions(parent, "T-1");
        fs.chmodSync(path.join(feat, ".lap", "log"), 0o755);
        assert.equal(r.ok, false);
        assert.match(r.error ?? "", /^branch feat: .*cannot be reached \(no permission on its folder\)/);
        assert.deepEqual(r.value.map((s) => s.msg), ["T-1: on main"], "what could be read is still given");
    } finally {
        if (saved === undefined) delete process.env["LAP_BIN"];
        else process.env["LAP_BIN"] = saved;
    }
});

test("board_sessions: a session whose commits could not be read is named in lapError", { skip: process.platform === "win32" && "the stand-in lap is a #! script, which Windows cannot start" }, async () => {
    const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "coboard-sessions-")));
    /* a lap that lists one session and cannot read its commits */
    const fake = path.join(root, "fake-lap");
    fs.writeFileSync(
        fake,
        `#!/bin/sh
case "$1 $2" in
"session list") echo '{"ok":true,"sessions":[{"id":"S1","msg":"T-1: work","started":"2026-09-28T00:00:00Z","ended":null,"commits":1,"active":false}]}' ;;
"branch list") echo '{"ok":true,"self":null,"branches":[]}' ;;
*) echo '{"ok":false,"error":"history_broken","message":"history chunk main.000002.jsonl is missing"}'; exit 1 ;;
esac
`,
    );
    fs.chmodSync(fake, 0o755);
    const saved = process.env["LAP_BIN"];
    process.env["LAP_BIN"] = fake;
    try {
        const board = new Board(root);
        const epic = board.create({ kind: "epic", title: "Work" });
        const t = board.create({ kind: "ticket", title: "A ticket", epic: epic.id });
        const out = await handle({ jsonrpc: "2.0", id: 1, method: "tools/call", params: { name: "board_sessions", arguments: { ticket: t.id } } }, { cwd: root, author: "agent" });
        const text = (out as { result: { content: { text: string }[] } }).result.content[0].text;
        const got = JSON.parse(text) as { sessions: { id: string; commits: unknown[] }[]; lapError?: string };
        assert.equal(got.sessions.length, 1);
        assert.deepEqual(got.sessions[0].commits, []);
        assert.equal(got.lapError, "session S1: history chunk main.000002.jsonl is missing");
    } finally {
        if (saved === undefined) delete process.env["LAP_BIN"];
        else process.env["LAP_BIN"] = saved;
    }
});
