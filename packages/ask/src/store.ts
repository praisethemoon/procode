/* Where forms live and how the server hands one to the editor and waits
 * for it to come back (specs/ask.md §3).
 *
 *   .ask/
 *     .gitignore     "*": forms are a conversation, not a record
 *     F-1/
 *       request.json   written here, read by the editor tab
 *       answer.json    written by the editor tab, read here
 */

import * as fs from "node:fs";
import * as path from "node:path";

import { Answer, AskError, Request, Step, StepAnswer, parseAnswer } from "./form";

export const STORE_DIR = ".ask";
export const REQUEST = "request.json";
export const ANSWER = "answer.json";

const ID = /^F-[1-9][0-9]*$/;

export function isFormId(id: unknown): id is string {
    return typeof id === "string" && ID.test(id);
}

/** The directory holding `.ask/`, walking up from `from`; null with none. */
export function findAsk(from: string): string | null {
    let dir = path.resolve(from);
    for (;;) {
        if (fs.statSync(path.join(dir, STORE_DIR), { throwIfNoEntry: false })?.isDirectory()) return dir;
        const up = path.dirname(dir);
        if (up === dir) return null;
        dir = up;
    }
}

/** Where the first form goes when there is no `.ask/` yet: the enclosing git
 *  repository's root, or `from` itself outside one. */
export function defaultRoot(from: string): string {
    let dir = path.resolve(from);
    for (;;) {
        if (fs.existsSync(path.join(dir, ".git"))) return dir;
        const up = path.dirname(dir);
        if (up === dir) return path.resolve(from);
        dir = up;
    }
}

function writeAtomic(file: string, data: string): void {
    const tmp = `${file}.${process.pid}.${Date.now()}.tmp`;
    fs.writeFileSync(tmp, data);
    fs.renameSync(tmp, file);
}

export type Wait =
    | { readonly status: "submitted" | "cancelled"; readonly answer: Answer }
    | { readonly status: "waiting" }
    | { readonly status: "aborted" };

export interface WaitOptions {
    /** Give up after this long and report the form as still waiting. */
    readonly timeoutMs: number;
    /** How often to look for the answer. */
    readonly pollMs?: number;
    /** Called on every look, so the caller can tell its client it is alive. */
    readonly onTick?: (waitedMs: number) => void;
    readonly signal?: AbortSignal;
}

export class Ask {
    readonly dir: string;

    constructor(readonly root: string) {
        this.dir = path.join(root, STORE_DIR);
    }

    private formDir(id: string): string {
        if (!isFormId(id)) throw new AskError("bad_id", `"${id}" is not a form id (F-<n>)`);
        return path.join(this.dir, id);
    }

    /** Opens a new form: the next free F-<n>, its request written whole. */
    create(title: string, steps: readonly Step[], previous?: Readonly<Record<string, StepAnswer>>, now = new Date()): Request {
        fs.mkdirSync(this.dir, { recursive: true });
        const ignore = path.join(this.dir, ".gitignore");
        if (!fs.existsSync(ignore)) fs.writeFileSync(ignore, "*\n");
        let n = 1;
        for (const name of fs.readdirSync(this.dir)) if (isFormId(name)) n = Math.max(n, Number(name.slice(2)) + 1);
        // Two servers may race for the same number: mkdir is the lock.
        for (;;) {
            try {
                fs.mkdirSync(path.join(this.dir, `F-${n}`));
                break;
            } catch (e) {
                if ((e as NodeJS.ErrnoException).code !== "EEXIST") throw e;
                n++;
            }
        }
        const req: Request = {
            id: `F-${n}`,
            title,
            createdAt: now.toISOString(),
            steps,
            ...(previous && Object.keys(previous).length ? { previous } : {}),
        };
        writeAtomic(path.join(this.dir, req.id, REQUEST), JSON.stringify(req, null, 2) + "\n");
        return req;
    }

    request(id: string): Request {
        const file = path.join(this.formDir(id), REQUEST);
        if (!fs.existsSync(file)) throw new AskError("not_found", `no form ${id}`);
        return JSON.parse(fs.readFileSync(file, "utf8")) as Request;
    }

    /** The form's answer, or null while the person has not finished it. */
    answer(id: string): Answer | null {
        const file = path.join(this.formDir(id), ANSWER);
        let raw: string;
        try {
            raw = fs.readFileSync(file, "utf8");
        } catch {
            return null;
        }
        return parseAnswer(JSON.parse(raw));
    }

    /** Closes a form nobody will answer: the editor tab sees the answer and
     *  closes itself. Leaves an answered form alone. */
    cancel(id: string, now = new Date()): void {
        if (this.answer(id)) return;
        const ans: Answer = { status: "cancelled", answeredAt: now.toISOString(), steps: {} };
        writeAtomic(path.join(this.formDir(id), ANSWER), JSON.stringify(ans, null, 2) + "\n");
    }

    /** Waits for the form's answer. Polling, not fs.watch: the answer is
     *  written by another process, sometimes on a network or synced
     *  filesystem, and a quarter second is nothing next to a person typing. */
    async wait(id: string, opts: WaitOptions): Promise<Wait> {
        this.request(id); // not_found before waiting on nothing
        const poll = opts.pollMs ?? 250;
        const start = Date.now();
        for (;;) {
            if (opts.signal?.aborted) return { status: "aborted" };
            const answer = this.answer(id);
            if (answer) return { status: answer.status, answer };
            const waited = Date.now() - start;
            if (waited >= opts.timeoutMs) return { status: "waiting" };
            opts.onTick?.(waited);
            await new Promise<void>((resolve) => {
                const stop = () => (clearTimeout(t), resolve());
                const t = setTimeout(() => (opts.signal?.removeEventListener("abort", stop), resolve()), Math.min(poll, opts.timeoutMs - waited));
                opts.signal?.addEventListener("abort", stop, { once: true });
            });
        }
    }
}
