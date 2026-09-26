/* The Board sidebar: a filter bar above the tree of epics, milestones and
 * tickets.
 *
 * THE BAR. A chevron on the left opens the field filters (status, kind,
 * priority, size, assignee, label); the input filters by id or title as you
 * type; the ✕ inside the input on the right, shown only while anything is
 * set, clears all of it. What is set survives the view being hidden and
 * VS Code restarting (the webview's own state).
 *
 * THE TREE. Epics, their milestones, and tickets under either — the same
 * shape the native tree had. Right-click is VS Code's own menu, driven by
 * each row's `data-vscode-context`; the + buttons on a hovered row are the
 * common actions without it.
 */

import { StrictMode, useEffect, useMemo, useRef, useState } from "react";
import { createRoot } from "react-dom/client";

import type { Summary } from "coboard";
import { EMPTY, FIELDS, FIELD_LABELS, Filter, clear, counts, fieldCount, hasQuery, isActive, options, toggle, visible } from "../src/filter";
import type { SidebarToHost, SidebarToView } from "../src/protocol";

declare function acquireVsCodeApi(): {
    postMessage(m: unknown): void;
    getState(): unknown;
    setState(s: unknown): void;
};

const vscode = acquireVsCodeApi();
const send = (m: SidebarToHost) => vscode.postMessage(m);

interface Saved {
    filter: Filter;
    collapsed: string[];
}

function restore(): Saved {
    const s = vscode.getState() as Partial<Saved> | undefined;
    const f = s?.filter;
    const filter: Filter =
        f && typeof f.text === "string" && f.fields
            ? { text: f.text, fields: { ...EMPTY.fields, ...f.fields }, openOnly: f.openOnly === true, archived: f.archived === true }
            : EMPTY;
    return { filter, collapsed: Array.isArray(s?.collapsed) ? s!.collapsed : [] };
}

const STATUS_ICON: Record<string, string> = {
    todo: "circle-large-outline",
    doing: "play-circle",
    blocked: "error",
    review: "eye",
    done: "pass-filled",
    open: "circle-large-outline",
};

function iconOf(s: Summary): string {
    if (s.kind === "epic") return s.status === "done" ? "pass" : "project";
    if (s.kind === "milestone") return s.status === "done" ? "pass" : "milestone";
    return STATUS_ICON[s.status] ?? "circle-large-outline";
}

function Codicon(props: { name: string; className?: string }): JSX.Element {
    return <i className={`codicon codicon-${props.name}${props.className ? ` ${props.className}` : ""}`} aria-hidden="true" />;
}

/* ------------------------------------------------------------ filter bar */

function FilterBar(props: {
    filter: Filter;
    all: readonly Summary[];
    onChange: (f: Filter) => void;
}): JSX.Element {
    const { filter, onChange } = props;
    const [open, setOpen] = useState(false);
    const input = useRef<HTMLInputElement>(null);
    const opts = useMemo(() => options(props.all), [props.all]);
    const tally = useMemo(() => counts(props.all, filter), [props.all, filter]);
    const count = fieldCount(filter);
    const active = hasQuery(filter);

    return (
        <div className="sb-filter">
            <div className={`sb-bar${open ? " sb-open" : ""}`}>
                <button
                    type="button"
                    className="sb-chevron"
                    title={open ? "Hide filters" : "More filters"}
                    aria-expanded={open}
                    onClick={() => setOpen(!open)}
                >
                    <Codicon name={open ? "chevron-down" : "chevron-right"} />
                    {count > 0 ? <span className="sb-count">{count}</span> : null}
                </button>
                <input
                    ref={input}
                    className="sb-input"
                    type="text"
                    placeholder="Filter by id or title"
                    aria-label="Filter the board"
                    value={filter.text}
                    spellCheck={false}
                    onChange={(e) => onChange({ ...filter, text: e.currentTarget.value })}
                    onKeyDown={(e) => {
                        if (e.key === "Escape") onChange({ ...filter, text: "" });
                    }}
                />
                {active ? (
                    <button
                        type="button"
                        className="sb-clear"
                        title="Clear all filters"
                        aria-label="Clear all filters"
                        onClick={() => {
                            onChange(clear(filter));
                            input.current?.focus();
                        }}
                    >
                        <Codicon name="close" />
                    </button>
                ) : null}
                <button
                    type="button"
                    className={`sb-quick${filter.openOnly ? " sb-on" : ""}`}
                    title={filter.openOnly ? "Showing open items only: click to show done ones too" : "Open items only: hide done tickets, epics and milestones"}
                    aria-label="Open items only"
                    aria-pressed={filter.openOnly}
                    onClick={() => onChange({ ...filter, openOnly: !filter.openOnly })}
                >
                    <Codicon name="issues" />
                </button>
                <button
                    type="button"
                    className={`sb-quick${filter.archived ? " sb-on" : ""}`}
                    title={filter.archived ? "Showing archived items: click to hide them" : "Include archived items"}
                    aria-label="Include archived items"
                    aria-pressed={filter.archived}
                    onClick={() => onChange({ ...filter, archived: !filter.archived })}
                >
                    <Codicon name="archive" />
                </button>
            </div>
            {open ? (
                <div className="sb-fields">
                    {FIELDS.map((field) =>
                        opts[field].length === 0 ? null : (
                            <div className="sb-group" key={field}>
                                <div className="sb-group-label">{FIELD_LABELS[field]}</div>
                                <div className="sb-chips">
                                    {opts[field].map((value) => {
                                        const on = filter.fields[field].includes(value);
                                        const n = tally[field].get(value) ?? 0;
                                        return (
                                            <button
                                                type="button"
                                                key={value}
                                                className={`sb-chip${on ? " sb-on" : ""}${n === 0 ? " sb-none" : ""}`}
                                                aria-pressed={on}
                                                title={`${n} item${n === 1 ? "" : "s"}`}
                                                onClick={() => onChange(toggle(filter, field, value))}
                                            >
                                                {on ? <Codicon name="check" /> : null}
                                                {value}
                                                <span className="sb-chip-n">{n}</span>
                                            </button>
                                        );
                                    })}
                                </div>
                            </div>
                        ),
                    )}
                </div>
            ) : null}
        </div>
    );
}

/* ------------------------------------------------------------------ tree */

interface Node {
    s: Summary;
    children: Node[];
}

function build(all: readonly Summary[], shown: Set<string>): Node[] {
    const keep = all.filter((s) => shown.has(s.id));
    const node = (s: Summary): Node => ({ s, children: [] });
    const epics = keep.filter((s) => s.kind === "epic").map(node);
    const byId = new Map<string, Node>(epics.map((n) => [n.s.id, n]));
    for (const m of keep.filter((s) => s.kind === "milestone")) {
        const n = node(m);
        byId.set(m.id, n);
        byId.get(m.epic ?? "")?.children.push(n);
    }
    for (const t of keep.filter((s) => s.kind === "ticket")) {
        const parent = (t.milestone ? byId.get(t.milestone) : undefined) ?? byId.get(t.epic ?? "");
        parent?.children.push(node(t));
    }
    return epics;
}

function Row(props: {
    n: Node;
    depth: number;
    collapsed: Set<string>;
    forceOpen: boolean;
    matched: Set<string>;
    filtering: boolean;
    onToggle: (id: string) => void;
}): JSX.Element {
    const { n, depth, collapsed, forceOpen, matched, filtering } = props;
    const s = n.s;
    const hasKids = n.children.length > 0;
    const expanded = hasKids && (forceOpen || !collapsed.has(s.id));
    const context = JSON.stringify({
        webviewSection: s.kind,
        id: s.id,
        coboardArchived: s.archived === true,
        preventDefaultContextMenuItems: true,
    });
    const dim = filtering && !matched.has(s.id);
    return (
        <>
            <div
                className={`sb-row${dim ? " sb-dim" : ""}${s.archived ? " sb-row-archived" : ""}`}
                style={{ paddingLeft: `calc(${depth} * var(--sb-indent) + var(--bk-spacing-1))` }}
                role="treeitem"
                aria-expanded={hasKids ? expanded : undefined}
                aria-level={depth + 1}
                tabIndex={0}
                data-vscode-context={context}
                title={`${s.id} — ${s.title}\n${s.kind}, ${s.status}${s.archived ? ", archived" : ""}`}
                onClick={() => send({ type: "open", id: s.id })}
                onKeyDown={(e) => {
                    if (e.key === "Enter") send({ type: "open", id: s.id });
                    if (hasKids && (e.key === "ArrowRight" || e.key === "ArrowLeft")) {
                        if ((e.key === "ArrowRight") !== expanded) props.onToggle(s.id);
                    }
                }}
            >
                <span
                    className="sb-twistie"
                    onClick={(e) => {
                        e.stopPropagation();
                        if (hasKids) props.onToggle(s.id);
                    }}
                >
                    {hasKids ? <Codicon name={expanded ? "chevron-down" : "chevron-right"} /> : null}
                </span>
                <Codicon name={iconOf(s)} className={`sb-icon sb-${s.kind === "ticket" ? s.status : s.kind}`} />
                <span className="sb-title">{s.title}</span>
                {s.archived ? <Codicon name="archive" className="sb-archived" /> : null}
                <span className="sb-desc">{s.kind === "ticket" ? `${s.id} · ${s.status}` : s.id}</span>
                <span className="sb-actions">
                    {s.kind === "epic" ? (
                        <button
                            type="button"
                            title="New Milestone"
                            onClick={(e) => {
                                e.stopPropagation();
                                send({ type: "command", command: "coboard.newMilestone", id: s.id });
                            }}
                        >
                            <Codicon name="milestone" />
                        </button>
                    ) : null}
                    {s.kind !== "ticket" ? (
                        <button
                            type="button"
                            title="New Ticket"
                            onClick={(e) => {
                                e.stopPropagation();
                                send({ type: "command", command: "coboard.newTicket", id: s.id });
                            }}
                        >
                            <Codicon name="new-file" />
                        </button>
                    ) : null}
                </span>
            </div>
            {expanded
                ? n.children.map((c) => (
                      <Row
                          key={c.s.id}
                          n={c}
                          depth={depth + 1}
                          collapsed={collapsed}
                          forceOpen={forceOpen}
                          matched={matched}
                          filtering={filtering}
                          onToggle={props.onToggle}
                      />
                  ))
                : null}
        </>
    );
}

/* ------------------------------------------------------------------- app */

function App(): JSX.Element {
    const saved = useMemo(restore, []);
    const [all, setAll] = useState<readonly Summary[] | null>(null);
    const [hasFolder, setHasFolder] = useState(true);
    const [filter, setFilter] = useState<Filter>(saved.filter);
    const [collapsed, setCollapsed] = useState<Set<string>>(new Set(saved.collapsed));

    useEffect(() => {
        const handler = (e: MessageEvent) => {
            const m = e.data as SidebarToView;
            if (m.type === "items") {
                setAll(m.items);
                setHasFolder(m.hasFolder);
            } else if (m.type === "collapseAll") {
                setCollapsed(new Set((allRef.current ?? []).filter((s) => s.kind !== "ticket").map((s) => s.id)));
            }
        };
        window.addEventListener("message", handler);
        send({ type: "ready" });
        return () => window.removeEventListener("message", handler);
    }, []);
    const allRef = useRef(all);
    allRef.current = all;

    useEffect(() => {
        vscode.setState({ filter, collapsed: [...collapsed] } satisfies Saved);
    }, [filter, collapsed]);

    if (!hasFolder) {
        return <p className="sb-empty">Open a folder to use its board.</p>;
    }
    if (all === null) {
        return <p className="sb-empty">Reading the board…</p>;
    }
    const filtering = isActive(filter);
    const { shown, matched } = visible(all, filter);
    const roots = build(all, shown);
    const toggleNode = (id: string) =>
        setCollapsed((c) => {
            const next = new Set(c);
            if (next.has(id)) next.delete(id);
            else next.add(id);
            return next;
        });

    return (
        <div className="sb">
            <FilterBar filter={filter} all={all} onChange={setFilter} />
            {all.length === 0 ? (
                <div className="sb-empty">
                    <p>No epics yet. Epics hold milestones, and milestones hold tickets.</p>
                    <button type="button" className="sb-button" onClick={() => send({ type: "command", command: "coboard.newEpic" })}>
                        New Epic
                    </button>
                </div>
            ) : roots.length === 0 ? (
                <p className="sb-empty">Nothing matches the filter.</p>
            ) : (
                <div className="sb-tree" role="tree" aria-label="Board">
                    {roots.map((n) => (
                        <Row
                            key={n.s.id}
                            n={n}
                            depth={0}
                            collapsed={collapsed}
                            forceOpen={hasQuery(filter)}
                            matched={matched}
                            filtering={filtering}
                            onToggle={toggleNode}
                        />
                    ))}
                </div>
            )}
        </div>
    );
}

createRoot(document.getElementById("root")!).render(
    <StrictMode>
        <App />
    </StrictMode>,
);
