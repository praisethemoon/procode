/* The forms one VS Code window holds, in memory (specs/ask.md §3).
 *
 * The MCP server runs inside the extension, so the form an agent opens and
 * the tab that answers it are in the same process: a form is an object here,
 * not a file, and waiting for its answer is waiting for an event. Forms last
 * as long as the window; `from` can carry over the answers of any form
 * answered since it opened.
 */

import { EventEmitter } from "node:events";

import { Answer, AskError, Request, Step, StepAnswer, parseAnswer } from "./form";

const ID = /^F-[1-9][0-9]*$/;

export function isFormId(id: unknown): id is string {
    return typeof id === "string" && ID.test(id);
}

export type Wait =
    | { readonly status: "submitted" | "cancelled"; readonly answer: Answer }
    | { readonly status: "waiting" }
    | { readonly status: "aborted" };

export interface WaitOptions {
    /** Give up after this long and report the form as still waiting. */
    readonly timeoutMs: number;
    /** Called every tickMs while waiting, so the caller can tell its client it is alive. */
    readonly onTick?: (waitedMs: number) => void;
    readonly tickMs?: number;
    readonly signal?: AbortSignal;
}

interface Events {
    /** A form was opened: show it. */
    open: [Request];
    /** A form was answered or cancelled, by its tab or by its caller. */
    answer: [string, Answer];
}

export class Forms {
    private readonly requests = new Map<string, Request>();
    private readonly answers = new Map<string, Answer>();
    private readonly events = new EventEmitter<Events>();
    private next = 1;

    on<K extends keyof Events>(event: K, fn: (...args: Events[K]) => void): () => void {
        this.events.on(event, fn as never);
        return () => this.events.off(event, fn as never);
    }

    /** Opens a new form, F-<n>, and tells whoever shows forms. */
    create(title: string, steps: readonly Step[], previous?: Readonly<Record<string, StepAnswer>>, now = new Date()): Request {
        const req: Request = {
            id: `F-${this.next++}`,
            title,
            createdAt: now.toISOString(),
            steps,
            ...(previous && Object.keys(previous).length ? { previous } : {}),
        };
        this.requests.set(req.id, req);
        this.events.emit("open", req);
        return req;
    }

    request(id: string): Request {
        if (!isFormId(id)) throw new AskError("bad_id", `"${id}" is not a form id (F-<n>)`);
        const req = this.requests.get(id);
        if (!req) throw new AskError("not_found", `no form ${id} in this window`);
        return req;
    }

    /** The form's answer, or null while the person has not finished it. */
    answer(id: string): Answer | null {
        return this.answers.get(id) ?? null;
    }

    /** Forms with no answer yet, oldest first. */
    pending(): Request[] {
        return [...this.requests.values()].filter((r) => !this.answers.has(r.id));
    }

    private settle(id: string, answer: Answer): void {
        this.answers.set(id, answer);
        this.events.emit("answer", id, answer);
    }

    /** The tab's answer, checked, for the steps the form asked. False when the
     *  form was already answered or cancelled: the first word stands. */
    submit(id: string, raw: unknown, now = new Date()): boolean {
        const req = this.request(id);
        if (this.answers.has(id)) return false;
        const parsed = parseAnswer(raw);
        const asked = new Set(req.steps.map((s) => s.id));
        const steps = Object.fromEntries(Object.entries(parsed.steps).filter(([k]) => asked.has(k)));
        this.settle(id, { status: parsed.status, answeredAt: now.toISOString(), steps });
        return true;
    }

    /** Closes a form nobody will answer. Leaves an answered form alone. */
    cancel(id: string, now = new Date()): void {
        if (!this.requests.has(id) || this.answers.has(id)) return;
        this.settle(id, { status: "cancelled", answeredAt: now.toISOString(), steps: {} });
    }

    /** Waits for the form's answer: until it comes, the timeout, or the abort. */
    wait(id: string, opts: WaitOptions): Promise<Wait> {
        this.request(id); // not_found before waiting on nothing
        const done = this.answers.get(id);
        if (done) return Promise.resolve({ status: done.status, answer: done });
        if (opts.signal?.aborted) return Promise.resolve({ status: "aborted" });
        return new Promise<Wait>((resolve) => {
            const start = Date.now();
            const finish = (w: Wait) => {
                off();
                clearTimeout(timeout);
                clearInterval(tick);
                opts.signal?.removeEventListener("abort", aborted);
                resolve(w);
            };
            const off = this.on("answer", (answered, answer) => {
                if (answered === id) finish({ status: answer.status, answer });
            });
            const aborted = () => finish({ status: "aborted" });
            opts.signal?.addEventListener("abort", aborted, { once: true });
            const timeout = setTimeout(() => finish({ status: "waiting" }), opts.timeoutMs);
            const tick = setInterval(() => opts.onTick?.(Date.now() - start), opts.tickMs ?? 15_000);
        });
    }
}
