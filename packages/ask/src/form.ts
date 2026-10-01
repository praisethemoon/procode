/* A form: the questions an agent asks, and what the person answered
 * (specs/ask.md §2). Checking happens here, once, so the store and the
 * editor tab can trust what they read. */

export const MAX_STEPS = 30;
export const MAX_OPTIONS = 12;
export const MAX_TITLE = 200;
export const MAX_LABEL = 200;
export const MAX_MARKDOWN = 20_000;
export const MAX_PREVIEW_BYTES = 200_000;

export type AskErrorCode = "invalid" | "not_found" | "bad_id";

export class AskError extends Error {
    constructor(
        readonly code: AskErrorCode,
        message: string,
    ) {
        super(message);
    }
}

export type StepKind = "single" | "multi" | "text";
export const STEP_KINDS: readonly StepKind[] = ["single", "multi", "text"];

export interface Option {
    readonly label: string;
    /** Markdown, shown under the label. */
    readonly description?: string;
    /** HTML, shown sandboxed beside the options when this one is focused. */
    readonly preview?: string;
}

export interface Step {
    readonly id: string;
    readonly title: string;
    /** Markdown: the question's context. */
    readonly body?: string;
    readonly kind: StepKind;
    readonly options?: readonly Option[];
    /** Choice steps offer a free-text "Other" unless this is false. */
    readonly other?: boolean;
}

export type StepState = "answered" | "skipped" | "needs_more";
export const STEP_STATES: readonly StepState[] = ["answered", "skipped", "needs_more"];

/** One step's answer, as the editor tab writes it. */
export interface StepAnswer {
    readonly state: StepState;
    /** The labels picked, for a choice step. */
    readonly choices?: readonly string[];
    /** The "Other" text, for a choice step. */
    readonly other?: string;
    /** The answer, for a text step. */
    readonly text?: string;
    /** Anything the person added beside the answer. */
    readonly note?: string;
    /** What the person wants to know first, when state is needs_more. */
    readonly question?: string;
}

export type FormStatus = "submitted" | "cancelled";

export interface Answer {
    readonly status: FormStatus;
    readonly answeredAt: string;
    readonly steps: Readonly<Record<string, StepAnswer>>;
}

export interface Request {
    readonly id: string;
    readonly title: string;
    readonly createdAt: string;
    readonly steps: readonly Step[];
    /** Answers carried over from an earlier form, keyed by step id. */
    readonly previous?: Readonly<Record<string, StepAnswer>>;
}

type Json = Record<string, unknown>;

function isObject(v: unknown): v is Json {
    return typeof v === "object" && v !== null && !Array.isArray(v);
}

function text(v: unknown, what: string, max: number, required: boolean): string | undefined {
    if (v === undefined || v === null) {
        if (required) throw new AskError("invalid", `${what} is required`);
        return undefined;
    }
    if (typeof v !== "string") throw new AskError("invalid", `${what} must be text`);
    if (required && !v.trim()) throw new AskError("invalid", `${what} is empty`);
    if (v.length > max) throw new AskError("invalid", `${what} is longer than ${max} characters`);
    return v;
}

function option(v: unknown, where: string): Option {
    if (typeof v === "string") v = { label: v };
    if (!isObject(v)) throw new AskError("invalid", `${where} must be a label or {label, description?, preview?}`);
    const label = text(v["label"], `${where}.label`, MAX_LABEL, true)!.trim();
    const description = text(v["description"], `${where}.description`, MAX_MARKDOWN, false);
    const preview = text(v["preview"], `${where}.preview`, MAX_PREVIEW_BYTES, false);
    return { label, ...(description ? { description } : {}), ...(preview ? { preview } : {}) };
}

function step(v: unknown, i: number): Step {
    const where = `steps[${i}]`;
    if (!isObject(v)) throw new AskError("invalid", `${where} must be an object`);
    const title = text(v["title"], `${where}.title`, MAX_TITLE, true)!.trim();
    const id = v["id"] === undefined ? String(i + 1) : text(v["id"], `${where}.id`, 64, true)!.trim();
    const body = text(v["body"], `${where}.body`, MAX_MARKDOWN, false);
    const options = v["options"] === undefined ? [] : v["options"];
    if (!Array.isArray(options)) throw new AskError("invalid", `${where}.options must be a list`);
    const kind = v["kind"] ?? (options.length ? "single" : "text");
    if (!STEP_KINDS.includes(kind as StepKind)) {
        throw new AskError("invalid", `${where}.kind must be one of ${STEP_KINDS.join(", ")}`);
    }
    if (kind === "text") {
        if (options.length) throw new AskError("invalid", `${where} is a text step and takes no options`);
        return { id, title, ...(body ? { body } : {}), kind: "text" };
    }
    if (options.length < 2 && v["other"] === false) {
        throw new AskError("invalid", `${where} needs at least two options, or one with "other" left on`);
    }
    if (options.length < 1) throw new AskError("invalid", `${where} is a ${String(kind)} step and needs options`);
    if (options.length > MAX_OPTIONS) throw new AskError("invalid", `${where} has more than ${MAX_OPTIONS} options`);
    const opts = options.map((o, j) => option(o, `${where}.options[${j}]`));
    const labels = new Set<string>();
    for (const o of opts) {
        if (labels.has(o.label)) throw new AskError("invalid", `${where} has two options labelled "${o.label}"`);
        labels.add(o.label);
    }
    if (v["other"] !== undefined && typeof v["other"] !== "boolean") throw new AskError("invalid", `${where}.other must be true or false`);
    return {
        id,
        title,
        ...(body ? { body } : {}),
        kind: kind as StepKind,
        options: opts,
        ...(v["other"] === false ? { other: false } : {}),
    };
}

/** The steps an agent sent, checked and normalised: a bare string option
 *  becomes {label}, a missing id becomes the step's position, a missing kind
 *  is single with options and text without. */
export function parseSteps(v: unknown): Step[] {
    if (!Array.isArray(v) || v.length === 0) throw new AskError("invalid", "steps must be a list of at least one question");
    if (v.length > MAX_STEPS) throw new AskError("invalid", `a form takes at most ${MAX_STEPS} steps`);
    const steps = v.map(step);
    const ids = new Set<string>();
    for (const s of steps) {
        if (ids.has(s.id)) throw new AskError("invalid", `two steps have the id "${s.id}"`);
        ids.add(s.id);
    }
    return steps;
}

function stepAnswer(v: unknown, where: string): StepAnswer {
    if (!isObject(v) || !STEP_STATES.includes(v["state"] as StepState)) {
        throw new AskError("invalid", `${where}.state must be one of ${STEP_STATES.join(", ")}`);
    }
    const out: Record<string, unknown> = { state: v["state"] };
    if (v["choices"] !== undefined) {
        if (!Array.isArray(v["choices"]) || !v["choices"].every((c) => typeof c === "string")) {
            throw new AskError("invalid", `${where}.choices must be a list of labels`);
        }
        out["choices"] = v["choices"];
    }
    for (const k of ["other", "text", "note", "question"]) {
        const t = text(v[k], `${where}.${k}`, MAX_MARKDOWN, false);
        if (t !== undefined && t.trim()) out[k] = t;
    }
    return out as unknown as StepAnswer;
}

/** An answer file, checked: what the editor tab wrote. */
export function parseAnswer(v: unknown): Answer {
    if (!isObject(v)) throw new AskError("invalid", "an answer must be an object");
    if (v["status"] !== "submitted" && v["status"] !== "cancelled") throw new AskError("invalid", "answer.status must be submitted or cancelled");
    const steps: Record<string, StepAnswer> = {};
    const raw = v["steps"] ?? {};
    if (!isObject(raw)) throw new AskError("invalid", "answer.steps must be an object keyed by step id");
    for (const [id, a] of Object.entries(raw)) steps[id] = stepAnswer(a, `answer.steps.${id}`);
    return { status: v["status"], answeredAt: String(v["answeredAt"] ?? ""), steps };
}

/** One step as the agent reads it back. */
export interface StepResult extends Partial<StepAnswer> {
    readonly id: string;
    readonly title: string;
    readonly state: StepState;
}

/** What the agent gets back: every step it asked, in its order. A step the
 *  person never touched reads as skipped. */
export function results(req: Request, answer: Answer): StepResult[] {
    return req.steps.map((s) => {
        const a = answer.steps[s.id];
        return a ? { id: s.id, title: s.title, ...a } : { id: s.id, title: s.title, state: "skipped" };
    });
}
