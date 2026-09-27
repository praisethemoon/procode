/* A lap session as a review: what it was for, what it changed, and how it got
 * there. The files first, with their net change over the session; then the
 * trajectory as a timeline (timeline.ts) — every change in the order it was
 * made, the steps on one file that share an intent drawn as one node with a
 * line per step for its behavior — each step opening as the same
 * diff-with-comment a ticket's commit list opens. */

import { Icon } from "baukasten-ui/core";
import { useState } from "react";

import type { LapReview, LapReviewFile, LapReviewStep } from "coboard/lap";
import { splitPath } from "../src/commits";
import { TimelineItem, gapLabel, linesOf, timeline } from "../src/timeline";
import { CommitLine, CommitText, IdLink } from "./parts";
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

const OP_TITLE: Record<string, string> = { create: "created", edit: "edited", delete: "deleted" };

/* One change on the rail: the file, the intent its steps share, and a line
 * per step with its behavior and short hash, each opening its own diff. */
function Node(props: { item: Extract<TimelineItem<LapReviewStep>, { kind: "node" }>; purpose: string }): JSX.Element {
    const { item } = props;
    const { name, dir } = splitPath(item.file);
    const show = (id: string) => send({ type: "showEdit", commit: id, sessionMsg: props.purpose });
    return (
        <li className={`cb-tl-node cb-tl-${item.op}`}>
            <span className="cb-tl-dot" title={OP_TITLE[item.op] ?? item.op} aria-hidden="true">
                {item.op === "delete" ? <Icon name="close" /> : null}
            </span>
            <div className="cb-tl-card">
                <div className="cb-edit-head">
                    <span className="cb-edit-name">{name}</span>
                    {dir ? <span className="cb-edit-dir">{dir}</span> : null}
                    {item.op !== "edit" ? <span className="cb-label">{item.op}</span> : null}
                    {item.steps.length > 1 ? <span className="cb-edit-count">{item.steps.length} edits</span> : null}
                </div>
                <div className="cb-edit-intent" title={item.intent}>
                    <CommitText text={item.intent} />
                </div>
            </div>
            <ol className="cb-steps">
                {item.steps.map((s) => (
                    <CommitLine
                        key={s.id}
                        commit={s}
                        lines={
                            (s.op === "delete" ? "deleted" : `line${s.new_lines === 1 ? "" : "s"} ${linesOf(s)}`) +
                            (s.from ? ` · from #${s.from.slice(0, 7)}` : "")
                        }
                        onOpen={() => show(s.id)}
                    />
                ))}
            </ol>
        </li>
    );
}

export function Review(props: {
    session: string;
    ticket: string | null;
    review: LapReview | null;
    error?: string;
    /* a session still only in a branch folder */
    branch?: string;
    /* an adopted session: its branch, and what that branch's merge stopped */
    adoptedFrom?: string;
    stops?: readonly { readonly file: string; readonly at: string }[];
}): JSX.Element {
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
                    <span className="cb-muted">
                        lap session {props.session}
                        {props.branch ? ` · in branch ${props.branch}, not merged yet` : ""}
                        {props.adoptedFrom ? ` · adopted from branch ${props.adoptedFrom}` : ""}
                    </span>
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

            {props.stops && props.stops.length > 0 ? (
                <section className="cb-section">
                    <h3>Stopped by the merge</h3>
                    <p className="cb-muted">
                        lap merge placed none of branch {props.adoptedFrom}'s edits to these files from the one named on: they
                        met the parent's own changes. The rest was committed by hand, or is still pending.
                    </p>
                    <ul className="cb-sessions">
                        {props.stops.map((s) => (
                            <li key={s.file} className="cb-session">
                                <Icon name="warning" /> <code>{s.file}</code> <span className="cb-muted">at</span>{" "}
                                <a
                                    className="cb-link cb-ref"
                                    href="#"
                                    title="The first edit to this file that was not adopted"
                                    onClick={(e) => {
                                        e.preventDefault();
                                        send({ type: "showEdit", commit: s.at });
                                    }}
                                >
                                    #{s.at.slice(0, 7)}
                                </a>
                            </li>
                        ))}
                    </ul>
                </section>
            ) : null}

            <section className="cb-section">
                <h3>How it got there</h3>
                <ol className="cb-tl">
                    {timeline(r.trajectory).map((item, i) =>
                        item.kind === "node" ? (
                            <Node key={item.steps[0].id} item={item} purpose={r.purpose} />
                        ) : item.kind === "move" ? (
                            <li key={`move-${i}`} className="cb-tl-move">
                                <Icon name="arrow-right" /> <code>{item.area || "the repository root"}</code>
                            </li>
                        ) : (
                            <li key={`gap-${i}`} className="cb-tl-gap">
                                {gapLabel(item.ms)}
                            </li>
                        ),
                    )}
                </ol>
            </section>
        </article>
    );
}
