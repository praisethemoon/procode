/* A stand-in for the `kb` binary, so the tool layer can be driven without one.
 *
 * WHY A REAL PROCESS AND NOT A MOCKED `Kb`. What is being tested is the whole
 * path from a tool call to an argument list: that `collection` reaches the
 * store as `--collection`, that `kb_add` sends no `--store`, that a search
 * costs exactly one process. A stubbed client would test that this package
 * calls a method — the half that cannot break — and would answer "yes"
 * throughout the entire class of bug these tests exist to catch. The recorded
 * argv is the evidence.
 *
 * It is `kb-js`'s fixture in shape and for the same reasons, kept here rather
 * than imported because a package's test directory is not its published
 * surface, and a suite that reached into another package's internals would
 * break on a refactor that broke nothing.
 */

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

export interface FakeAnswer {
    stdout?: string;
    stderr?: string;
    exit?: number;
}

export interface FakeCall {
    argv: string[];
    stdin: string;
    cwd: string;
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
    JSON.stringify({ argv: process.argv.slice(2), stdin, cwd: process.cwd() }) + "\\n",
);
const plan = JSON.parse(fs.readFileSync(path.join(dir, "plan.json"), "utf8"));
const n = fs.readFileSync(path.join(dir, "calls.jsonl"), "utf8").trimEnd().split("\\n").length;
const answer = plan[Math.min(n - 1, plan.length - 1)] ?? {};
if (typeof answer.stdout === "string") process.stdout.write(answer.stdout);
if (typeof answer.stderr === "string") process.stderr.write(answer.stderr);
process.exit(typeof answer.exit === "number" ? answer.exit : 0);
`;

export class FakeKb {
    private constructor(
        readonly dir: string,
        readonly bin: string,
    ) {}

    static create(answers: readonly FakeAnswer[]): FakeKb {
        const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kb-mcp-fake-"));
        const bin = path.join(dir, "kb");
        fs.writeFileSync(bin, SCRIPT, { mode: 0o755 });
        fs.writeFileSync(path.join(dir, "plan.json"), JSON.stringify(answers));
        fs.writeFileSync(path.join(dir, "calls.jsonl"), "");
        return new FakeKb(dir, bin);
    }

    env(): NodeJS.ProcessEnv {
        return { ...process.env, KB_FAKE_DIR: this.dir };
    }

    calls(): FakeCall[] {
        const text = fs.readFileSync(path.join(this.dir, "calls.jsonl"), "utf8").trim();
        return text === "" ? [] : text.split("\n").map((l) => JSON.parse(l) as FakeCall);
    }

    dispose(): void {
        fs.rmSync(this.dir, { recursive: true, force: true });
    }
}

/* The CLI's two envelopes. `error` is the CODE and `message` the prose, which
 * is what `cmd_common.c` prints and is not what a reader of §11's table would
 * guess. */
export function ok(payload: Record<string, unknown>): string {
    return `${JSON.stringify({ ok: true, ...payload })}\n`;
}

export function refusal(code: string, message: string): string {
    return `${JSON.stringify({ ok: false, error: code, message })}\n`;
}
