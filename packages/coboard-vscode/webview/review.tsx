/* A lap session as a review: what it was for, what it changed, and how it got
 * there. The files first, with their net change over the session; then the
 * trajectory as a timeline (timeline.ts) — every change in the order it was
 * made, with the reason recorded for it, a restructure's parts drawn as one
 * node — each opening as the same diff-with-comment a ticket's commit list
 * opens. */

import { Icon } from "baukasten-ui/core";
import { useState } from "react";

import type { LapReview, LapReviewFile, LapReviewStep } from "coboard/lap";
import { splitPath } from "../src/commits";
import { TimelineItem, gapLabel, linesOf, timeline } from "../src/timeline";
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

const OP_TITLE: Record<string, string> = { create: "created", edit: "edited", delete: "deleted" };

/* One change on the rail. A single step opens its diff; a restructure's parts
 * are listed under it, each opening its own. */
function Node(props: { item: Extract<TimelineItem<LapReviewStep>, { kind: "node" }>; purpose: string }): JSX.Element {
    const [open, setOpen] = useState(false);
    const { item } = props;
    const { name, dir } = splitPath(item.file);
    const many = item.steps.length > 1;
    const first = item.steps[0];
    const last = item.steps[item.steps.length - 1];
    const show = (id: string) => send({ type: "showEdit", commit: id, sessionMsg: props.purpose });
    const toggle = () => (many ? setOpen(!open) : show(first.id));
    return (
        <li className={`cb-tl-node cb-tl-${item.op}`}>
            <span className="cb-tl-dot" title={OP_TITLE[item.op] ?? item.op} aria-hidden="true">
                {item.op === "delete" ? <Icon name="close" /> : null}
            </span>
            <div
                className={`cb-tl-card${many ? " cb-tl-many" : ""}`}
                role="button"
                tabIndex={0}
                title={many ? `${item.steps.length} edits: show them` : `Show the diff of ${first.id}`}
                onClick={toggle}
                onKeyDown={(e) => {
                    if (e.key === "Enter") toggle();
                }}
            >
                <div className="cb-edit-head">
                    <span className="cb-edit-name">{name}</span>
                    {dir ? <span className="cb-edit-dir">{dir}</span> : null}
                    {item.op !== "edit" ? <span className="cb-label">{item.op}</span> : null}
                    {many ? <span className="cb-edit-count">{item.steps.length} edits</span> : null}
                    <span className="cb-tl-lines">
                        {item.op === "delete" && !many ? "" : `line${many || first.new_lines !== 1 ? "s" : ""} ${item.steps.map(linesOf).join(" · ")}`}
                    </span>
                    <code className="cb-edit-id">{many ? `${first.id}–${last.id}` : first.id}</code>
                </div>
                <div className="cb-edit-msg" title={item.msg}>
                    {item.msg}
                </div>
                {many ? (
                    <div className="cb-tl-more">
                        <Icon name={open ? "chevron-down" : "chevron-right"} /> {open ? "hide" : "show"} the {item.steps.length} parts
                    </div>
                ) : null}
            </div>
            {many && open ? (
                <ol className="cb-edit-parts">
                    {item.steps.map((s, i) => (
                        <li key={s.id} className="cb-commit" title={`Show the diff of ${s.id}`} onClick={() => show(s.id)}>
                            <Icon name="diff" /> <code>{s.id}</code>{" "}
                            <span className="cb-muted">
                                part {i + 1} of {item.steps.length} · line{s.new_lines === 1 ? "" : "s"} {linesOf(s)}
                            </span>
                        </li>
                    ))}
                </ol>
            ) : null}
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
