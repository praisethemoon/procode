/* Lap History: a filter bar above the log's sessions and commits, and a pager
 * under them.
 *
 * THE BAR. A chevron on the left opens the deeper filters (kind of change,
 * who, the session's state); the input matches ids, hashes, intents,
 * behaviors, session purposes and file paths; the dropdown at its right end
 * is the time range; the ✕, shown only while anything is set, clears all of
 * it.
 *
 * A COMMIT is its id, short hash, file and lines. Opened — by its twistie,
 * or by clicking it, which also opens its diff — it shows the intent, then
 * the behavior, and whether the message checks were skipped. References to
 * other commits in that text (`#<hash>`, `L<n>`) are links: the host
 * resolves one and answers with the page that shows it.
 *
 * THE HOST DOES THE WORK. This view sends what is set and gets back one page
 * (history.ts runs in the host, where the log is). What is set, the page and
 * what is open survive the view being hidden and VS Code restarting.
 */

import { Select } from "baukasten-ui/core";
import { StrictMode, useEffect, useRef, useState } from "react";
import { createRoot } from "react-dom/client";

import {
    CommitRow,
    EMPTY_FILTER,
    HistoryFilter,
    HistoryPage,
    OPS,
    RANGES,
    RANGE_LABELS,
    Range,
    STATES,
    STATE_LABELS,
    SessionRow,
    fieldCount,
    isFiltering,
} from "../src/history";
import { shortHash } from "../src/model";
import type { ToHost, ToView } from "../src/protocol";
import { parseRefs } from "../src/refs";

declare function acquireVsCodeApi(): {
    postMessage(m: unknown): void;
    getState(): unknown;
    setState(s: unknown): void;
};

const vscode = acquireVsCodeApi();
const send = (m: ToHost) => vscode.postMessage(m);

interface Saved {
    filter: HistoryFilter;
    page: number;
    /* Sessions opened or closed by hand; the rest follow the default. */
    open: Record<string, boolean>;
    /* Commits opened to their intent and behavior. */
    details: Record<string, boolean>;
}

function restore(): Saved {
    const s = vscode.getState() as Partial<Saved> | undefined;
    const f = s?.filter;
    const list = (v: unknown) => (Array.isArray(v) ? v.filter((x): x is string => typeof x === "string") : []);
    const filter: HistoryFilter =
        f && typeof f.text === "string"
            ? {
                  text: f.text,
                  range: (RANGES as readonly string[]).includes(f.range) ? f.range : "recent",
                  ops: list(f.ops),
                  users: list(f.users),
                  states: list(f.states).filter((x): x is HistoryFilter["states"][number] => (STATES as readonly string[]).includes(x)),
              }
            : EMPTY_FILTER;
    return {
        filter,
        page: typeof s?.page === "number" ? s.page : 0,
        open: s?.open && typeof s.open === "object" ? s.open : {},
        details: s?.details && typeof s.details === "object" ? s.details : {},
    };
}

function Codicon(props: { name: string; className?: string }): JSX.Element {
    return <i className={`codicon codicon-${props.name}${props.className ? ` ${props.className}` : ""}`} aria-hidden="true" />;
}

function toggle<T extends string>(list: readonly T[], v: T): T[] {
    return list.includes(v) ? list.filter((x) => x !== v) : [...list, v];
}

/* ------------------------------------------------------------ filter bar */

function FilterBar(props: { filter: HistoryFilter; users: readonly string[]; onChange: (f: HistoryFilter) => void }): JSX.Element {
    const { filter, onChange } = props;
    const [open, setOpen] = useState(false);
    const input = useRef<HTMLInputElement>(null);
    const count = fieldCount(filter);
    const groups: { label: string; values: readonly string[]; on: readonly string[]; set: (v: string) => HistoryFilter; name?: (v: string) => string }[] = [
        { label: "Change", values: OPS, on: filter.ops, set: (v) => ({ ...filter, ops: toggle(filter.ops, v) }) },
        { label: "Who", values: props.users, on: filter.users, set: (v) => ({ ...filter, users: toggle(filter.users, v) }) },
        {
            label: "Session",
            values: STATES,
            on: filter.states,
            set: (v) => ({ ...filter, states: toggle(filter.states, v as HistoryFilter["states"][number]) }),
            name: (v) => STATE_LABELS[v as HistoryFilter["states"][number]],
        },
    ];
    return (
        <div className="lh-filter">
            <div className="lh-top">
                <div className="lh-bar">
                    <button type="button" className="lh-chevron" title={open ? "Hide filters" : "More filters"} aria-expanded={open} onClick={() => setOpen(!open)}>
                        <Codicon name={open ? "chevron-down" : "chevron-right"} />
                        {count > 0 ? <span className="lh-count">{count}</span> : null}
                    </button>
                    <input
                        ref={input}
                        className="lh-input"
                        type="text"
                        placeholder="Filter by id, hash, text or file"
                        aria-label="Filter the history"
                        value={filter.text}
                        spellCheck={false}
                        onChange={(e) => onChange({ ...filter, text: e.currentTarget.value })}
                        onKeyDown={(e) => {
                            if (e.key === "Escape") onChange({ ...filter, text: "" });
                        }}
                    />
                    {isFiltering(filter) ? (
                        <button
                            type="button"
                            className="lh-clear"
                            title="Clear all filters"
                            aria-label="Clear all filters"
                            onClick={() => {
                                onChange(EMPTY_FILTER);
                                input.current?.focus();
                            }}
                        >
                            <Codicon name="close" />
                        </button>
                    ) : null}
                </div>
                <div className={`lh-range${filter.range !== "recent" ? " lh-on" : ""}`} title="Time range">
                    <Select<Range>
                        size="sm"
                        value={filter.range}
                        options={RANGES.map((r) => ({ value: r, label: RANGE_LABELS[r] }))}
                        onChange={(range) => onChange({ ...filter, range })}
                    />
                </div>
            </div>
            {open ? (
                <div className="lh-fields">
                    {groups.map((g) =>
                        g.values.length === 0 ? null : (
                            <div className="lh-group" key={g.label}>
                                <div className="lh-group-label">{g.label}</div>
                                <div className="lh-chips">
                                    {g.values.map((v) => {
                                        const on = g.on.includes(v);
                                        return (
                                            <button
                                                type="button"
                                                key={v}
                                                className={`lh-chip${on ? " lh-on" : ""}`}
                                                aria-pressed={on}
                                                onClick={() => onChange(g.set(v))}
                                            >
                                                {on ? <Codicon name="check" /> : null}
                                                {g.name ? g.name(v) : v}
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

/* ------------------------------------------------------------------ rows */

const OP_ICON: Record<string, string> = { create: "diff-added", delete: "diff-removed" };

const FORCED = "forced: the message checks were skipped (--force-message)";

/* A commit's text with each reference to another commit a link. */
function Linked(props: { text: string }): JSX.Element {
    return (
        <span className="lh-text">
            {parseRefs(props.text).map((p, i) =>
                p.ref === undefined ? (
                    p.text
                ) : (
                    <a
                        key={i}
                        className="lh-ref"
                        href="#"
                        title={`Show ${p.ref} in the history`}
                        onClick={(e) => {
                            e.preventDefault();
                            send({ type: "reveal", ref: p.ref! });
                        }}
                    >
                        {p.text}
                    </a>
                ),
            )}
        </span>
    );
}

/* What a commit row shows and does, from the view: whether it is opened to
 * its detail, whether it is the one selected, and how to change either. */
interface CommitState {
    readonly details: Record<string, boolean>;
    readonly selected: string | null;
    readonly setDetail: (id: string, open: boolean) => void;
    readonly select: (id: string) => void;
}

function CommitLine(props: { c: CommitRow; depth: number; showSession: boolean; state: CommitState }): JSX.Element {
    const { c, state } = props;
    const detail = state.details[c.id] ?? false;
    const open = () => {
        state.select(c.id);
        state.setDetail(c.id, true);
        send({ type: "open", id: c.id });
    };
    const indent = `${props.depth} * var(--lh-indent)`;
    return (
        <>
            <div
                className={`lh-row${state.selected === c.id ? " lh-selected" : ""}`}
                style={{ paddingLeft: `calc(${indent} + 4px)` }}
                role="treeitem"
                aria-level={props.depth + 1}
                aria-expanded={detail}
                aria-selected={state.selected === c.id}
                data-commit={c.id}
                tabIndex={0}
                title={
                    `${c.id} ${shortHash(c.hash)} · ${c.file} · ${c.region} · ${c.ts}` +
                    `${c.session ? ` · session ${c.session}` : " · no session"}${c.user ? ` · ${c.user}` : ""}` +
                    `\n\nIntent: ${c.intent}\n\nBehavior: ${c.behavior}${c.forced ? `\n\n${FORCED}` : ""}`
                }
                onClick={open}
                onKeyDown={(e) => {
                    if (e.key === "Enter") open();
                    else if (e.key === "ArrowRight" || e.key === "ArrowLeft") state.setDetail(c.id, e.key === "ArrowRight");
                }}
            >
                <span
                    className="lh-twistie"
                    title={detail ? "Hide the intent and behavior" : "Show the intent and behavior"}
                    onClick={(e) => {
                        e.stopPropagation();
                        state.setDetail(c.id, !detail);
                    }}
                >
                    <Codicon name={detail ? "chevron-down" : "chevron-right"} />
                </span>
                <Codicon name={OP_ICON[c.op] ?? "diff-modified"} className={`lh-icon lh-${c.op}`} />
                <span className="lh-id">{c.id}</span>
                <span className="lh-hash">{shortHash(c.hash)}</span>
                {c.forced ? <Codicon name="warning" className="lh-icon lh-forced" /> : null}
                <span className="lh-desc">
                    {c.file} · {c.region}
                    {props.showSession && c.session ? ` · ${c.session}` : ""}
                </span>
            </div>
            {detail ? (
                <div className="lh-detail" style={{ paddingLeft: `calc(${indent} + 42px)` }} role="group" aria-label={`${c.id} intent and behavior`}>
                    <div className="lh-field">
                        <span className="lh-label">Intent:</span> <Linked text={c.intent} />
                    </div>
                    <div className="lh-field">
                        <span className="lh-label">Behavior:</span> <Linked text={c.behavior} />
                    </div>
                    {c.forced ? (
                        <div className="lh-forced-note">
                            <Codicon name="warning" className="lh-forced" /> {FORCED}
                        </div>
                    ) : null}
                </div>
            ) : null}
        </>
    );
}

function SessionLine(props: { s: SessionRow; open: boolean; filtering: boolean; onToggle: () => void; commits: CommitState }): JSX.Element {
    const { s } = props;
    const shown = s.commits.length;
    const count =
        props.filtering && shown < s.total ? `${shown} of ${s.total} commits` : `${s.total} commit${s.total === 1 ? "" : "s"}`;
    const icon = s.state === "active" ? "play-circle" : s.state === "none" ? "circle-slash" : "milestone";
    const hasKids = shown > 0;
    return (
        <>
            <div
                className="lh-row"
                style={{ paddingLeft: "4px" }}
                role="treeitem"
                aria-expanded={hasKids ? props.open : undefined}
                aria-level={1}
                tabIndex={0}
                title={`${s.id ?? "no session"} · ${s.msg}\n\nstarted ${s.ts}${s.endTs ? `, ended ${s.endTs}` : s.state === "active" ? " · active" : ""}`}
                onClick={() => hasKids && props.onToggle()}
                onKeyDown={(e) => {
                    if (hasKids && (e.key === "Enter" || e.key === "ArrowRight" || e.key === "ArrowLeft")) {
                        if (e.key === "Enter" || (e.key === "ArrowRight") !== props.open) props.onToggle();
                    }
                }}
            >
                <span className="lh-twistie">{hasKids ? <Codicon name={props.open ? "chevron-down" : "chevron-right"} /> : null}</span>
                <Codicon name={icon} className={`lh-icon${s.state === "active" ? " lh-active" : ""}`} />
                {s.id ? <span className="lh-id">{s.id}</span> : null}
                <span className="lh-title">{s.summary}</span>
                <span className="lh-desc">
                    {count}
                    {s.state === "active" ? " · active" : ""}
                </span>
            </div>
            {props.open ? s.commits.map((c) => <CommitLine key={c.id} c={c} depth={1} showSession={false} state={props.commits} />) : null}
        </>
    );
}

function Pager(props: { page: HistoryPage; onPage: (p: number) => void }): JSX.Element | null {
    const p = props.page;
    if (p.total === 0) return null;
    const from = p.page * p.pageSize + 1;
    const to = Math.min(p.total, from + p.pageSize - 1);
    const what = p.grouped ? (p.total === 1 ? "session" : "sessions") : p.total === 1 ? "commit" : "commits";
    return (
        <div className="lh-pager">
            <button type="button" className="lh-page" title="Previous page" aria-label="Previous page" disabled={p.page === 0} onClick={() => props.onPage(p.page - 1)}>
                <Codicon name="chevron-left" />
            </button>
            <span className="lh-pager-text" title={`page ${p.page + 1} of ${p.pages}`}>
                {from}–{to} of {p.total} {what}
                {p.pages > 1 ? ` · ${p.page + 1}/${p.pages}` : ""}
            </span>
            <button type="button" className="lh-page" title="Next page" aria-label="Next page" disabled={p.page >= p.pages - 1} onClick={() => props.onPage(p.page + 1)}>
                <Codicon name="chevron-right" />
            </button>
        </div>
    );
}

/* ------------------------------------------------------------------ view */

function History(): JSX.Element {
    const saved = useRef(restore()).current;
    const [filter, setFilter] = useState<HistoryFilter>(saved.filter);
    const [page, setPage] = useState(saved.page);
    const [open, setOpen] = useState<Record<string, boolean>>(saved.open);
    const [details, setDetails] = useState<Record<string, boolean>>(saved.details);
    const [selected, setSelected] = useState<string | null>(null);
    /* A commit just revealed, to scroll to once it is drawn. */
    const [revealed, setRevealed] = useState<string | null>(null);
    const [data, setData] = useState<{ page: HistoryPage | null; hasRepo: boolean; active: string | null } | null>(null);
    const list = useRef<HTMLDivElement>(null);
    const shown = useRef<readonly SessionRow[]>([]);
    shown.current = data?.page?.sessions ?? [];

    /* The host answers in the order it was asked, so the last page to arrive
     * is the one for what is set now. Its page number is the host's, clamped
     * to what exists; the pager works from that. */
    useEffect(() => {
        const handler = (e: MessageEvent) => {
            const m = e.data as ToView;
            if (m.type === "page") {
                setData({ page: m.page, hasRepo: m.hasRepo, active: m.active });
                if (m.reveal && m.page) {
                    /* The host chose the filter and page that show the
                     * commit; the view takes them, opens its session and the
                     * commit itself, and selects it. */
                    const id = m.reveal.id;
                    const home = m.page.sessions.find((s) => s.commits.some((c) => c.id === id));
                    setFilter(m.reveal.filter);
                    setPage(m.page.page);
                    if (home) setOpen((o) => ({ ...o, [home.id ?? "none"]: true }));
                    setDetails((d) => ({ ...d, [id]: true }));
                    setSelected(id);
                    setRevealed(id);
                }
            } else if (m.type === "collapseAll") {
                setOpen((o) => {
                    const next = { ...o };
                    for (const s of shown.current) next[s.id ?? "none"] = false;
                    return next;
                });
            }
        };
        window.addEventListener("message", handler);
        return () => window.removeEventListener("message", handler);
    }, []);

    /* Ask for the page whenever what is set changes; the text waits for a
     * pause in typing. */
    useEffect(() => {
        const t = setTimeout(() => send({ type: "query", filter, page }), filter.text ? 120 : 0);
        return () => clearTimeout(t);
    }, [filter, page]);

    useEffect(() => {
        vscode.setState({ filter, page, open, details } satisfies Saved);
    }, [filter, page, open, details]);

    useEffect(() => {
        if (revealed === null) return;
        const row = list.current?.querySelector<HTMLElement>(`[data-commit="${revealed}"]`);
        if (row) {
            row.scrollIntoView({ block: "center" });
            row.focus({ preventScroll: true });
            setRevealed(null);
        }
    }, [revealed, data]);

    const change = (f: HistoryFilter) => {
        setFilter(f);
        setPage(0);
    };
    const goTo = (p: number) => {
        setPage(p);
        list.current?.scrollTo({ top: 0 });
    };

    if (data && !data.hasRepo) {
        return (
            <p className="lh-empty">
                No lap repository in this workspace. Create one with <code>lap init</code>, and commits appear here as they are made.
            </p>
        );
    }
    const p = data?.page ?? null;
    const filtering = isFiltering(filter);
    const isOpen = (s: SessionRow) => open[s.id ?? "none"] ?? (s.state === "active" || (filtering && !s.matchedSelf));
    const commits: CommitState = {
        details,
        selected,
        setDetail: (id, on) =>
            setDetails((d) => {
                const next = { ...d };
                if (on) next[id] = true;
                else delete next[id];
                return next;
            }),
        select: setSelected,
    };
    return (
        <div className="lh">
            <FilterBar filter={filter} users={p?.users ?? []} onChange={change} />
            <div className="lh-list" role="tree" aria-label="Lap history" ref={list}>
                {p === null ? (
                    <p className="lh-empty">Reading the log…</p>
                ) : p.total === 0 ? (
                    <p className="lh-empty">{filtering ? "Nothing matches the filter." : "No commits yet. They appear here as they are made."}</p>
                ) : p.grouped ? (
                    p.sessions.map((s) => (
                        <SessionLine
                            key={s.id ?? "none"}
                            s={s}
                            open={isOpen(s)}
                            filtering={filtering}
                            onToggle={() => setOpen({ ...open, [s.id ?? "none"]: !isOpen(s) })}
                            commits={commits}
                        />
                    ))
                ) : (
                    p.commits.map((c) => <CommitLine key={c.id} c={c} depth={0} showSession={true} state={commits} />)
                )}
            </div>
            {p ? <Pager page={p} onPage={goTo} /> : null}
        </div>
    );
}

createRoot(document.getElementById("root")!).render(
    <StrictMode>
        <History />
    </StrictMode>,
);
