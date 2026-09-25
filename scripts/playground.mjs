/* Fills .playground/ with a small project to look at in the test window:
 * a board with epics, milestones and tickets, lap sessions linked to those
 * tickets (with real edits), and a few kb documents.
 *
 * Each part is seeded only when it is empty, so running this again changes
 * nothing, and work done in the playground is kept. It never deletes
 * anything. Needs the CLIs built and coboard compiled (the "playground" task
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
// The board and the knowledge base are not source; lap should not track them.
const ignore = path.join(dir, ".lapignore");
const ignored = fs.readFileSync(ignore, "utf8");
if (!ignored.includes(".coboard/")) fs.appendFileSync(ignore, "\n# stores, not source\n.coboard/\n.kb/\n");

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
    lap("commit", "src/server.h", "-m", "the server's one entry point");
    write("src/server.c", '#include "server.h"\n\nint server_run(int port) {\n    (void)port;\n    return 0;\n}\n');
    lap("commit", "src/server.c", "-m", "a server that does nothing yet, so the loop has a place to live");
    write(
        "src/server.c",
        '#include "server.h"\n\nint server_run(int port) {\n    SOCKET listener = listen_on(port);\n    for (;;) {\n        accept_one(listener); /* AcceptEx under the hood */\n    }\n}\n',
    );
    lap("commit", "src/server.c", "-m", "accept forever: AcceptEx hands each socket to the reader");
    lap("session", "end");

    // T-2: in progress, still open.
    lap("session", "start", "T-2: overlapped reads", "--meta", "ticket=T-2");
    write("src/read.c", "/* one WSARecv in flight per connection */\nvoid read_arm(Conn *c) {\n    WSARecv(c->sock, &c->buf, 1, NULL, &c->flags, &c->ov, NULL);\n}\n");
    lap("commit", "src/read.c", "-m", "post the first read; completion re-arms it");
    lap("session", "end");

    // Work not tied to any ticket, to show it is not picked up.
    lap("session", "start", "tidy the readme");
    write("README.md", "# playground\n\nA toy server for trying the Board, Knowledge and Lap History.\n");
    lap("commit", "README.md", "-m", "say what this folder is");
    lap("commit", ".lapignore", "-m", "keep the board and the knowledge base out of lap");
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

console.log(`playground ready: ${dir}`);
