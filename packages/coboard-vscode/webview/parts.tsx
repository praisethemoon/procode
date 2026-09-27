/* The pieces every item view is made of. */

import { Button, IconButton, Icon, Input, Select, TextArea } from "baukasten-ui/core";
import { useEffect, useState, type ReactNode } from "react";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";

import type { LapMessage } from "coboard/lap";
import type { Counts } from "coboard/model";
import { TICKET_STATUSES } from "coboard/model";
import { shortHash } from "../src/commits";
import { commitRefs, linkTarget, linkifyIds } from "../src/linkify";
import { open, send } from "./rpc";

/* A link that opens an item. Shown as the bare id in monospace, or as
 * whatever children it is given (a title) in the body font. */
export function IdLink(props: { id: string; children?: ReactNode }): JSX.Element {
    return (
        <a
            className={props.children === undefined ? "cb-id" : "cb-link"}
            href={`#${props.id}`}
            onClick={(e) => {
                e.preventDefault();
                open(props.id);
            }}
        >
            {props.children ?? props.id}
        </a>
    );
}

/* A lap commit's intent or behavior as plain text, with the commits it names
 * ("#1a2b3c4", "L1029") as links that open that commit's diff. A link inside
 * a clickable row opens its own commit, not the row's. */
export function CommitText(props: { text: string }): JSX.Element {
    return (
        <>
            {commitRefs(props.text).map((p, i) =>
                typeof p === "string" ? (
                    p
                ) : (
                    <a
                        key={i}
                        className="cb-id cb-ref"
                        href={`#${p.ref}`}
                        title={`Show the diff of ${p.text}`}
                        onClick={(e) => {
                            e.preventDefault();
                            e.stopPropagation();
                            send({ type: "showEdit", commit: p.ref });
                        }}
                    >
                        {p.text}
                    </a>
                ),
            )}
        </>
    );
}

/* Marks a commit recorded with --force-message, past lap's message checks. */
export function ForcedTag(): JSX.Element {
    return (
        <span className="cb-label cb-forced" title="Committed with --force-message: lap's message checks were bypassed">
            forced
        </span>
    );
}

/* Marks a commit whose intent and behavior lap amend corrected. */
export function AmendedTag(props: { n: number }): JSX.Element {
    return (
        <span className="cb-label cb-amended" title="Corrected with lap amend: this is the latest text; lap show lists the earlier ones">
            {props.n === 1 ? "amended" : `amended ${props.n}×`}
        </span>
    );
}

/* One lap commit under its group's intent: what it makes the code do, a mark
 * when it was forced, where it is when given, and its short hash and id.
 * Opens the commit's diff. */
export function CommitLine(props: { commit: LapMessage & { id: string; hash: string }; lines?: string; onOpen: () => void }): JSX.Element {
    const c = props.commit;
    return (
        <li
            className="cb-step"
            role="button"
            tabIndex={0}
            title={`Show the diff of ${c.id} (#${shortHash(c.hash)})\n\n${c.behavior}`}
            onClick={props.onOpen}
            onKeyDown={(e) => {
                if (e.key === "Enter") props.onOpen();
            }}
        >
            <Icon name="diff" />
            <span className="cb-step-behavior">
                <CommitText text={c.behavior} />
            </span>
            {c.forced ? <ForcedTag /> : null}
            {c.amended ? <AmendedTag n={c.amended} /> : null}
            {props.lines ? <span className="cb-step-lines">{props.lines}</span> : null}
            <code className="cb-step-hash">{shortHash(c.hash)}</code>
            <code className="cb-step-id">{c.id}</code>
        </li>
    );
}

/* Markdown, with item ids turned into links that open the item. */
export function Markdown(props: { text: string; empty?: string }): JSX.Element {
    if (!props.text.trim()) {
        return <p className="cb-muted">{props.empty ?? "Nothing written yet."}</p>;
    }
    return (
        <div className="cb-md">
            <ReactMarkdown
                remarkPlugins={[remarkGfm]}
                components={{
                    a: ({ href, children }) => {
                        const id = linkTarget(href);
                        return id ? <IdLink id={id}>{children}</IdLink> : <a href={href}>{children}</a>;
                    },
                }}
            >
                {linkifyIds(props.text)}
            </ReactMarkdown>
        </div>
    );
}

/* A status, coloured only when it asks for attention: doing, review and
 * blocked are filled; todo is an outline and done a muted check, so a list of
 * finished work stays quiet and the open work in it stands out. */
export function StatusBadge(props: { status: string }): JSX.Element {
    return (
        <span className={`cb-status cb-status-${props.status}`}>
            {props.status === "done" ? <i className="codicon codicon-check" aria-hidden="true" /> : null}
            {props.status}
        </span>
    );
}

/* A single-line value that becomes an input on click and saves on Enter or
 * blur; Escape abandons the edit. */
export function InlineText(props: {
    value: string;
    placeholder?: string;
    onSave: (v: string) => void;
    className?: string;
    required?: boolean;
}): JSX.Element {
    const [editing, setEditing] = useState(false);
    const [draft, setDraft] = useState(props.value);
    useEffect(() => setDraft(props.value), [props.value]);
    if (!editing) {
        return (
            <span className={`cb-inline ${props.className ?? ""}`} title="Click to edit" onClick={() => setEditing(true)}>
                {props.value || <span className="cb-muted">{props.placeholder ?? "—"}</span>}
            </span>
        );
    }
    const commit = () => {
        setEditing(false);
        const v = draft.trim();
        if (v !== props.value && (v || !props.required)) props.onSave(v);
        else setDraft(props.value);
    };
    return (
        <Input
            autoFocus
            size="sm"
            value={draft}
            placeholder={props.placeholder}
            onChange={(e) => setDraft(e.target.value)}
            onBlur={commit}
            onKeyDown={(e) => {
                if (e.key === "Enter") commit();
                if (e.key === "Escape") {
                    setDraft(props.value);
                    setEditing(false);
                }
            }}
        />
    );
}

/* A Markdown description, read by default, edited in a text area. */
export function Description(props: { value: string; onSave: (v: string) => void }): JSX.Element {
    const [editing, setEditing] = useState(false);
    const [draft, setDraft] = useState(props.value);
    useEffect(() => {
        if (!editing) setDraft(props.value);
    }, [props.value, editing]);
    return (
        <section className="cb-section">
            <div className="cb-section-head">
                <h3>Description</h3>
                {!editing && <IconButton icon={<Icon name="edit" />} size="sm" variant="ghost" title="Edit" onClick={() => setEditing(true)} />}
            </div>
            {editing ? (
                <div className="cb-editor">
                    <TextArea fullWidth minRows={8} maxRows={30} value={draft} onChange={(e) => setDraft(e.target.value)} autoFocus />
                    <div className="cb-row">
                        <Button
                            size="sm"
                            onClick={() => {
                                props.onSave(draft);
                                setEditing(false);
                            }}
                        >
                            Save
                        </Button>
                        <Button size="sm" variant="secondary" onClick={() => setEditing(false)}>
                            Cancel
                        </Button>
                        <span className="cb-muted">Markdown. Mention items by id, e.g. T-4.</span>
                    </div>
                </div>
            ) : (
                <Markdown text={props.value} empty="No description yet." />
            )}
        </section>
    );
}

export function Pick(props: {
    label: string;
    value: string;
    options: readonly { value: string; label?: string }[];
    onChange: (v: string) => void;
}): JSX.Element {
    return (
        <label className="cb-field">
            <span className="cb-field-label">{props.label}</span>
            <Select<string> size="sm" value={props.value} options={[...props.options]} onChange={(v) => v !== props.value && props.onChange(v)} />
        </label>
    );
}

/* Done out of total, as text and a bar. */
export function Progress(props: { counts: Counts }): JSX.Element {
    const total = Object.values(props.counts).reduce((a, b) => a + b, 0);
    const done = props.counts["done"] ?? 0;
    const pct = total ? Math.round((done / total) * 100) : 0;
    return (
        <span className="cb-progress" title={TICKET_STATUSES.map((s) => `${s}: ${props.counts[s] ?? 0}`).join("  ")}>
            <span className="cb-bar">
                <span className="cb-bar-fill" style={{ width: `${pct}%` }} />
            </span>
            <span className="cb-muted">
                {done}/{total} done
            </span>
        </span>
    );
}

/* A one-field form that creates something. */
export function QuickAdd(props: { placeholder: string; onAdd: (title: string) => void }): JSX.Element {
    const [title, setTitle] = useState("");
    const add = () => {
        if (title.trim()) {
            props.onAdd(title.trim());
            setTitle("");
        }
    };
    return (
        <div className="cb-row cb-quickadd">
            <Input size="sm" fullWidth value={title} placeholder={props.placeholder} onChange={(e) => setTitle(e.target.value)} onKeyDown={(e) => e.key === "Enter" && add()} />
            <Button size="sm" variant="secondary" onClick={add} disabled={!title.trim()}>
                Add
            </Button>
        </div>
    );
}
