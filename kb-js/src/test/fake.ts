/* A stand-in for the `kb` binary, so the spawn path can be tested without one.
 *
 * WHY A REAL PROCESS AND NOT A MOCK. The thing being tested IS the process
 * boundary: that an argument arrives as one argument, that stdin carries the
 * document, that an exit code becomes the right kind of error. A mocked
 * `spawn` would test that this package calls a function — which is the half
 * that cannot break — and would have answered "yes" throughout the entire
 * class of bug this file exists to catch.
 *
 * IT RECORDS WHAT IT WAS GIVEN, which is how the injection tests are stated as
 * facts rather than as intentions: a query containing `; rm -rf ~/dummy` is asserted
 * to arrive as ONE element of `process.argv`, whole, with the semicolon in it.
 * A package that had built a command string would fail that assertion by
 * arriving as several elements, or not arriving at all.
 *
 * The script is written to a temporary directory at run time rather than
 * checked in, because `tsc` compiles `.ts` and a fixture that needed a shebang
 * and an executable bit could not be one of its outputs.
 */

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

/* One answer the fake gives back. Written as the three things a process has to
 * say: what it printed, what it complained about, and how it ended. */
export interface FakeAnswer {
    stdout?: string;
    stderr?: string;
    exit?: number;
    /* Milliseconds to wait before answering, for the timeout path. */
    delayMs?: number;
}

export interface FakeCall {
    argv: string[];
    stdin: string;
    cwd: string;
    env: Record<string, string>;
}

const SCRIPT = `#!/usr/bin/env node
"use strict";
const fs = require("node:fs");
const path = require("node:path");
const dir = process.env["KB_FAKE_DIR"];
let stdin = "";
try {
    stdin = fs.readFileSync(0, "utf8");
} catch {
    stdin = "";
}
fs.appendFileSync(
    path.join(dir, "calls.jsonl"),
    JSON.stringify({
        argv: process.argv.slice(2),
        stdin,
        cwd: process.cwd(),
        env: { KB_STORE: process.env["KB_STORE"] ?? "" },
    }) + "\\n",
);
const plan = JSON.parse(fs.readFileSync(path.join(dir, "plan.json"), "utf8"));
const n = fs.readFileSync(path.join(dir, "calls.jsonl"), "utf8").trimEnd().split("\\n").length;
const answer = plan[Math.min(n - 1, plan.length - 1)] ?? {};
const emit = () => {
    if (typeof answer.stdout === "string") process.stdout.write(answer.stdout);
    if (typeof answer.stderr === "string") process.stderr.write(answer.stderr);
    process.exit(typeof answer.exit === "number" ? answer.exit : 0);
};
if (typeof answer.delayMs === "number" && answer.delayMs > 0) {
    setTimeout(emit, answer.delayMs);
} else {
    emit();
}
`;

export class FakeKb {
    private constructor(
        readonly dir: string,
        readonly bin: string,
    ) {}

    static create(answers: readonly FakeAnswer[]): FakeKb {
        const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kb-js-fake-"));
        const bin = path.join(dir, "kb");
        fs.writeFileSync(bin, SCRIPT, { mode: 0o755 });
        fs.writeFileSync(path.join(dir, "plan.json"), JSON.stringify(answers));
        fs.writeFileSync(path.join(dir, "calls.jsonl"), "");
        return new FakeKb(dir, bin);
    }

    /* The environment a client has to be given for the fake to find its own
     * plan. Spelled here so a test cannot forget it and get a crash about a
     * missing directory instead of the failure it was looking for. */
    env(extra: Record<string, string> = {}): NodeJS.ProcessEnv {
        return { ...process.env, KB_FAKE_DIR: this.dir, ...extra };
    }

    calls(): FakeCall[] {
        const text = fs.readFileSync(path.join(this.dir, "calls.jsonl"), "utf8").trim();
        return text === "" ? [] : text.split("\n").map((l) => JSON.parse(l) as FakeCall);
    }

    dispose(): void {
        fs.rmSync(this.dir, { recursive: true, force: true });
    }
}

/* An `{ok:true, ...}` envelope, the way every command answers one. */
export function ok(payload: Record<string, unknown>): string {
    return `${JSON.stringify({ ok: true, ...payload })}\n`;
}

/* The CLI's refusal envelope — `error` is the CODE and `message` the prose,
 * which is the shape `cmd_common.c`'s `err_out` prints and is not the shape a
 * reader of §11's table would guess. */
export function refusal(code: string, message: string): string {
    return `${JSON.stringify({ ok: false, error: code, message })}\n`;
}
