/* One view per kind of item. Each shows the item, what contains it (as links
 * back up) and what it contains (as links down), and edits in place. */

import { Button, Icon, TextArea } from "baukasten-ui/core";
import { useState } from "react";

import type { LapCommit } from "coboard/lap";
import { EPIC_STATUSES, MILESTONE_STATUSES, PRIORITIES, SIZES, TICKET_STATUSES } from "coboard/model";
import type { EpicView, MilestoneView, Summary, TicketView } from "coboard/query";
import { CommitGroup, groupCommits, splitPath } from "../src/commits";
import { ViewMode, columns, moves } from "../src/kanban";
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

/* ------------------------------------------------------------- the board */

/* List or Kanban: one choice for the workspace, kept by the host, so every
 * epic and milestone tab switches together. */
function ModeToggle(props: { mode: ViewMode }): JSX.Element {
    const item = (mode: ViewMode, icon: string, label: string) => (
        <button
            type="button"
            className={`cb-mode${props.mode === mode ? " cb-mode-on" : ""}`}
            aria-pressed={props.mode === mode}
            title={`Show tickets as a ${label.toLowerCase()}`}
            onClick={() => send({ type: "mode", mode })}
        >
            <i className={`codicon codicon-${icon}`} aria-hidden="true" /> {label}
        </button>
    );
    return (
        <div className="cb-modes" role="group" aria-label="Show tickets as">
            {item("list", "list-unordered", "List")}
            {item("kanban", "layout", "Board")}
        </div>
    );
}

/* One column per status. A card is dragged to another column to change its
 * status — through the same update the ticket's own Status field sends — and
 * opened by clicking it. `showMilestone` names each card's milestone, for an
 * epic's board where cards from several milestones sit together. */
function Kanban(props: { tickets: readonly Summary[]; showMilestone?: boolean }): JSX.Element {
    const [over, setOver] = useState<string | null>(null);
    const byId = new Map(props.tickets.map((t) => [t.id, t]));
    return (
        <div className="cb-kanban">
            {columns(props.tickets).map((col) => (
                <div
                    key={col.status}
                    className={`cb-column${over === col.status ? " cb-over" : ""}`}
                    onDragOver={(e) => {
                        e.preventDefault();
                        setOver(col.status);
                    }}
                    onDragLeave={() => setOver((o) => (o === col.status ? null : o))}
                    onDrop={(e) => {
                        e.preventDefault();
                        setOver(null);
                        const t = byId.get(e.dataTransfer.getData("text/plain"));
                        if (t && moves(t, col.status)) update(t.id, { status: col.status });
                    }}
                >
                    <div className="cb-column-head">
                        <StatusBadge status={col.status} />
                        <span className="cb-muted">{col.tickets.length}</span>
                    </div>
                    {col.tickets.map((t) => (
                        <div
                            key={t.id}
                            className="cb-card"
                            draggable
                            role="button"
                            tabIndex={0}
                            onDragStart={(e) => {
                                e.dataTransfer.setData("text/plain", t.id);
                                e.dataTransfer.effectAllowed = "move";
                            }}
                            onClick={() => send({ type: "open", id: t.id })}
                            onKeyDown={(e) => {
                                if (e.key === "Enter") send({ type: "open", id: t.id });
                            }}
                        >
                            <div className="cb-card-title">{t.title}</div>
                            <div className="cb-card-meta">
                                <span className="cb-id">{t.id}</span>
                                {props.showMilestone && t.milestone ? <span className="cb-card-tag">{t.milestone}</span> : null}
                                {t.priority && t.priority !== "medium" ? (
                                    <span className={`cb-card-tag cb-pri-${t.priority}`}>{t.priority}</span>
                                ) : null}
                                {t.size ? <span className="cb-card-tag">{t.size}</span> : null}
                                {t.assignee ? <span className="cb-card-who">{t.assignee}</span> : null}
                            </div>
                        </div>
                    ))}
                </div>
            ))}
        </div>
    );
}

/* ------------------------------------------------------------------ epic */

export function Epic(props: { v: EpicView; mode: ViewMode }): JSX.Element {
    const { epic, milestones, tickets, allTickets, counts } = props.v;
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
            {props.mode === "kanban" ? (
                <section className="cb-section">
                    <div className="cb-section-head">
                        <h3>Tickets</h3>
                        <ModeToggle mode={props.mode} />
                    </div>
                    <Kanban tickets={allTickets} showMilestone />
                    <QuickAdd placeholder="New ticket title" onAdd={(title) => send({ type: "create", kind: "ticket", title, epic: epic.id })} />
                </section>
            ) : null}
            <section className="cb-section">
                <div className="cb-section-head">
                    <h3>Milestones</h3>
                    {props.mode === "list" ? <ModeToggle mode={props.mode} /> : null}
                </div>
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
            {props.mode === "list" ? (
                <section className="cb-section">
                    <h3>Tickets in no milestone</h3>
                    <TicketTable tickets={tickets} empty="Every ticket in this epic is in a milestone." />
                    <QuickAdd placeholder="New ticket title" onAdd={(title) => send({ type: "create", kind: "ticket", title, epic: epic.id })} />
                </section>
            ) : null}
        </article>
    );
}

/* ------------------------------------------------------------- milestone */

export function Milestone(props: { v: MilestoneView; choices: Choices; mode: ViewMode }): JSX.Element {
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
                <div className="cb-section-head">
                    <h3>Tickets</h3>
                    <ModeToggle mode={props.mode} />
                </div>
                {props.mode === "kanban" ? (
                    <Kanban tickets={tickets} />
                ) : (
                    <TicketTable tickets={tickets} empty="No tickets in this milestone yet." />
                )}
                <QuickAdd placeholder="New ticket title" onAdd={(title) => send({ type: "create", kind: "ticket", title, milestone: milestone.id })} />
            </section>
        </article>
    );
}

/* ---------------------------------------------------------------- ticket */

/* One lap edit or a group of them: the file's name with its folder muted, an
 * op tag only when the edit made or removed the file, the lap id(s), and the
 * reason underneath, clamped, with the whole of it on hover. */
function EditRow(props: { g: CommitGroup; sessionMsg: string }): JSX.Element {
    const [open, setOpen] = useState(false);
    const { g } = props;
    const { name, dir } = splitPath(g.file);
    const many = g.commits.length > 1;
    const first = g.commits[0];
    const last = g.commits[g.commits.length - 1];
    const show = (id: string) => send({ type: "showEdit", commit: id, sessionMsg: props.sessionMsg });
    return (
        <li className="cb-edit">
            <div
                className="cb-edit-row"
                role="button"
                tabIndex={0}
                title={many ? `${g.commits.length} edits: show them` : `Show the diff of ${first.id}`}
                onClick={() => (many ? setOpen(!open) : show(first.id))}
                onKeyDown={(e) => {
                    if (e.key === "Enter") (many ? setOpen(!open) : show(first.id));
                }}
            >
                <Icon name={many ? (open ? "chevron-down" : "chevron-right") : "diff"} />
                <div className="cb-edit-body">
                    <div className="cb-edit-head">
                        <span className="cb-edit-name">{name}</span>
                        {dir ? <span className="cb-edit-dir">{dir}</span> : null}
                        {g.op !== "edit" ? <span className="cb-label">{g.op}</span> : null}
                        {many ? <span className="cb-edit-count">{g.commits.length} edits</span> : null}
                        <code className="cb-edit-id">{many ? `${first.id}–${last.id}` : first.id}</code>
                    </div>
                    <div className="cb-edit-msg" title={g.msg}>
                        {g.msg}
                    </div>
                </div>
            </div>
            {many && open ? (
                <ol className="cb-edit-parts">
                    {g.commits.map((c, i) => (
                        <li key={c.id} className="cb-commit" title={`Show the diff of ${c.id}`} onClick={() => show(c.id)}>
                            <Icon name="diff" /> <code>{c.id}</code>{" "}
                            <span className="cb-muted">
                                part {i + 1} of {g.commits.length}
                            </span>
                        </li>
                    ))}
                </ol>
            ) : null}
        </li>
    );
}

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
                                    <button
                                        type="button"
                                        className="cb-review-button"
                                        title={`Review ${x.id}: what it changed and how`}
                                        onClick={(e) => {
                                            e.stopPropagation();
                                            send({ type: "review", session: x.id, ticket: props.ticket });
                                        }}
                                    >
                                        <Icon name="git-pull-request" /> Review
                                    </button>
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
                                            groupCommits(commits).map((g) => (
                                                <EditRow key={g.commits[0].id} g={g} sessionMsg={x.msg} />
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
