/* A form's tab (specs/ask.md §5): the steps down the side, one step at a
 * time beside them, and a review page before Submit.
 *
 * Every change to the answers goes through src/state.ts, so what the tests
 * pin is what this page does; this file only draws the state and turns
 * clicks and keys into those calls.
 */

import { StrictMode, useEffect, useMemo, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import { Button, Icon, TextArea } from "baukasten-ui/core";
import type { Option, Request, Step } from "ask";

import { PREVIEW_SANDBOX, PreviewParts, previewDocument, themeCss } from "../src/preview";
import type { ToHost, ToView } from "../src/protocol";
import {
    Action,
    FormState,
    Place,
    ShownState,
    answeredCount,
    back,
    draftOf,
    go,
    initial,
    keyAction,
    next,
    pick,
    pickOther,
    setField,
    shownState,
    skip,
    toAnswer,
} from "../src/state";
import { Markdown } from "./Markdown";

declare function acquireVsCodeApi(): { postMessage(m: unknown): void };
const vscode = acquireVsCodeApi();
const send = (m: ToHost) => vscode.postMessage(m);

const STATE_ICON: Record<ShownState, { icon: "pass-filled" | "circle-large-outline" | "debug-step-over" | "question"; label: string }> = {
    answered: { icon: "pass-filled", label: "answered" },
    open: { icon: "circle-large-outline", label: "open" },
    skipped: { icon: "debug-step-over", label: "skipped" },
    needs_more: { icon: "question", label: "needs more" },
};

/* ---------------------------------------------------------------- theme */

/* The webview's --vscode-* variables, re-read when the theme changes, so a
 * preview frame (which inherits none of them) can be rebuilt in the theme. */
function useTheme(): string {
    const read = () => {
        const style = document.documentElement.style;
        const vars: Record<string, string> = {};
        for (let i = 0; i < style.length; i++) {
            const name = style[i];
            if (name.startsWith("--vscode-")) vars[name] = style.getPropertyValue(name);
        }
        return themeCss(vars);
    };
    const [css, setCss] = useState(read);
    useEffect(() => {
        const watch = new MutationObserver(() => setCss(read()));
        watch.observe(document.documentElement, { attributes: true, attributeFilter: ["style", "class"] });
        watch.observe(document.body, { attributes: true, attributeFilter: ["class", "data-vscode-theme-kind"] });
        return () => watch.disconnect();
    }, []);
    return css;
}

function Preview(props: { html: string; parts: PreviewParts; label: string }): JSX.Element {
    const theme = useTheme();
    const doc = useMemo(() => previewDocument(props.html, props.parts, theme), [props.html, props.parts, theme]);
    return <iframe className="ask-preview" title={`Preview: ${props.label}`} sandbox={PREVIEW_SANDBOX} srcDoc={doc} />;
}

/* ---------------------------------------------------------------- a step */

function OptionCard(props: {
    n: number;
    option: Option;
    multi: boolean;
    on: boolean;
    onPick: () => void;
    onLook: () => void;
}): JSX.Element {
    // A div, not a button: the description is Markdown, which may hold
    // paragraphs and links, and neither belongs inside a button.
    return (
        <div
            tabIndex={0}
            role={props.multi ? "checkbox" : "radio"}
            aria-checked={props.on}
            className={`ask-option${props.on ? " is-on" : ""}`}
            onClick={(e) => {
                if (!(e.target instanceof HTMLAnchorElement)) props.onPick();
            }}
            onKeyDown={(e) => {
                if (e.key !== " ") return;
                e.preventDefault();
                props.onPick();
            }}
            onMouseEnter={props.onLook}
            onFocus={props.onLook}
        >
            <span className="ask-key">{props.n}</span>
            <span className="ask-option-text">
                <span className="ask-option-label">{props.option.label}</span>
                {props.option.description ? <Markdown source={props.option.description} /> : null}
            </span>
            <Icon name={props.multi ? (props.on ? "pass-filled" : "circle-large-outline") : props.on ? "circle-large-filled" : "circle-large-outline"} />
        </div>
    );
}

function Choices(props: { step: Step; state: FormState; set: (s: FormState) => void; parts: PreviewParts }): JSX.Element {
    const { step, state, set } = props;
    const d = draftOf(state, step);
    const options = step.options ?? [];
    const multi = step.kind === "multi";
    const withPreview = options.filter((o) => o.preview);
    const [looking, setLooking] = useState<string | null>(null);
    const shown = withPreview.find((o) => o.label === looking) ?? withPreview.find((o) => d.choices.includes(o.label)) ?? withPreview[0];
    const otherRef = useRef<HTMLTextAreaElement>(null);
    const list = (
        <div className="ask-options" role={multi ? "group" : "radiogroup"} aria-label={step.title}>
            {options.map((o, i) => (
                <OptionCard
                    key={o.label}
                    n={i + 1}
                    option={o}
                    multi={multi}
                    on={d.choices.includes(o.label)}
                    onPick={() => set(pick(state, step, o.label))}
                    onLook={() => setLooking(o.label)}
                />
            ))}
            {step.other !== false ? (
                <div className={`ask-option ask-other${d.otherOn ? " is-on" : ""}`}>
                    <button
                        type="button"
                        role={multi ? "checkbox" : "radio"}
                        aria-checked={d.otherOn}
                        className="ask-other-pick"
                        onClick={() => {
                            set(pickOther(state, step));
                            setTimeout(() => otherRef.current?.focus());
                        }}
                    >
                        <span className="ask-key">{options.length + 1}</span>
                        <span className="ask-option-label">Other</span>
                    </button>
                    <TextArea
                        ref={otherRef}
                        fullWidth
                        minRows={1}
                        maxRows={6}
                        placeholder="Your own answer"
                        value={d.other}
                        onChange={(e) => set(setField(state, step, "other", e.target.value))}
                    />
                </div>
            ) : null}
        </div>
    );
    if (!shown?.preview) return list;
    return (
        <div className="ask-with-preview">
            {list}
            <figure className="ask-preview-pane">
                <figcaption>{shown.label}</figcaption>
                <Preview html={shown.preview} parts={props.parts} label={shown.label} />
            </figure>
        </div>
    );
}

function Extra(props: { label: string; icon: "note" | "question"; value: string; placeholder: string; onChange: (v: string) => void }): JSX.Element {
    const [open, setOpen] = useState(props.value !== "");
    if (!open) {
        return (
            <Button variant="ghost" size="sm" onClick={() => setOpen(true)}>
                <Icon name={props.icon} /> {props.label}
            </Button>
        );
    }
    return (
        <label className="ask-extra">
            <span className="ask-extra-label">
                <Icon name={props.icon} /> {props.label}
            </span>
            <TextArea fullWidth minRows={2} maxRows={10} autoFocus={props.value === ""} placeholder={props.placeholder} value={props.value} onChange={(e) => props.onChange(e.target.value)} />
        </label>
    );
}

function StepPage(props: { req: Request; index: number; state: FormState; set: (s: FormState) => void; parts: PreviewParts }): JSX.Element {
    const { req, index, state, set } = props;
    const step = req.steps[index];
    const d = draftOf(state, step);
    const st = shownState(step, d);
    return (
        <article className="ask-step" key={step.id}>
            <div className="ask-eyebrow">
                Question {index + 1} of {req.steps.length}
                <span className={`ask-state is-${st}`}>
                    <Icon name={STATE_ICON[st].icon} /> {STATE_ICON[st].label}
                </span>
            </div>
            <h1>{step.title}</h1>
            {step.body ? <Markdown source={step.body} /> : null}
            <section className="ask-answer">
                {step.kind === "text" ? (
                    <TextArea
                        fullWidth
                        minRows={5}
                        maxRows={20}
                        autoFocus
                        placeholder="Your answer"
                        value={d.text}
                        onChange={(e) => set(setField(state, step, "text", e.target.value))}
                    />
                ) : (
                    <Choices step={step} state={state} set={set} parts={props.parts} />
                )}
            </section>
            <section className="ask-extras">
                <Extra key={`note-${step.id}`} label="Add a note" icon="note" value={d.note} placeholder="Anything to add beside the answer" onChange={(v) => set(setField(state, step, "note", v))} />
                <Extra
                    key={`more-${step.id}`}
                    label="I need more on this"
                    icon="question"
                    value={d.more}
                    placeholder="What do you need to know before answering?"
                    onChange={(v) => set(setField(state, step, "more", v))}
                />
            </section>
        </article>
    );
}

/* ---------------------------------------------------------------- review */

function summary(step: Step, state: FormState): string {
    const d = draftOf(state, step);
    const st = shownState(step, d);
    if (st === "needs_more") return `Asks: ${d.more.trim()}`;
    if (st !== "answered") return "";
    if (step.kind === "text") return d.text.trim();
    return [...d.choices, ...(d.otherOn && d.other.trim() ? [d.other.trim()] : [])].join(", ");
}

function Review(props: { req: Request; state: FormState; set: (s: FormState) => void; submit: () => void }): JSX.Element {
    const { req, state } = props;
    const open = req.steps.filter((s) => shownState(s, draftOf(state, s)) === "open").length;
    return (
        <article className="ask-step ask-review">
            <div className="ask-eyebrow">Review</div>
            <h1>{req.title}</h1>
            <ol className="ask-review-list">
                {req.steps.map((s, i) => {
                    const st = shownState(s, draftOf(state, s));
                    return (
                        <li key={s.id}>
                            <button type="button" className="ask-review-row" onClick={() => props.set(go(state, i))}>
                                <Icon name={STATE_ICON[st].icon} className={`ask-icon is-${st}`} />
                                <span className="ask-review-title">{s.title}</span>
                                <span className="ask-review-answer">{summary(s, state) || STATE_ICON[st].label}</span>
                            </button>
                        </li>
                    );
                })}
            </ol>
            {open ? (
                <p className="ask-muted">
                    {open} {open === 1 ? "question is" : "questions are"} still open and will be sent as skipped.
                </p>
            ) : null}
            <div className="ask-submit">
                <Button variant="primary" onClick={props.submit}>
                    <Icon name="send" /> Submit answers
                </Button>
                <span className="ask-muted">⌘/Ctrl+Enter</span>
            </div>
        </article>
    );
}

/* ---------------------------------------------------------------- the form */

function Rail(props: { req: Request; state: FormState; set: (s: FormState) => void }): JSX.Element {
    const { req, state } = props;
    return (
        <nav className="ask-rail" aria-label="Questions">
            <div className="ask-rail-head">
                <div className="ask-rail-title">{req.title}</div>
                <div className="ask-muted">
                    {answeredCount(req, state)} of {req.steps.length} answered · {req.id}
                </div>
            </div>
            <ol>
                {req.steps.map((s, i) => {
                    const st = shownState(s, draftOf(state, s));
                    return (
                        <li key={s.id}>
                            <button
                                type="button"
                                className={`ask-rail-item${state.place === i ? " is-here" : ""}`}
                                aria-current={state.place === i ? "step" : undefined}
                                title={STATE_ICON[st].label}
                                onClick={() => props.set(go(state, i))}
                            >
                                <Icon name={STATE_ICON[st].icon} className={`ask-icon is-${st}`} />
                                <span>{s.title}</span>
                            </button>
                        </li>
                    );
                })}
                <li>
                    <button
                        type="button"
                        className={`ask-rail-item ask-rail-review${state.place === "review" ? " is-here" : ""}`}
                        aria-current={state.place === "review" ? "step" : undefined}
                        onClick={() => props.set(go(state, "review"))}
                    >
                        <Icon name="checklist" className="ask-icon" />
                        <span>Review and submit</span>
                    </button>
                </li>
            </ol>
        </nav>
    );
}

function isTyping(t: EventTarget | null): boolean {
    return t instanceof HTMLElement && (t.tagName === "TEXTAREA" || t.tagName === "INPUT" || t.isContentEditable);
}

function Form(props: { req: Request; parts: PreviewParts }): JSX.Element {
    const { req } = props;
    const [state, setState] = useState<FormState>(() => initial(req));
    const page = useRef<HTMLElement>(null);
    // The state as of the last change, not the last render: two keys pressed
    // before a render must both land, and the second must be read against the
    // page the first left (a digit on the step Enter just moved to).
    const latest = useRef(state);
    const set = (s: FormState) => {
        latest.current = s;
        setState(s);
    };

    const submit = () => send({ type: "submit", answer: toAnswer(req, latest.current) });

    const act = (a: Action) => {
        const s = latest.current;
        const here = s.place === "review" ? null : req.steps[s.place];
        switch (a.do) {
            case "pick":
                if (here) set(pick(s, here, a.label));
                return;
            case "other":
                if (here) set(pickOther(s, here));
                return;
            case "next":
                set(next(req, s));
                return;
            case "back":
                set(back(req, s));
                return;
            case "submit":
                submit();
                return;
            case "blur":
                (document.activeElement as HTMLElement | null)?.blur();
                page.current?.focus();
                return;
        }
    };

    useEffect(() => {
        const onKey = (e: KeyboardEvent) => {
            const a = keyAction(req, latest.current.place, {
                key: e.key,
                shift: e.shiftKey,
                mod: e.metaKey || e.ctrlKey,
                typing: isTyping(e.target),
            });
            if (!a) return;
            e.preventDefault();
            act(a);
        };
        window.addEventListener("keydown", onKey);
        return () => window.removeEventListener("keydown", onKey);
    }, [req]);

    // A new page starts with the keys on it, not on the control that led there.
    const place: Place = state.place;
    useEffect(() => {
        if (!isTyping(document.activeElement)) page.current?.focus();
        page.current?.scrollTo({ top: 0 });
    }, [place]);

    const index = place === "review" ? -1 : place;
    const here = index >= 0 ? req.steps[index] : null;
    return (
        <div className="ask">
            <Rail req={req} state={state} set={set} />
            <main className="ask-page" ref={page} tabIndex={-1}>
                {here ? <StepPage req={req} index={index} state={state} set={set} parts={props.parts} /> : <Review req={req} state={state} set={set} submit={submit} />}
                {here ? (
                    <footer className="ask-footer">
                        <Button variant="secondary" disabled={index === 0} onClick={() => act({ do: "back" })}>
                            <Icon name="arrow-left" /> Back
                        </Button>
                        <Button variant="ghost" onClick={() => set(next(req, skip(latest.current, here)))}>
                            Skip
                        </Button>
                        <span className="ask-hints ask-muted">
                            {here.kind === "text"
                                ? "⌘/Ctrl+Enter next"
                                : `1–${(here.options?.length ?? 0) + (here.other === false ? 0 : 1)} pick · Enter next · ⇧Enter back`}
                        </span>
                        <Button variant="primary" onClick={() => act({ do: "next" })}>
                            {index === req.steps.length - 1 ? "Review" : "Next"} <Icon name="arrow-right" />
                        </Button>
                    </footer>
                ) : null}
            </main>
        </div>
    );
}

function Closed(props: { status: string }): JSX.Element {
    return (
        <div className="ask-closed">
            <h1>{props.status === "submitted" ? "Answered" : "This form was closed"}</h1>
            <p className="ask-muted">
                {props.status === "submitted" ? "The answers were sent." : "The agent stopped waiting for it, so nothing you enter here would reach it."}
            </p>
            <Button variant="secondary" onClick={() => send({ type: "dismiss" })}>
                Close tab
            </Button>
        </div>
    );
}

function App(): JSX.Element {
    const [loaded, setLoaded] = useState<{ req: Request; parts: PreviewParts } | null>(null);
    const [closed, setClosed] = useState<string | null>(null);
    useEffect(() => {
        const on = (e: MessageEvent) => {
            const m = e.data as ToView;
            if (m?.type === "load") setLoaded({ req: m.request, parts: m.preview });
            else if (m?.type === "closed") setClosed(m.status);
        };
        window.addEventListener("message", on);
        send({ type: "ready" });
        return () => window.removeEventListener("message", on);
    }, []);
    if (closed) return <Closed status={closed} />;
    if (!loaded) return <div className="ask-closed ask-muted">Loading…</div>;
    return <Form req={loaded.req} parts={loaded.parts} />;
}

createRoot(document.getElementById("root")!).render(
    <StrictMode>
        <App />
    </StrictMode>,
);
