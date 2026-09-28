/* index-ui.md §2: the search bar at the top — the same bar as the Board's and
 * Lap History's filter — and below it the collections, or one collection's
 * documents.
 *
 * WITH NO QUERY THE SIDEBAR SHOWS THE COLLECTIONS, each with its document
 * count, size and oldest fetch; a collection opens its documents in the
 * sidebar, newest first, and a back control returns. Both lists end in "Load
 * more" rather than stopping at a limit (`view/rail.ts`). Rename and delete
 * are not here: one mis-click from losing a topic, they stay on the
 * Collections page (§4), a link at the top of the list.
 *
 * WITH A QUERY IT IS SEARCH RESULTS, DEBOUNCED, WITH NO SUBMIT BUTTON — across
 * the store from the collections, within the collection from inside it. The
 * debounce is `useDebounced` and the query is keyed on its settled value, so a
 * keystroke does not spawn a process and an in-flight answer for a prefix
 * cannot replace the answer for what is on screen.
 *
 * ROWS ARE NOT HIGHLIGHTED AND MATCHES ARE NOT MARKED UP. §2 is explicit. The
 * row's line is a string from `oneLine` and there is no shape here for a
 * `<mark>` to be inserted into.
 */

import { useEffect, useState } from "react";

import { KbCollection, KbDocument, KbHit, KbStatus } from "kb-js/pure";

import {
    Row,
    isSearching,
    leftToEmbed,
    pendingLine,
    searchQuery,
    searchRows,
    unembeddedNote,
} from "../src/view/rows";
import { formatBytes, formatDate } from "../src/view/facts";
import { HOME, Place, collectionsShown, cut, documentRow, documentsQuery, openCollection, searchScope } from "../src/view/rail";
import { Codicon, Resolved, StaleBadge, useDebounced, useQuery } from "./parts";
import { finishEmbedding, onHostEvent, open, tag } from "./rpc";

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

/* §2's default: the collections, a page at a time, each opening in place. */
function CollectionsList(props: { onOpen: (name: string) => void }): JSX.Element {
    const [pages, setPages] = useState(1);
    const { state } = useQuery<KbCollection[]>("collections");
    return (
        <Resolved state={state} loading="Reading the store…">
            {(collections) => {
                if (collections.length === 0) {
                    return <div className="kb-empty">{NOTHING_INDEXED}</div>;
                }
                const { shown, more } = collectionsShown(collections, pages);
                return (
                    <div className="kb-list">
                        <div className="kb-rail-head">
                            <span className="kb-muted">Collections</span>
                            {/* §4: rename and delete live on that page and nowhere else. */}
                            <button type="button" className="kb-action" onClick={() => open("collections")}>
                                Manage collections…
                            </button>
                        </div>
                        {shown.map((c) => (
                            <div
                                key={c.name}
                                className="kb-row kb-collection-row"
                                role="button"
                                tabIndex={0}
                                title={`Open ${c.name}`}
                                onClick={() => props.onOpen(c.name)}
                                onKeyDown={(e) => {
                                    if (e.key === "Enter" || e.key === " ") {
                                        e.preventDefault();
                                        props.onOpen(c.name);
                                    }
                                }}
                            >
                                <div className="kb-row-head">
                                    <Codicon name="folder" />
                                    <span className="kb-row-title">{c.name}</span>
                                </div>
                                <div className="kb-row-meta">
                                    <span>
                                        {c.documents} document{c.documents === 1 ? "" : "s"}
                                    </span>
                                    <span>{formatBytes(c.bytes)}</span>
                                    {c.oldestFetchedAt ? (
                                        <span className="kb-when" title={`oldest fetch ${c.oldestFetchedAt}`}>
                                            since {formatDate(c.oldestFetchedAt)}
                                        </span>
                                    ) : null}
                                </div>
                            </div>
                        ))}
                        {more ? <LoadMore onClick={() => setPages(pages + 1)} /> : null}
                    </div>
                );
            }}
        </Resolved>
    );
}

function LoadMore(props: { onClick: () => void }): JSX.Element {
    return (
        <div className="kb-more">
            <button type="button" className="kb-action" onClick={props.onClick}>
                Load more
            </button>
        </div>
    );
}

/* One collection's documents: a back control, then its pages. */
function CollectionView(props: { name: string; onBack: () => void }): JSX.Element {
    return (
        <div className="kb-list">
            <div className="kb-rail-head">
                <button type="button" className="kb-action kb-back" title="Back to the collections" onClick={props.onBack}>
                    <Codicon name="arrow-left" /> Collections
                </button>
                <span className="kb-row-title">{props.name}</span>
            </div>
            <DocumentsPage collection={props.name} after={null} />
        </div>
    );
}

/* A page of a collection's documents, newest first; Load more draws the
 * next page after this one's last row. */
function DocumentsPage(props: { collection: string; after: string | null }): JSX.Element {
    const [more, setMore] = useState(false);
    const { state } = useQuery<KbDocument[]>("ls", documentsQuery(props.collection, props.after));
    return (
        <Resolved state={state} loading="Reading the documents…">
            {(page) => {
                const { shown, next } = cut(page);
                if (shown.length === 0 && props.after === null) {
                    return <div className="kb-empty">Nothing in {props.collection} yet.</div>;
                }
                return (
                    <>
                        {shown.map((d) => (
                            <DocumentLine key={d.id} d={d} />
                        ))}
                        {next === null ? null : more ? (
                            <DocumentsPage collection={props.collection} after={next} />
                        ) : (
                            <LoadMore onClick={() => setMore(true)} />
                        )}
                    </>
                );
            }}
        </Resolved>
    );
}

function DocumentLine(props: { d: KbDocument }): JSX.Element {
    const row = documentRow(props.d);
    return (
        <div
            className="kb-row"
            role="button"
            tabIndex={0}
            title={`${row.reference} · ${row.title}`}
            onClick={() => open(row.reference, null)}
            onKeyDown={(e) => {
                if (e.key === "Enter" || e.key === " ") {
                    e.preventDefault();
                    open(row.reference, null);
                }
            }}
        >
            <div className="kb-row-head">
                <span className="kb-row-title">{row.title}</span>
            </div>
            {row.description !== null ? (
                <div className="kb-row-line kb-row-line-description" title={row.description}>
                    {row.description}
                </div>
            ) : null}
            <div className="kb-row-meta">
                <span>{formatBytes(row.bytes)}</span>
                <span>{row.mime}</span>
                <span className="kb-when" title={row.fetchedAt}>
                    {formatDate(row.fetchedAt)}
                </span>
            </div>
        </div>
    );
}

function SearchList(props: {
    q: string;
    collection: string;
    staleDays: number;
}): JSX.Element {
    const { state } = useQuery<{ hits: KbHit[]; count: number; unembedded?: number }>("search", {
        ...searchQuery(props.collection),
        q: props.q,
    });
    return (
        <Resolved state={state} loading="Searching…">
            {(result) => {
                const rows = searchRows(result.hits, Date.now(), props.staleDays);
                /* index-api §4: the store searched with the vectors it has.
                 * Said above the rows, because a passage the reader expected
                 * and does not see is explained by it. */
                const note = unembeddedNote(result.unembedded);
                const shown =
                    rows.length === 0 ? (
                        <div className="kb-empty">Nothing matched “{props.q}”.</div>
                    ) : (
                        <List rows={rows} />
                    );
                return note === null ? (
                    shown
                ) : (
                    <>
                        {/* No button of its own: the rail's line above the
                          * list already offers the pass that finishes them. */}
                        <div className="kb-notice kb-unembedded">{note}</div>
                        {shown}
                    </>
                );
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
    const [place, setPlace] = useState<Place>(HOME);
    const settled = useDebounced(q, DEBOUNCE_MS);
    const status = useQuery<KbStatus>("status");

    /* §4: "a collection row opens the sidebar scoped to it." The host reveals
     * this view and then says which one. */
    useEffect(
        () =>
            onHostEvent((r) => {
                if (r.kind === "scope") {
                    setPlace(r.collection === "" ? HOME : openCollection(r.collection));
                    setQ("");
                }
            }),
        [],
    );

    return (
        <div className="kb-view">
            <div className="kb-bar">
                <span className="kb-grow kb-search">
                    <input
                        className="kb-search-input"
                        type="text"
                        value={q}
                        placeholder={place.kind === "collection" ? `Search ${place.name}` : "Search"}
                        spellCheck={false}
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
                        /* index-api §2: filings embed within a budget and leave
                         * the rest pending. The rail says how many and offers
                         * the pass that finishes them. */
                        const pending = leftToEmbed(value);
                        return (
                            <>
                                {pending > 0 ? (
                                    <div className="kb-notice kb-pending">
                                        {pendingLine(pending)}{" "}
                                        <button type="button" className="kb-action" onClick={() => finishEmbedding()}>
                                            Finish embedding
                                        </button>
                                    </div>
                                ) : null}
                                {isSearching(settled) ? (
                                    <SearchList
                                        q={settled.trim()}
                                        collection={searchScope(place)}
                                        staleDays={tag.staleAfterDays}
                                    />
                                ) : place.kind === "collection" ? (
                                    <CollectionView name={place.name} onBack={() => setPlace(HOME)} />
                                ) : (
                                    <CollectionsList onOpen={(name) => setPlace(openCollection(name))} />
                                )}
                            </>
                        );
                    }}
                </Resolved>
            </div>
        </div>
    );
}
