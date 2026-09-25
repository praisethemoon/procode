/* One view per kind of item. Each shows the item, what contains it (as links
 * back up) and what it contains (as links down), and edits in place. */

import { Button, Icon, TextArea } from "baukasten-ui/core";
import { useState } from "react";

import type { LapCommit } from "coboard/lap";
import { EPIC_STATUSES, MILESTONE_STATUSES, PRIORITIES, SIZES, TICKET_STATUSES } from "coboard/model";
import type { EpicView, MilestoneView, Summary, TicketView } from "coboard/query";
import type { Choices, Fields, Sessions } from "../src/protocol";
import { Description, IdLink, InlineText, Markdown, Pick, Progress, QuickAdd, StatusBadge } from "./parts";
import { send } from "./rpc";

const opts = (xs: readonly string[]) => xs.map((x) => ({ value: x, label: x }));

function update(id: string, fields: Fields): void {
    send({ type: "update", id, fields });
}

function Header(props: { id: string; title: string; trail: (Summary | null)[]; actions?: JSX.Element }): JSX.Element {
    return (
        <header className="cb-header">
            <div className="cb-trail">
                {props.trail
                    .filter((s): s is Summary => s !== null)
                    .map((s) => (
                        <span key={s.id}>
                            <IdLink id={s.id}>
                                {s.id} {s.title}
                            </IdLink>
                            <span className="cb-sep"> › </span>
                        </span>
                    ))}
                <span className="cb-muted">{props.id}</span>
            </div>
            <div className="cb-title-row">
                <h1>
                    <InlineText value={props.title} required onSave={(title) => update(props.id, { title })} />
                </h1>
                <div className="cb-row">
                    {props.actions}
                    <Button size="sm" variant="ghost" title={`Delete ${props.id}`} onClick={() => send({ type: "delete", id: props.id })}>
                        <Icon name="trash" />
                    </Button>
                </div>
            </div>
        </header>
    );
}

function TicketTable(props: { tickets: readonly Summary[]; empty: string }): JSX.Element {
    if (props.tickets.length === 0) {
        return <p className="cb-muted">{props.empty}</p>;
    }
    return (
        <table className="cb-table">
            <tbody>
                {props.tickets.map((t) => (
                    <tr key={t.id}>
                        <td className="cb-col-id">
                            <IdLink id={t.id} />
                        </td>
                        <td>
                            <IdLink id={t.id}>{t.title}</IdLink>
                            {(t.labels ?? []).map((l) => (
                                <span key={l} className="cb-label">
                                    {l}
                                </span>
                            ))}
                        </td>
                        <td className="cb-col-small">
                            <StatusBadge status={t.status} />
                        </td>
                        <td className="cb-col-small cb-muted">{t.size ?? ""}</td>
                        <td className="cb-col-small cb-muted">{t.priority}</td>
                        <td className="cb-col-small cb-muted">{t.assignee ?? ""}</td>
                    </tr>
                ))}
            </tbody>
        </table>
    );
}

/* ------------------------------------------------------------------ epic */

export function Epic(props: { v: EpicView }): JSX.Element {
    const { epic, milestones, tickets, counts } = props.v;
    return (
        <article>
            <Header id={epic.id} title={epic.title} trail={[]} />
            <div className="cb-fields">
                <Pick label="Status" value={epic.status} options={opts(EPIC_STATUSES)} onChange={(status) => update(epic.id, { status })} />
                <label className="cb-field">
                    <span className="cb-field-label">Progress</span>
                    <Progress counts={counts} />
                </label>
            </div>
            <Description value={epic.description} onSave={(description) => update(epic.id, { description })} />
            <section className="cb-section">
                <h3>Milestones</h3>
                {milestones.length === 0 ? (
                    <p className="cb-muted">No milestones yet.</p>
                ) : (
                    <table className="cb-table">
                        <tbody>
                            {milestones.map((m) => (
                                <tr key={m.id}>
                                    <td className="cb-col-id">
                                        <IdLink id={m.id} />
                                    </td>
                                    <td>
                                        <IdLink id={m.id}>{m.title}</IdLink>
                                    </td>
                                    <td className="cb-col-small">
                                        <StatusBadge status={m.status} />
                                    </td>
                                    <td className="cb-col-progress">
                                        <Progress counts={m.counts} />
                                    </td>
                                </tr>
                            ))}
                        </tbody>
                    </table>
                )}
                <QuickAdd placeholder="New milestone title" onAdd={(title) => send({ type: "create", kind: "milestone", title, epic: epic.id })} />
            </section>
            <section className="cb-section">
                <h3>Tickets in no milestone</h3>
                <TicketTable tickets={tickets} empty="Every ticket in this epic is in a milestone." />
                <QuickAdd placeholder="New ticket title" onAdd={(title) => send({ type: "create", kind: "ticket", title, epic: epic.id })} />
            </section>
        </article>
    );
}

/* ------------------------------------------------------------- milestone */

export function Milestone(props: { v: MilestoneView; choices: Choices }): JSX.Element {
    const { milestone, epic, tickets, counts } = props.v;
    return (
        <article>
            <Header id={milestone.id} title={milestone.title} trail={[epic]} />
            <div className="cb-fields">
                <Pick label="Status" value={milestone.status} options={opts(MILESTONE_STATUSES)} onChange={(status) => update(milestone.id, { status })} />
                <Pick
                    label="Epic"
                    value={milestone.epic}
                    options={props.choices.epics.map((e) => ({ value: e.id, label: `${e.id} ${e.title}` }))}
                    onChange={(to) => send({ type: "move", id: milestone.id, epic: to })}
                />
                <label className="cb-field">
                    <span className="cb-field-label">Progress</span>
                    <Progress counts={counts} />
                </label>
            </div>
            <Description value={milestone.description} onSave={(description) => update(milestone.id, { description })} />
            <section className="cb-section">
                <h3>Tickets</h3>
                <TicketTable tickets={tickets} empty="No tickets in this milestone yet." />
                <QuickAdd placeholder="New ticket title" onAdd={(title) => send({ type: "create", kind: "ticket", title, milestone: milestone.id })} />
            </section>
        </article>
    );
}

/* ---------------------------------------------------------------- ticket */

function SessionsSection(props: { ticket: string; sessions: Sessions | null; commits: Record<string, readonly LapCommit[] | string> }): JSX.Element {
    const [openRows, setOpen] = useState<Record<string, boolean>>({});
    const s = props.sessions;
    return (
        <section className="cb-section">
            <div className="cb-section-head">
                <h3>Work (lap sessions)</h3>
                <Button size="sm" variant="secondary" onClick={() => send({ type: "startSession", ticket: props.ticket })}>
                    <Icon name="record" /> Start session
                </Button>
            </div>
            {!s ? (
                <p className="cb-muted">Loading…</p>
            ) : !s.ok ? (
                <p className="cb-muted">lap could not answer: {s.error}</p>
            ) : s.sessions.length === 0 ? (
                <p className="cb-muted">
                    No lap session is linked to {props.ticket} yet. An agent links one with{" "}
                    <code>lap session start "{props.ticket}: …" --meta ticket={props.ticket}</code>.
                </p>
            ) : (
                <ul className="cb-sessions">
                    {s.sessions.map((x) => {
                        const expanded = openRows[x.id] === true;
                        const commits = props.commits[x.id];
                        return (
                            <li key={x.id}>
                                <div
                                    className="cb-session"
                                    onClick={() => {
                                        setOpen({ ...openRows, [x.id]: !expanded });
                                        if (!expanded && commits === undefined) send({ type: "commits", session: x.id });
                                    }}
                                >
                                    <Icon name={expanded ? "chevron-down" : "chevron-right"} />
                                    <strong>{x.id}</strong> <span>{x.msg.split("\n")[0]}</span>
                                    <span className="cb-muted">
                                        {" "}
                                        · {x.commits} commit{x.commits === 1 ? "" : "s"} · {x.started.slice(0, 10)}
                                        {x.active ? " · active" : x.ended ? "" : " · open"}
                                    </span>
                                </div>
                                {expanded && (
                                    <ul className="cb-commits">
                                        {commits === undefined ? (
                                            <li className="cb-muted">Loading…</li>
                                        ) : typeof commits === "string" ? (
                                            <li className="cb-muted">{commits}</li>
                                        ) : commits.length === 0 ? (
                                            <li className="cb-muted">No commits.</li>
                                        ) : (
                                            commits.map((c) => (
                                                <li
                                                    key={c.id}
                                                    className="cb-commit"
                                                    title={`Show the diff of ${c.id}`}
                                                    onClick={() => send({ type: "showEdit", commit: c.id })}
                                                >
                                                    <Icon name="diff" /> <code>{c.id}</code> <span className="cb-muted">{c.op}</span>{" "}
                                                    <code>{c.file}</code> — {c.msg.split("\n")[0]}
                                                </li>
                                            ))
                                        )}
                                    </ul>
                                )}
                            </li>
                        );
                    })}
                </ul>
            )}
        </section>
    );
}

function Comments(props: { ticket: TicketView["ticket"] }): JSX.Element {
    const [body, setBody] = useState("");
    const t = props.ticket;
    return (
        <section className="cb-section">
            <h3>Comments</h3>
            {t.comments.length === 0 && <p className="cb-muted">No comments yet.</p>}
            {t.comments.map((c, i) => (
                <div key={i} className="cb-comment">
                    <div className="cb-comment-head">
                        <strong>{c.author}</strong> <span className="cb-muted">{c.at.replace("T", " ").replace("Z", "")}</span>
                    </div>
                    <Markdown text={c.body} />
                </div>
            ))}
            <TextArea fullWidth minRows={3} maxRows={16} value={body} placeholder="Add a comment (Markdown)" onChange={(e) => setBody(e.target.value)} />
            <div className="cb-row">
                <Button
                    size="sm"
                    disabled={!body.trim()}
                    onClick={() => {
                        send({ type: "comment", ticket: t.id, body });
                        setBody("");
                    }}
                >
                    Comment
                </Button>
            </div>
        </section>
    );
}

export function Ticket(props: {
    v: TicketView;
    choices: Choices;
    sessions: Sessions | null;
    commits: Record<string, readonly LapCommit[] | string>;
}): JSX.Element {
    const { ticket: t, epic, milestone } = props.v;
    const milestones = props.choices.milestones.filter((m) => m.epic === t.epic);
    return (
        <article>
            <Header id={t.id} title={t.title} trail={[epic, milestone]} />
            <div className="cb-fields">
                <Pick label="Status" value={t.status} options={opts(TICKET_STATUSES)} onChange={(status) => update(t.id, { status })} />
                <Pick label="Priority" value={t.priority} options={opts(PRIORITIES)} onChange={(priority) => update(t.id, { priority })} />
                <Pick
                    label="Size"
                    value={t.size ?? ""}
                    options={[{ value: "", label: "—" }, ...opts(SIZES)]}
                    onChange={(size) => update(t.id, { size: size || null })}
                />
                <label className="cb-field">
                    <span className="cb-field-label">Assignee</span>
                    <InlineText value={t.assignee ?? ""} placeholder="nobody" onSave={(a) => update(t.id, { assignee: a || null })} />
                </label>
                <label className="cb-field">
                    <span className="cb-field-label">Labels</span>
                    <InlineText
                        value={t.labels.join(", ")}
                        placeholder="none"
                        onSave={(l) => update(t.id, { labels: l.split(",").map((x) => x.trim()).filter(Boolean) })}
                    />
                </label>
            </div>
            <div className="cb-fields">
                <Pick
                    label="Epic"
                    value={t.epic}
                    options={props.choices.epics.map((e) => ({ value: e.id, label: `${e.id} ${e.title}` }))}
                    onChange={(to) => send({ type: "move", id: t.id, epic: to })}
                />
                <Pick
                    label="Milestone"
                    value={t.milestone ?? ""}
                    options={[{ value: "", label: "— none —" }, ...milestones.map((m) => ({ value: m.id, label: `${m.id} ${m.title}` }))]}
                    onChange={(to) => send({ type: "move", id: t.id, milestone: to || null })}
                />
                <label className="cb-field">
                    <span className="cb-field-label">Updated</span>
                    <span className="cb-muted">{t.updated.replace("T", " ").replace("Z", "")}</span>
                </label>
            </div>
            <Description value={t.description} onSave={(description) => update(t.id, { description })} />
            <Comments ticket={t} />
            <SessionsSection ticket={t.id} sessions={props.sessions} commits={props.commits} />
        </article>
    );
}
