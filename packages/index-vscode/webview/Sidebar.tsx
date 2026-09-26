/* index-ui.md §2: a search `Input` at the top, a collection `Select` beside it,
 * and a list below.
 *
 * WITH NO QUERY THE LIST IS EVERY DOCUMENT IN SCOPE, NEWEST FIRST. §2 says so
 * and says why: "browsing is the default state, not an empty prompt — the store
 * is worth looking through even when there is no question." This is the
 * property most likely to regress into a "type to search" placeholder, so the
 * branch is `isSearching(q)` from `view/rows.ts`, which has a test, and the two
 * lists are two operations rather than one with an optional argument.
 *
 * WITH A QUERY IT IS SEARCH RESULTS, DEBOUNCED, WITH NO SUBMIT BUTTON. The
 * debounce is `useDebounced` and the query is keyed on its settled value, so a
 * keystroke does not spawn a process and an in-flight answer for a prefix
 * cannot replace the answer for what is on screen.
 *
 * ROWS ARE NOT HIGHLIGHTED AND MATCHES ARE NOT MARKED UP. §2 is explicit. The
 * row's line is a string from `oneLine` and there is no shape here for a
 * `<mark>` to be inserted into.
 *
 * THE SELECT'S OWN WIDTH FLOOR IS CANCELLED IN THE STYLESHEET AND NOT HERE.
 * baukasten's `Select` carries `min-width: calc(var(--bk-spacing-20) * 2.5)` on
 * its own root — 162.5px, wider than a VSCode sidebar at its narrowest — and
 * `fullWidth` loses to it, because a min-width clamps a width from below. The
 * cancel has to land on the CONTROL rather than on the wrapper, and it must not
 * be `overflow: hidden`, which would clip the open dropdown.
 */

import { useEffect, useState } from "react";
import { Input, Select } from "baukasten-ui/core";

import { KbCollection, KbDocument, KbHit, KbStatus } from "kb-js/pure";

import {
    BROWSE_LIMIT,
    Row,
    browseCut,
    browseQuery,
    browseRows,
    isSearching,
    searchQuery,
    searchRows,
} from "../src/view/rows";
import { formatDate } from "../src/view/facts";
import { Codicon, Resolved, StaleBadge, useDebounced, useQuery } from "./parts";
import { onHostEvent, open, tag } from "./rpc";

/* §2's debounce. Short enough that the list feels like it is following the
 * typing and long enough that a word costs one search rather than five. */
const DEBOUNCE_MS = 180;

/* §2's empty state, verbatim. */
const NOTHING_INDEXED = "Nothing indexed yet. Research lands here when an agent files what it read.";

/* §1.4's one store, found by walking up from the workspace folder. `path` is
 * null when the walk found nothing, and `present` says the same thing. */
function hasStore(status: KbStatus): boolean {
    return status.present && status.path !== null;
}

/* ------------------------------------------------------------- the row */

function KnowledgeRow(props: { row: Row }): JSX.Element {
    const row = props.row;
    const clamped = row.line !== row.lineFull;
    return (
        <div
            className="kb-row"
            role="button"
            tabIndex={0}
            title={`${row.reference} · ${row.title}`}
            onClick={() => open(row.reference, row.chunk)}
            onKeyDown={(e) => {
                if (e.key === "Enter" || e.key === " ") {
                    e.preventDefault();
                    open(row.reference, row.chunk);
                }
            }}
        >
            <div className="kb-row-head">
                <span className="kb-row-title">{row.title === "" ? row.reference : row.title}</span>
                {row.stale ? <StaleBadge /> : null}
            </div>
            {/* §2's one-line snippet, clamped, with the full text on hover. The
              * hover is only set when there is more to see — a title attribute
              * repeating what is already on screen is a tooltip that teaches
              * the reader to ignore tooltips. */}
            <div
                className={`kb-row-line kb-row-line-${row.lineKind}`}
                title={clamped ? row.lineFull : undefined}
            >
                {row.line}
            </div>
            <div className="kb-row-meta">
                <span className="kb-chip">{row.collection === "" ? "—" : row.collection}</span>
                <span className="kb-when" title={row.fetchedAt}>
                    {formatDate(row.fetchedAt)}
                </span>
                {/* §2: "which retrieval paths matched, when the row came from a
                  * search". §4: a result found by both is a different kind of
                  * result from one found by either, so both are named rather
                  * than reduced to one word. */}
                {row.matched.map((m) => (
                    <span key={m} className={`kb-match kb-match-${m}`}>
                        {m}
                    </span>
                ))}
            </div>
        </div>
    );
}

/* ---------------------------------------------------------- the two lists */

function BrowseList(props: { collection: string; staleDays: number }): JSX.Element {
    const [all, setAll] = useState(false);
    useEffect(() => setAll(false), [props.collection]);
    const { state } = useQuery<KbDocument[]>("ls", browseQuery(props.collection, all));
    return (
        <Resolved state={state} loading="Reading the store…">
            {(documents) => {
                const { shown, more } = browseCut(documents);
                const rows = browseRows(all ? documents : shown, Date.now(), props.staleDays);
                if (rows.length === 0) {
                    return (
                        <div className="kb-empty">
                            {props.collection === ""
                                ? NOTHING_INDEXED
                                : `Nothing in ${props.collection} yet.`}
                        </div>
                    );
                }
                return (
                    <>
                        <List rows={rows} />
                        {more && !all ? (
                            <div className="kb-empty kb-more">
                                Only the first {BROWSE_LIMIT} documents are listed; there are more.{" "}
                                <button type="button" className="kb-action" onClick={() => setAll(true)}>
                                    Show all
                                </button>
                            </div>
                        ) : null}
                    </>
                );
            }}
        </Resolved>
    );
}

function SearchList(props: {
    q: string;
    collection: string;
    staleDays: number;
}): JSX.Element {
    const { state } = useQuery<{ hits: KbHit[]; count: number }>("search", {
        ...searchQuery(props.collection),
        q: props.q,
    });
    return (
        <Resolved state={state} loading="Searching…">
            {(result) => {
                const rows = searchRows(result.hits, Date.now(), props.staleDays);
                if (rows.length === 0) {
                    return <div className="kb-empty">Nothing matched “{props.q}”.</div>;
                }
                return <List rows={rows} />;
            }}
        </Resolved>
    );
}

function List(props: { rows: readonly Row[] }): JSX.Element {
    return (
        <div className="kb-list">
            {props.rows.map((row, i) => (
                /* The key carries the index because a search can return two
                 * chunks of one document, and two rows keyed on the same
                 * reference would make React reuse one of them. */
                <KnowledgeRow key={`${row.reference}:${row.chunk ?? i}`} row={row} />
            ))}
            <div className="kb-count">
                {props.rows.length} document{props.rows.length === 1 ? "" : "s"}
            </div>
        </div>
    );
}

/* ------------------------------------------------------------ the rail */

export function Sidebar(): JSX.Element {
    const [q, setQ] = useState("");
    const [collection, setCollection] = useState("");
    const settled = useDebounced(q, DEBOUNCE_MS);
    const status = useQuery<KbStatus>("status");
    const collections = useQuery<KbCollection[]>("collections");

    /* §4: "a collection row opens the sidebar scoped to it." The host reveals
     * this view and then says which one. */
    useEffect(
        () =>
            onHostEvent((r) => {
                if (r.kind === "scope") {
                    setCollection(r.collection);
                    setQ("");
                }
            }),
        [],
    );

    const names =
        collections.state.status === "ok"
            ? [...new Set(collections.state.value.map((c) => c.name))].sort()
            : [];

    return (
        <div className="kb-view">
            <div className="kb-bar">
                <span className="kb-grow kb-search">
                    <Input
                        className="kb-search-input"
                        value={q}
                        placeholder="Search"
                        size="sm"
                        fullWidth
                        aria-label="Search the knowledge base"
                        onChange={(e) => setQ(e.currentTarget.value)}
                        onKeyDown={(e) => {
                            /* VSCode's own Search box clears on Escape, and so
                             * does this. Guarded on there being text, so an
                             * Escape on an empty box still reaches whatever
                             * VSCode would have done with it. */
                            if (e.key === "Escape" && q !== "") {
                                e.preventDefault();
                                setQ("");
                            }
                        }}
                    />
                    {q !== "" ? (
                        <button
                            type="button"
                            className="kb-search-clear"
                            aria-label="Clear the search text"
                            title="Clear the search text"
                            onClick={() => setQ("")}
                        >
                            <Codicon name="close" />
                        </button>
                    ) : null}
                </span>
                <span className="kb-narrow">
                    {/* baukasten's `Select` takes an `id` and no `aria-label`,
                      * so the name comes from a real label associated with it.
                      * The label is off-screen rather than absent: the rail is
                      * one control wide and a visible one would take the width
                      * the box beside it is there for, but a control with no
                      * name at all is a control nothing that is not a pair of
                      * eyes can use. */}
                    <label className="kb-sr-only" htmlFor="kb-collection">
                        Collection
                    </label>
                    <Select<string>
                        id="kb-collection"
                        size="sm"
                        fullWidth
                        value={collection}
                        options={[
                            { value: "", label: "All" },
                            ...names.map((n) => ({ value: n, label: n })),
                        ]}
                        placeholder="All"
                        onChange={(next) => setCollection(typeof next === "string" ? next : "")}
                    />
                </span>
            </div>
            <div className="kb-scroll">
                <Resolved state={status.state} loading="Looking for a store…">
                    {(value) => {
                        if (!hasStore(value)) {
                            return (
                                <div className="kb-empty">
                                    <p>
                                        <strong>No knowledge store here.</strong> There is no{" "}
                                        <code>.kb/</code> in this folder or in any folder above it.
                                    </p>
                                    <p>
                                        Run <code>Knowledge: Initialise A Store In This Workspace</code>{" "}
                                        from the command palette. It creates <code>.kb/</code>; the
                                        logs and the blobs are the truth and are worth committing,
                                        and everything under <code>index/</code> is derived.
                                    </p>
                                </div>
                            );
                        }
                        return isSearching(settled) ? (
                            <SearchList
                                q={settled.trim()}
                                collection={collection}
                                staleDays={tag.staleAfterDays}
                            />
                        ) : (
                            <BrowseList collection={collection} staleDays={tag.staleAfterDays} />
                        );
                    }}
                </Resolved>
            </div>
        </div>
    );
}
