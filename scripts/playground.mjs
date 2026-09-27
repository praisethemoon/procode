/* Fills .playground/ with a small project to look at in the test window:
 * a board with epics, milestones and tickets, lap sessions linked to those
 * tickets (with real edits), a few kb documents, and two artifacts.
 *
 * Each part is seeded only when it is empty, so running this again changes
 * nothing, and work done in the playground is kept. It never deletes
 * anything. Needs the CLIs built and coboard and artifacts compiled (the "playground" task
 * in .vscode/tasks.json does both first).
 *
 *   node scripts/playground.mjs
 */

import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import { createRequire } from "node:module";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const dir = path.join(repo, ".playground");
const LAP = path.join(repo, "cli/lap-cli/bin/lap");
const KB = path.join(repo, "cli/kb-cli/bin/kb");
const { Board } = createRequire(import.meta.url)(path.join(repo, "packages/coboard/out/index.js"));
const { Artifacts } = createRequire(import.meta.url)(path.join(repo, "packages/artifacts/out/index.js"));

fs.mkdirSync(dir, { recursive: true });
const env = { ...process.env, LAP_USER: "claude" };
const lap = (...args) => execFileSync(LAP, args, { cwd: dir, env, encoding: "utf8" });
const kb = (args, input) => execFileSync(KB, args, { cwd: dir, input, encoding: "utf8" });
const write = (file, text) => {
    fs.mkdirSync(path.dirname(path.join(dir, file)), { recursive: true });
    fs.writeFileSync(path.join(dir, file), text);
};

/* ---------------------------------------------------------------- stores */

if (!fs.existsSync(path.join(dir, ".lap"))) lap("init");
if (!fs.existsSync(path.join(dir, ".kb"))) kb(["init"]);
// The board, the knowledge base and the artifacts are not source; lap should
// not track them.
const ignore = path.join(dir, ".lapignore");
const ignored = fs.readFileSync(ignore, "utf8");
if (!ignored.includes(".coboard/")) fs.appendFileSync(ignore, "\n# stores, not source\n.coboard/\n.kb/\n");
if (!ignored.includes(".artifact/")) fs.appendFileSync(ignore, ".artifact/\n");

/* ----------------------------------------------------------------- board */

const board = new Board(dir);
if (board.all().length === 0) {
    const e1 = board.create({
        kind: "epic",
        title: "Windows backend",
        description:
            "Port the event loop to **IOCP** so the server runs natively on Windows.\n\n" +
            "The Linux side (E-2) is the reference; behaviour must match it.\n\n" +
            "- accept, read and write through completion ports\n- clean cancellation on shutdown",
    });
    const e2 = board.create({ kind: "epic", title: "Linux io_uring backend", description: "Move from epoll to io_uring. See T-5." });
    const m1 = board.create({ kind: "milestone", title: "Accept & read", epic: e1.id, description: "The first request served end to end." });
    const m2 = board.create({ kind: "milestone", title: "Shutdown", epic: e1.id });
    board.create({
        kind: "ticket",
        title: "Accept loop on IOCP",
        milestone: m1.id,
        status: "done",
        size: "m",
        priority: "high",
        assignee: "claude",
        labels: ["windows", "net"],
        description: "Use `AcceptEx` with a pre-created socket.\n\n```c\nAcceptEx(listener, sock, buf, 0, addr_len, addr_len, &got, &ov);\n```",
    });
    board.create({
        kind: "ticket",
        title: "Read path with overlapped WSARecv",
        milestone: m1.id,
        status: "doing",
        size: "l",
        priority: "high",
        assignee: "claude",
        labels: ["windows", "net"],
        description: "Post one `WSARecv` per connection and re-arm on completion.\n\nNeeds T-1.",
    });
    board.create({
        kind: "ticket",
        title: "Cancel pending I/O on shutdown",
        milestone: m2.id,
        status: "blocked",
        size: "s",
        priority: "medium",
        description: "`CancelIoEx` everything, then drain the port. Blocked until T-2 lands.",
    });
    board.create({ kind: "ticket", title: "Write the design note", epic: e1.id, status: "todo", size: "xs", priority: "low", labels: ["docs"] });
    board.create({
        kind: "ticket",
        title: "Spike: io_uring accept",
        epic: e2.id,
        status: "review",
        size: "s",
        assignee: "ana",
        description: "Compare `io_uring_prep_accept` with the epoll loop. Mirrors T-1.",
    });
    board.comment("T-1", "Done: the loop accepts and hands sockets to the reader. See the lap session for the edits.", "claude");
    board.comment("T-2", "Started. Re-arming happens in the completion handler.", "claude");
    board.comment("T-2", "Looks good so far — mind the zero-byte read on close.", "ana");
    console.log("board: seeded E-1..E-2, M-1..M-2, T-1..T-5");
}

/* ------------------------------------------------------------------- lap */

const sessions = JSON.parse(lap("session", "list", "--json")).sessions;
if (sessions.length === 0) {
    if (JSON.parse(lap("session", "current", "--json")).session) lap("session", "end");

    // T-1: two edits to the server, one to the header.
    lap("session", "start", "T-1: accept loop on IOCP", "--meta", "ticket=T-1");
    write("src/server.h", "#pragma once\n\nint server_run(int port);\n");
    lap("commit", "src/server.h", "-i", "Give the IOCP accept loop a home.", "-b", "Declares server_run(port), the server's one entry point.");
    write("src/server.c", '#include "server.h"\n\nint server_run(int port) {\n    (void)port;\n    return 0;\n}\n');
    lap("commit", "src/server.c", "-i", "Give the IOCP accept loop a home.", "-b", "Defines server_run as a stub that returns 0, so the loop has a place to live.");
    write(
        "src/server.c",
        '#include "server.h"\n\nint server_run(int port) {\n    SOCKET listener = listen_on(port);\n    for (;;) {\n        accept_one(listener); /* AcceptEx under the hood */\n    }\n}\n',
    );
    lap("commit", "src/server.c", "-i", "Accept connections forever on IOCP.", "-b", "Loops on accept_one over the listener; AcceptEx hands each socket to the reader.");
    lap("session", "end");

    // T-2: in progress, still open.
    lap("session", "start", "T-2: overlapped reads", "--meta", "ticket=T-2");
    write("src/read.c", "/* one WSARecv in flight per connection */\nvoid read_arm(Conn *c) {\n    WSARecv(c->sock, &c->buf, 1, NULL, &c->flags, &c->ov, NULL);\n}\n");
    lap("commit", "src/read.c", "-i", "Keep one overlapped read in flight per connection.", "-b", "read_arm posts the first WSARecv; the completion handler re-arms it.");
    lap("session", "end");

    // Work not tied to any ticket, to show it is not picked up.
    lap("session", "start", "tidy the readme");
    write("README.md", "# playground\n\nA toy server for trying the Board, Knowledge and Lap History.\n");
    lap("commit", "README.md", "-i", "Say what this folder is.", "-b", "Adds a README naming the toy server and what it is for.");
    lap("commit", ".lapignore", "-i", "Keep the board and the knowledge base out of lap.", "-b", "Ignores .coboard and .kb so their stores are never recorded as edits.");
    lap("session", "end");
    console.log("lap: seeded S1 (T-1), S2 (T-2), S3 (no ticket)");
}

/* -------------------------------------------------------------------- kb */

if (JSON.parse(kb(["ls", "--json"])).count === 0) {
    const docs = [
        ["I/O completion ports", "win32-iocp", "# I/O completion ports\n\nA completion port queues finished I/O. `CreateIoCompletionPort` makes one; `GetQueuedCompletionStatus` waits on it.\n\n## AcceptEx\n\n`AcceptEx` accepts into a socket created beforehand, so the accept itself is overlapped."],
        ["io_uring basics", "io-uring", "# io_uring\n\nTwo rings shared with the kernel: submissions and completions.\n\n## Accept\n\n`io_uring_prep_accept` queues an accept; the result arrives as a CQE."],
        ["kqueue", "bsd", "# kqueue\n\n`EVFILT_READ` fires when a descriptor is readable. It reports readiness, where IOCP and io_uring report completion."],
    ];
    for (const [title, collection, text] of docs) {
        kb(["add", "--title", title, "--collection", collection, "--mime", "text/markdown", "--json"], text);
    }
    console.log("kb: seeded 3 documents");
}

/* ------------------------------------------------------------- artifacts */

const artifacts = new Artifacts(dir);
if (artifacts.list().length === 0) {
    artifacts.publish(
        {
            title: "Accepting connections: IOCP, io_uring, kqueue",
            description: "How each API accepts a connection without blocking a thread, from the kb documents D-1 to D-3.",
            html: `<h1>Accepting connections</h1>
<p class="muted">From the knowledge base: D-1 (IOCP), D-2 (io_uring), D-3 (kqueue).</p>
<p>All three let one thread wait on many sockets. Two report <strong>completion</strong>, one reports <strong>readiness</strong>.</p>
<table>
<thead><tr><th>API</th><th>Accept</th><th>Model</th></tr></thead>
<tbody>
<tr><td>IOCP</td><td><code>AcceptEx</code> into a socket made beforehand</td><td>completion</td></tr>
<tr><td>io_uring</td><td><code>io_uring_prep_accept</code>, result as a CQE</td><td>completion</td></tr>
<tr><td>kqueue</td><td><code>EVFILT_READ</code> on the listener, then <code>accept</code></td><td>readiness</td></tr>
</tbody>
</table>
<h2>What it means for the server</h2>
<blockquote>With completion APIs the buffer is committed when the request is made; with readiness the read happens after the wake-up.</blockquote>`,
        },
        new Date(Date.now() - 2 * 3600 * 1000),
    );
    artifacts.publish({
        title: "T-2 progress",
        description: "Where the overlapped-read ticket stands, with a small status chart.",
        html: `<!doctype html><html><head><style>
.bar { height: var(--bk-spacing-3); border-radius: var(--bk-radius-full); background: var(--bk-color-background-secondary); overflow: hidden; }
.bar > span { display: block; height: 100%; background: var(--bk-color-primary); }
.row { display: grid; grid-template-columns: 10rem 1fr 3rem; gap: var(--bk-gap-md); align-items: center; margin-bottom: var(--bk-spacing-2); }
</style></head><body>
<h1>T-2: overlapped reads</h1>
<div class="row"><span>read arming</span><div class="bar"><span style="width:80%"></span></div><small>80%</small></div>
<div class="row"><span>re-arm on completion</span><div class="bar"><span style="width:35%"></span></div><small>35%</small></div>
<div class="row"><span>tests</span><div class="bar"><span style="width:10%"></span></div><small>10%</small></div>
<p id="note" class="muted"></p>
<script>document.getElementById("note").textContent = "Rendered " + new Date().toLocaleString() + " — scripts run inside the sandbox.";</script>
</body></html>`,
    });
    console.log("artifacts: seeded A-1, A-2");
}

console.log(`playground ready: ${dir}`);
