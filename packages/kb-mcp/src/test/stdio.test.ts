/* The server as a process, spoken to over a real pipe.
 *
 * WHAT THIS ADDS OVER EVERY OTHER FILE HERE. Those drive modules; this drives
 * the thing an agent host actually launches. The failures it can find are the
 * ones that live in the wiring and nowhere else: a `bin` script that requires
 * the wrong path, an entry point that never starts, a process that writes a
 * banner to stdout before the first message, one that exits while a call is in
 * flight. None of those are reachable from an import.
 *
 * `KB_BIN` POINTS AT THE FAKE AND THE WORKING DIRECTORY IS A THROWAWAY, so
 * nothing here can reach a real store. A test that filed into whatever `.kb/`
 * sits above the checkout would put research under `win32-iocp` on somebody's
 * machine every time it ran.
 */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { spawn } from "node:child_process";
import { test } from "node:test";

import { FakeKb, ok } from "./fake";

const ENTRY = path.resolve(__dirname, "..", "..", "bin", "kb-mcp");

interface Session {
    stdout: string;
    stderr: string;
    code: number | null;
}

/* Starts the server, hands `write` the stdin stream, and reads until it ends.
 * The callback decides how the bytes are cut up, which is the whole point. */
function session(fake: FakeKb, write: (stdin: NodeJS.WritableStream) => void): Promise<Session> {
    const cwd = fs.mkdtempSync(path.join(os.tmpdir(), "kb-mcp-cwd-"));
    return new Promise<Session>((resolve, reject) => {
        const child = spawn(process.execPath, [ENTRY], {
            cwd,
            env: { ...fake.env(), KB_BIN: fake.bin },
            stdio: ["pipe", "pipe", "pipe"],
        });
        let out = "";
        let err = "";
        child.stdout.setEncoding("utf8");
        child.stderr.setEncoding("utf8");
        child.stdout.on("data", (d: string) => (out += d));
        child.stderr.on("data", (d: string) => (err += d));
        child.on("error", reject);
        child.on("close", (code) => {
            fs.rmSync(cwd, { recursive: true, force: true });
            resolve({ stdout: out, stderr: err, code });
        });
        write(child.stdin);
    });
}

function lines(text: string): Record<string, unknown>[] {
    return text
        .split("\n")
        .filter((l) => l.length > 0)
        .map((l) => JSON.parse(l) as Record<string, unknown>);
}

test("a whole session over a pipe: handshake, list, call", async () => {
    const fake = FakeKb.create([{ stdout: ok({ collections: [], count: 0 }) }]);
    try {
        const { stdout, stderr, code } = await session(fake, (stdin) => {
            /* All four messages in one write, so the server has to find three
             * boundaries inside one chunk. */
            stdin.end(
                [
                    { jsonrpc: "2.0", id: 1, method: "initialize", params: { protocolVersion: "2024-11-05" } },
                    { jsonrpc: "2.0", method: "notifications/initialized" },
                    { jsonrpc: "2.0", id: 2, method: "tools/list" },
                    {
                        jsonrpc: "2.0",
                        id: 3,
                        method: "tools/call",
                        params: { name: "kb_collections", arguments: {} },
                    },
                ]
                    .map((m) => `${JSON.stringify(m)}\n`)
                    .join(""),
            );
        });
        const answers = lines(stdout);
        /* Three, not four: the notification is owed nothing. */
        assert.deepEqual(answers.map((a) => a["id"]), [1, 2, 3]);
        assert.equal(
            ((answers[1]["result"] as Record<string, unknown>)["tools"] as unknown[]).length,
            6,
        );
        assert.equal(stderr, "", `the server wrote diagnostics: ${stderr}`);
        assert.equal(code, 0);
    } finally {
        fake.dispose();
    }
});

test("a request cut in half by the pipe is answered once and correctly", async () => {
    /* The two halves are written with a tick between them, so they arrive as
     * two `data` events rather than being coalesced. */
    const fake = FakeKb.create([{ stdout: ok({ collections: [], count: 0 }) }]);
    try {
        const message = `${JSON.stringify({
            jsonrpc: "2.0",
            id: 11,
            method: "tools/call",
            params: { name: "kb_collections", arguments: {} },
        })}\n`;
        const { stdout } = await session(fake, (stdin) => {
            stdin.write(message.slice(0, 30));
            setTimeout(() => stdin.end(message.slice(30)), 20);
        });
        const answers = lines(stdout);
        assert.equal(answers.length, 1);
        assert.equal(answers[0]["id"], 11);
        assert.equal("error" in answers[0], false);
        assert.deepEqual(fake.calls()[0].argv, ["collections", "--json"]);
    } finally {
        fake.dispose();
    }
});

test("stdout carries nothing but messages, from the first byte", async () => {
    /* A banner, a progress line, a stray console.log: any of them lands in the
     * middle of the protocol, and what a client does with it ranges from
     * ignoring it to closing the session. What it never does is say which
     * module printed. */
    const fake = FakeKb.create([{ stdout: ok({ collections: [], count: 0 }) }]);
    try {
        const { stdout } = await session(fake, (stdin) => {
            stdin.end(`${JSON.stringify({ jsonrpc: "2.0", id: 1, method: "ping" })}\n`);
        });
        assert.equal(stdout, `${JSON.stringify({ jsonrpc: "2.0", id: 1, result: {} })}\n`);
    } finally {
        fake.dispose();
    }
});

test("stdin that ends with nothing in it exits cleanly", async () => {
    const fake = FakeKb.create([]);
    try {
        const { stdout, stderr, code } = await session(fake, (stdin) => stdin.end());
        assert.equal(stdout, "");
        assert.equal(stderr, "");
        assert.equal(code, 0);
    } finally {
        fake.dispose();
    }
});

test("the process does not exit while a call is still in flight", async () => {
    /* stdin ends the moment the request is written; the answer still has to
     * come back. A server that resolved on `end` rather than on the queue
     * draining would close the pipe on its own reply. */
    const fake = FakeKb.create([{ stdout: ok({ collections: [], count: 0 }) }]);
    try {
        const { stdout } = await session(fake, (stdin) => {
            stdin.end(
                `${JSON.stringify({
                    jsonrpc: "2.0",
                    id: 21,
                    method: "tools/call",
                    params: { name: "kb_collections", arguments: {} },
                })}\n`,
            );
        });
        assert.equal(lines(stdout).length, 1);
        assert.equal(lines(stdout)[0]["id"], 21);
    } finally {
        fake.dispose();
    }
});

test("malformed bytes are answered and the session carries on", async () => {
    const fake = FakeKb.create([]);
    try {
        const { stdout, code } = await session(fake, (stdin) => {
            stdin.write("this is not JSON\n");
            stdin.end(`${JSON.stringify({ jsonrpc: "2.0", id: 2, method: "ping" })}\n`);
        });
        const answers = lines(stdout);
        assert.equal(answers.length, 2);
        assert.equal((answers[0]["error"] as Record<string, unknown>)["code"], -32700);
        assert.equal(answers[1]["id"], 2);
        assert.equal(code, 0);
    } finally {
        fake.dispose();
    }
});
