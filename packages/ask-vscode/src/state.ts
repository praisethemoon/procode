/* What the person has done to each step so far, and what it becomes when
 * they submit (specs/ask.md §5). Pure: the webview holds a FormState and
 * changes it only through these functions, which the tests drive directly.
 *
 * A step is OPEN until it is touched. It reads ANSWERED as soon as it holds an
 * answer, SKIPPED when the person chose to skip it, and NEEDS_MORE when they
 * wrote what they want to know. A question for more wins over an answer: the
 * person is saying the answer is not final. Skipping a step with an answer in
 * it sets the answer aside (it is not sent) without erasing it; touching the
 * step again brings it back.
 */

import type { Answer, Request, Step, StepAnswer, StepState } from "ask";

export interface Draft {
    readonly choices: readonly string[];
    readonly otherOn: boolean;
    readonly other: string;
    readonly text: string;
    readonly note: string;
    readonly more: string;
    readonly skipped: boolean;
}

export const EMPTY: Draft = { choices: [], otherOn: false, other: "", text: "", note: "", more: "", skipped: false };

/** Where the person is: a step's index, or the review page after the last. */
export type Place = number | "review";

export interface FormState {
    readonly place: Place;
    readonly drafts: Readonly<Record<string, Draft>>;
}

export type ShownState = StepState | "open";

export function draftOf(state: FormState, step: Step): Draft {
    return state.drafts[step.id] ?? EMPTY;
}

/** A form opened fresh, or with the answers carried over from an earlier one. */
export function initial(req: Request): FormState {
    const drafts: Record<string, Draft> = {};
    for (const s of req.steps) {
        const p = req.previous?.[s.id];
        if (!p) continue;
        const labels = new Set((s.options ?? []).map((o) => o.label));
        drafts[s.id] = {
            ...EMPTY,
            choices: (p.choices ?? []).filter((c) => labels.has(c)),
            otherOn: !!p.other,
            other: p.other ?? "",
            text: p.text ?? "",
            note: p.note ?? "",
        };
    }
    return { place: 0, drafts };
}

function hasAnswer(step: Step, d: Draft): boolean {
    if (step.kind === "text") return d.text.trim() !== "";
    return d.choices.length > 0 || (d.otherOn && d.other.trim() !== "");
}

export function shownState(step: Step, d: Draft): ShownState {
    if (d.more.trim()) return "needs_more";
    if (d.skipped) return "skipped";
    return hasAnswer(step, d) ? "answered" : "open";
}

function edit(state: FormState, step: Step, change: Partial<Draft>): FormState {
    const before = draftOf(state, step);
    // Doing anything to a skipped step un-skips it.
    const after = { ...before, skipped: false, ...change };
    return { ...state, drafts: { ...state.drafts, [step.id]: after } };
}

/** Picks an option: the only one on a single step, toggled on a multi. */
export function pick(state: FormState, step: Step, label: string): FormState {
    const d = draftOf(state, step);
    if (step.kind === "multi") {
        const on = d.choices.includes(label);
        return edit(state, step, { choices: on ? d.choices.filter((c) => c !== label) : [...d.choices, label] });
    }
    return edit(state, step, { choices: [label], otherOn: false });
}

/** Picks Other: alone on a single step, toggled on a multi. */
export function pickOther(state: FormState, step: Step): FormState {
    const d = draftOf(state, step);
    if (step.kind === "multi") return edit(state, step, { otherOn: !d.otherOn });
    return edit(state, step, { choices: [], otherOn: true });
}

export function setField(state: FormState, step: Step, field: "other" | "text" | "note" | "more", value: string): FormState {
    // Typing an Other picks it, the way clicking it would.
    if (field === "other") return edit(state, step, step.kind === "multi" ? { other: value, otherOn: true } : { other: value, otherOn: true, choices: [] });
    return edit(state, step, { [field]: value });
}

/** Skips the step, leaving what was written so it can be taken back. */
export function skip(state: FormState, step: Step): FormState {
    return { ...state, drafts: { ...state.drafts, [step.id]: { ...draftOf(state, step), skipped: true } } };
}

export function go(state: FormState, place: Place): FormState {
    return { ...state, place };
}

export function next(req: Request, state: FormState): FormState {
    if (state.place === "review") return state;
    return go(state, state.place + 1 >= req.steps.length ? "review" : state.place + 1);
}

export function back(req: Request, state: FormState): FormState {
    if (state.place === "review") return go(state, req.steps.length - 1);
    return go(state, Math.max(0, state.place - 1));
}

/** How many steps hold an answer. */
export function answeredCount(req: Request, state: FormState): number {
    return req.steps.filter((s) => shownState(s, draftOf(state, s)) === "answered").length;
}

function trimmed(v: string): string | undefined {
    const t = v.trim();
    return t ? t : undefined;
}

/** One step as the answer file holds it. An open step is sent as skipped,
 *  and a skipped one carries only its note: an answer set aside is not sent. */
export function stepAnswer(step: Step, d: Draft): StepAnswer {
    const shown = shownState(step, d);
    const out: { -readonly [K in keyof StepAnswer]: StepAnswer[K] } = { state: shown === "open" ? "skipped" : shown };
    if (shown === "answered" || shown === "needs_more") {
        if (step.kind === "text") out.text = trimmed(d.text);
        else {
            if (d.choices.length) out.choices = [...d.choices];
            if (d.otherOn) out.other = trimmed(d.other);
        }
    }
    if (shown !== "open") out.note = trimmed(d.note);
    if (shown === "needs_more") out.question = trimmed(d.more);
    for (const k of Object.keys(out) as (keyof StepAnswer)[]) if (out[k] === undefined) delete out[k];
    return out;
}

export function toAnswer(req: Request, state: FormState, now = new Date()): Answer {
    const steps: Record<string, StepAnswer> = {};
    for (const s of req.steps) steps[s.id] = stepAnswer(s, draftOf(state, s));
    return { status: "submitted", answeredAt: now.toISOString(), steps };
}

/* ---------------------------------------------------------------- keys */

export interface Key {
    readonly key: string;
    readonly shift: boolean;
    readonly mod: boolean;
    /** The key went to a text field. */
    readonly typing: boolean;
}

export type Action =
    | { readonly do: "pick"; readonly label: string }
    | { readonly do: "other" }
    | { readonly do: "next" }
    | { readonly do: "back" }
    | { readonly do: "submit" }
    | { readonly do: "blur" };

/** What a key does on the page the person is on, or null to let it be.
 *  In a text field only ⌘/Ctrl+Enter (next, or submit on the review page) and
 *  Escape (leave the field) are taken; everything else is typing. */
export function keyAction(req: Request, place: Place, k: Key): Action | null {
    if (k.key === "Escape" && k.typing) return { do: "blur" };
    if (k.key === "Enter" && k.mod) return place === "review" ? { do: "submit" } : { do: "next" };
    if (k.typing) return null;
    if (k.key === "Enter") return k.shift ? { do: "back" } : place === "review" ? null : { do: "next" };
    if (place === "review") return null;
    const step = req.steps[place];
    if (!step || step.kind === "text" || !/^[1-9]$/.test(k.key)) return null;
    const n = Number(k.key) - 1;
    const options = step.options ?? [];
    if (n < options.length) return { do: "pick", label: options[n].label };
    if (n === options.length && step.other !== false) return { do: "other" };
    return null;
}
