/* The one place this package starts a process.
 *
 * `spawn` WITH AN ARGV ARRAY AND NO SHELL, AND THAT IS THE WHOLE SECURITY
 * ARGUMENT. There is no command string anywhere in this package for a value to
 * be interpolated into, so a title, a collection name or a query a reader typed
 * cannot become syntax: `io_uring; rm -rf ~` is one argument containing a
 * semicolon. `exec` and `execSync` take a line and hand it to `/bin/sh`, so
 * neither is used and `guards.test.ts` refuses both by name, along with
 * `shell: true` and a backticked command.
 *
 * `--json` IS ADDED HERE, EXACTLY ONCE. Every command takes it (§10's table
 * says so), no builder in `argv.ts` writes it, and a second copy would be a
 * flag the CLI's parser has to decide about. Adding it in one place also means
 * a caller cannot ask for the human output by accident and get a parse failure
 * instead of an answer.
 *
 * THE EXIT CODE DECIDES WHAT KIND OF FAILURE IT IS. §10: `0` success, `1` user
 * or store error, `2` internal failure. A `1` carries §11's payload and becomes
 * a `KbError`; a `2`, a binary that would not start, and an answer this layer
 * cannot read all become a `KbCrash`. Folding them together is what would make
 * a bug in kb look like a question the reader asked wrong.
 *
 * ASYNCHRONOUS, BECAUSE THE CALLER IS A UI. §2 of the UI spec searches as the
 * reader types; a synchronous spawn per keystroke would block the extension
 * host, and a host that is blocked cannot repaint the box being typed into.
 */

import { spawn } from "node:child_process";

import { KbCrash, KbError } from "./errors";

/* The binary, and where it should run.
 *
 * `cwd` IS LOAD-BEARING AND IS NOT A CONVENIENCE. §1.4 finds the store
 * by walking up from the working directory, like `.git` — so the directory
 * this process happens to have been started in decides which store a read
 * spans. A binding that left it to chance would answer differently depending
 * on how the editor was launched. */
export interface KbOptions {
    /* The `kb` executable. A bare name is resolved through PATH by the
     * platform's own exec, not by this package — there is no search here to
     * get wrong. */
    bin?: string;
    cwd?: string;
    env?: NodeJS.ProcessEnv;
    /* A search that never answers must not leave a UI waiting for ever. */
    timeoutMs?: number;
    /* What a single answer may weigh. `kb get --include text` on the largest
     * document the store accepts is 64 MiB of content plus its escaping, so
     * the default has room for one of those and refuses anything that could
     * only be a runaway. */
    maxBytes?: number;
}

export const DEFAULT_BIN = "kb";
export const DEFAULT_TIMEOUT_MS = 30_000;
export const DEFAULT_MAX_BYTES = 192 * 1024 * 1024;

interface Completed {
    code: number | null;
    signal: NodeJS.Signals | null;
    stdout: string;
    stderr: string;
}

/* One process, its output, and the three ways it can end badly: it never
 * started, it wrote more than a caller can hold, or it did not answer. */
function spawnKb(
    argv: readonly string[],
    options: KbOptions,
    stdin: string | null,
): Promise<Completed> {
    const bin = options.bin ?? DEFAULT_BIN;
    const timeoutMs = options.timeoutMs ?? DEFAULT_TIMEOUT_MS;
    const maxBytes = options.maxBytes ?? DEFAULT_MAX_BYTES;
    return new Promise<Completed>((resolve, reject) => {
        const child = spawn(bin, [...argv], {
            cwd: options.cwd,
            env: options.env,
            /* Stated rather than left to the default. A `true` here would put
             * a shell between this argv and the binary and undo every
             * guarantee `argv.ts` makes. */
            shell: false,
            windowsHide: true,
            stdio: ["pipe", "pipe", "pipe"],
        });

        const out: Buffer[] = [];
        const err: Buffer[] = [];
        let outBytes = 0;
        let errBytes = 0;
        let settled = false;

        const finish = (fn: () => void): void => {
            if (settled) {
                return;
            }
            settled = true;
            clearTimeout(timer);
            fn();
        };

        const timer = setTimeout(() => {
            finish(() => {
                child.kill("SIGKILL");
                reject(
                    new KbCrash(
                        `kb did not answer within ${timeoutMs}ms.`,
                        argv,
                        null,
                        Buffer.concat(err).toString("utf8").trim(),
                    ),
                );
            });
        }, timeoutMs);
        /* A pending timer keeps Node alive; a caller that has already been
         * answered should not be held open by this one. */
        timer.unref?.();

        const overflow = (): void => {
            finish(() => {
                child.kill("SIGKILL");
                reject(
                    new KbCrash(
                        `kb wrote more than ${maxBytes} bytes, which is more than this reader will hold.`,
                        argv,
                        null,
                        "",
                    ),
                );
            });
        };

        child.stdout.on("data", (b: Buffer) => {
            outBytes += b.length;
            if (outBytes > maxBytes) {
                overflow();
                return;
            }
            out.push(b);
        });
        /* stderr is capped too, and against the same budget it would be read
         * with: a binary in a loop writing diagnostics is the same runaway. */
        child.stderr.on("data", (b: Buffer) => {
            errBytes += b.length;
            if (errBytes > maxBytes) {
                overflow();
                return;
            }
            err.push(b);
        });

        child.on("error", (e: NodeJS.ErrnoException) => {
            finish(() =>
                reject(
                    new KbCrash(
                        e.code === "ENOENT"
                            ? `kb is not on the PATH (looked for "${bin}").`
                            : `kb could not be started: ${e.message}`,
                        argv,
                        null,
                        "",
                    ),
                ),
            );
        });

        child.on("close", (code, signal) => {
            finish(() =>
                resolve({
                    code,
                    signal,
                    stdout: Buffer.concat(out).toString("utf8"),
                    stderr: Buffer.concat(err).toString("utf8"),
                }),
            );
        });

        if (stdin === null) {
            child.stdin.end();
        } else {
            /* A document handed over on stdin rather than in the argv: an
             * argument list has a hard size limit a page reaches long before
             * anybody notices, and a page's text does not belong in a process
             * listing. An EPIPE here is the child having exited first, which
             * the close handler above is already reporting. */
            child.stdin.on("error", () => undefined);
            child.stdin.end(stdin, "utf8");
        }
    });
}

function parse(text: string): Record<string, unknown> | null {
    let value: unknown;
    try {
        value = JSON.parse(text);
    } catch {
        return null;
    }
    return typeof value === "object" && value !== null && !Array.isArray(value)
        ? (value as Record<string, unknown>)
        : null;
}

function text(value: unknown, fallback: string): string {
    return typeof value === "string" && value.length > 0 ? value : fallback;
}

/* Run one command and answer its `{ok:true, ...}` payload.
 *
 * The envelope's own two keys are not stripped: `ok` is the only one, every
 * other key is the answer, and a caller reading `documents` off the object is
 * reading exactly what the CLI printed. */
export async function run(
    argv: readonly string[],
    options: KbOptions = {},
    stdin: string | null = null,
): Promise<Record<string, unknown>> {
    const full = [...argv, "--json"];
    const done = await spawnKb(full, options, stdin);
    const payload = parse(done.stdout);

    if (done.signal !== null) {
        throw new KbCrash(`kb was killed by ${done.signal}.`, full, null, done.stderr.trim());
    }

    if (done.code === 0) {
        if (payload === null || payload["ok"] !== true) {
            /* A zero exit with an unreadable answer is the binding and the
             * binary disagreeing about the contract, which is a fault and not
             * a refusal — there is no §11 code for "kb said something else". */
            throw new KbCrash(
                `kb succeeded and answered something this reader cannot parse.`,
                full,
                0,
                done.stderr.trim(),
            );
        }
        return payload;
    }

    if (done.code === 1) {
        if (payload === null || payload["ok"] !== false) {
            throw new KbCrash(
                `kb refused and did not say why in JSON; --json was passed and the answer was not an error envelope.`,
                full,
                1,
                done.stderr.trim(),
            );
        }
        /* `error` carries the code and `message` the prose. The code is taken
         * verbatim: `errors.ts` records why an unrecognised one is reported as
         * unrecognised rather than mapped onto something plausible. */
        const code = text(payload["error"], "");
        const message = text(payload["message"], "kb refused the request.");
        if (code === "") {
            throw new KbCrash(
                `kb refused without naming a code: ${message}`,
                full,
                1,
                done.stderr.trim(),
            );
        }
        throw new KbError(code, message, full);
    }

    /* Exit 2 is kb's own word for a fault, and anything else — a signal
     * already handled above, a shell's 127, a code nobody documented — is one
     * too as far as a caller is concerned. */
    const detail = payload === null ? done.stderr.trim() : text(payload["message"], "");
    throw new KbCrash(
        `kb failed (exit ${done.code === null ? "unknown" : done.code})${detail === "" ? "." : `: ${detail}`}`,
        full,
        done.code,
        done.stderr.trim(),
    );
}
