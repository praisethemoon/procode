/* A lap session as a review: what it was for, what it changed, and how it got
 * there. The files first, with their net change over the session; then the
 * trajectory — every edit in the order it was made, with the reason recorded
 * for it — each opening as the same diff-with-comment a ticket's commit list
 * opens. */

import { Icon } from "baukasten-ui/core";
import { useState } from "react";

import type { LapReview, LapReviewFile } from "coboard/lap";
import { IdLink } from "./parts";
import { send } from "./rpc";

function when(iso: string): string {
    return iso.replace("T", " ").replace(/:\d\dZ$/, "").replace(/Z$/, "");
}

/* A unified diff, coloured by line. The text is lap's; nothing is re-diffed. */
function Diff(props: { text: string }): JSX.Element {
    return (
        <pre className="cb-diff">
            {props.text.split("\n").map((line, i) => {
                const t = line.trimStart();
                const cls = t.startsWith("@@") ? "cb-diff-hunk" : t.startsWith("+") ? "cb-diff-add" : t.startsWith("-") ? "cb-diff-del" : "";
                return (
                    <div key={i} className={cls}>
                        {line || " "}
                    </div>
                );
            })}
        </pre>
    );
}

function FileRow(props: { f: LapReviewFile }): JSX.Element {
    const [open, setOpen] = useState(false);
    const f = props.f;
    return (
        <li className="cb-review-file">
            <div className="cb-session" onClick={() => setOpen(!open)}>
                <Icon name={open ? "chevron-down" : "chevron-right"} />
                <code>{f.path}</code>
                {f.deleted ? <span className="cb-label">deleted</span> : null}
                <span className="cb-review-stat">
                    <span className="cb-plus">+{f.added}</span> <span className="cb-minus">−{f.removed}</span>
                </span>
            </div>
            {open ? <Diff text={f.diff} /> : null}
        </li>
    );
}

export function Review(props: { session: string; ticket: string | null; review: LapReview | null; error?: string }): JSX.Element {
    const r = props.review;
    if (r === null) {
        return (
            <article>
                <h1>{props.session} review</h1>
                <p className="cb-muted">lap could not answer: {props.error ?? "no review"}</p>
            </article>
        );
    }
    const added = r.files.reduce((n, f) => n + f.added, 0);
    const removed = r.files.reduce((n, f) => n + f.removed, 0);
    return (
        <article>
            <header className="cb-header">
                <div className="cb-trail">
                    {props.ticket ? (
                        <span>
                            <IdLink id={props.ticket} />
                            <span className="cb-sep"> › </span>
                        </span>
                    ) : null}
                    <span className="cb-muted">lap session {props.session}</span>
                </div>
                <h1>{r.purpose.split("\n")[0] || props.session}</h1>
                <p className="cb-muted">
                    {r.commits} edit{r.commits === 1 ? "" : "s"} to {r.files.length} file{r.files.length === 1 ? "" : "s"} ·{" "}
                    <span className="cb-plus">+{added}</span> <span className="cb-minus">−{removed}</span> · {when(r.from)} → {when(r.to)}
                </p>
            </header>

            <section className="cb-section">
                <h3>What changed</h3>
                {r.files.length === 0 ? (
                    <p className="cb-muted">No net change: every edit in this session was undone within it.</p>
                ) : (
                    <ul className="cb-sessions">
                        {r.files.map((f) => (
                            <FileRow key={f.path} f={f} />
                        ))}
                    </ul>
                )}
            </section>

            <section className="cb-section">
                <h3>How it got there</h3>
                <ol className="cb-trajectory">
                    {r.trajectory.map((step) => (
                        <li
                            key={step.id}
                            className="cb-commit"
                            title={`Show the diff of ${step.id}`}
                            onClick={() => send({ type: "showEdit", commit: step.id, sessionMsg: r.purpose })}
                        >
                            <div className="cb-step-head">
                                <Icon name="diff" /> <code>{step.id}</code> <code>{step.file}</code>
                                <span className="cb-muted">
                                    {" "}
                                    {step.op}
                                    {step.new_lines > 0 ? ` · line ${step.new_start}${step.new_lines > 1 ? `–${step.new_start + step.new_lines - 1}` : ""}` : ""}
                                </span>
                            </div>
                            <div className="cb-step-msg">{step.msg}</div>
                        </li>
                    ))}
                </ol>
            </section>
        </article>
    );
}
