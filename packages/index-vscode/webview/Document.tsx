/* index-ui.md §3: a document, at `kb:/D-241`.
 *
 * THE PROVENANCE IS ALWAYS VISIBLE, and §3.1 gives the reason rather than a
 * preference: "a passage whose age and origin are unknown is a passage that
 * will eventually be trusted when it should not be." So the header is not a
 * collapsible, not a hover, and not behind a tab — it is the first thing in the
 * document and it carries the locator, the dates, the size, the type and the
 * chunk count.
 *
 * ANYTHING ELSE IN `meta` IS A PLAIN KEY/VALUE LIST. index-api.md §1.2 makes it
 * free-form per document, so this shows what is there and asserts nothing about
 * what should be — no known-key list, no key treated specially, and a rendering
 * per JSON shape so an array of authors is not `[object Object]`.
 *
 * READ-ONLY. There is no editing affordance anywhere in this file, and §3 says
 * why: editing an indexed copy of somebody else's documentation would make the
 * content hash meaningless and the provenance a lie. The editor is registered
 * as a `CustomReadonlyEditorProvider` so VSCode does not offer a save either.
 *
 * NO MATCH HIGHLIGHTING. §3.2. Opening a search result scrolls to the matching
 * chunk's heading and stops there.
 */

import { useEffect, useState } from "react";

import { KbChunk, KbDocument, KbSourceRead, KbSourceRefreshed, isStale } from "kb-js/pure";

import { documentFacts, formatDate, metaEntries } from "../src/view/facts";
import { revealId } from "../src/view/headings";
import { mimeLabel, renderingFor } from "../src/view/mime";
import { RefreshOutcome, outcomeMessage } from "../src/refresh";
import { Body } from "./Body";
import { Codicon, Resolved, StaleBadge, useQuery } from "./parts";
import { call, link, notify, onHostEvent, open, setTitle, tag } from "./rpc";

interface Read {
    document: KbDocument;
    text?: string | null;
    chunks?: KbChunk[] | null;
}

/* §3.2's scroll: to the chunk's heading where the document is drawn with
 * headings, to its first line where it is drawn line by line (code, plain
 * text). `revealId` in view/headings.ts decides which; a target that resolves
 * to nothing leaves the document at the top, where it would have been anyway.
 *
 * IT WAITS FOR THE BODY. The reveal message can arrive before the text has come
 * back from the store, and `getElementById` on a document that has not rendered
 * answers null. The effect re-runs when the read changes, so the scroll happens
 * on whichever of the two arrives second. */
function useReveal(read: Read | null): void {
    const [pending, setPending] = useState<string | null>(null);

    useEffect(
        () =>
            onHostEvent((r) => {
                if (r.kind === "reveal") {
                    setPending(r.chunk);
                }
            }),
        [],
    );

    useEffect(() => {
        if (read === null || typeof read.text !== "string" || pending === null) {
            return;
        }
        const chunk = (read.chunks ?? []).find((c) => c.id === pending);
        const id = chunk === undefined ? "" : revealId(chunk, renderingFor(read.document.mime), read.text);
        const el = id === "" ? null : document.getElementById(id);
        if (el !== null) {
            el.scrollIntoView({ block: "start" });
        }
        /* Cleared whether or not it landed. A target that stays pending would
         * re-scroll on every later render, which would fight a reader who had
         * scrolled somewhere else. */
        setPending(null);
    }, [read, pending]);
}

function Provenance(props: { document: KbDocument; onRefresh: () => void }): JSX.Element {
    const d = props.document;
    const stale = isStale(d.fetchedAt, Date.now(), tag.staleAfterDays);
    const label = mimeLabel(d.mime);
    const meta = metaEntries(d.meta);
    return (
        <header className="kb-head">
            <h1 className="kb-head-title">{d.title === "" ? d.id : d.title}</h1>
            <div className="kb-head-line">
                <span className="kb-ref">{d.id}</span>
                {/* §3.1: "the source locator, as a link that opens the original
                  * externally". A button rather than an anchor — nothing in
                  * this webview navigates, and the host checks the scheme and
                  * confirms before the system handler sees it. */}
                {d.locator === "" ? (
                    <span className="kb-muted">no locator</span>
                ) : (
                    <button
                        type="button"
                        className="kb-link kb-locator"
                        title={d.locator}
                        onClick={() => link(d.locator)}
                    >
                        <Codicon name="link-external" />
                        {d.locator}
                    </button>
                )}
                {stale ? (
                    <span className="kb-spread">
                        <StaleBadge />
                        {/* §3.1: "stale badge with a refresh action". */}
                        <button type="button" className="kb-action" onClick={props.onRefresh}>
                            <Codicon name="sync" />
                            Refresh
                        </button>
                    </span>
                ) : null}
            </div>
            <dl className="kb-facts">
                {documentFacts(d).map((fact) => (
                    <div className="kb-fact" key={fact.label}>
                        <dt>{fact.label}</dt>
                        <dd title={fact.title}>
                            {fact.label === "Type" && label !== null ? `${fact.value} (${label})` : fact.value}
                        </dd>
                    </div>
                ))}
            </dl>
            {/* §3.1's free-form meta, "as a plain key/value list below". */}
            {meta.length > 0 ? (
                <dl className="kb-facts kb-meta">
                    {meta.map((entry) => (
                        <div className="kb-fact" key={entry.key}>
                            <dt>{entry.key}</dt>
                            <dd title={entry.value}>{entry.value}</dd>
                        </div>
                    ))}
                </dl>
            ) : null}
        </header>
    );
}

export function DocumentView(props: { reference: string }): JSX.Element {
    const { state, refresh } = useQuery<Read>("get", { id: props.reference });
    useReveal(state.status === "ok" ? state.value : null);

    useEffect(() => {
        if (state.status === "ok") {
            /* §1 of UI.md's discipline, which §3 inherits: a tab's title is
             * read from the store rather than stored on the tab. */
            setTitle(props.reference, state.value.document.title);
        }
    }, [state, props.reference]);

    /* §3.1's refresh: this document, from its own source. The outcome is
     * said out loud, because "unchanged" and "re-indexed" look identical on
     * the page and "cannot" would otherwise look like nothing at all. */
    const onRefresh = (): void => {
        call<RefreshOutcome>("refreshDocument", { id: props.reference })
            .then((outcome) => {
                notify(outcome.outcome === "cannot" ? "warning" : "info", outcomeMessage(props.reference, outcome));
                refresh();
            })
            .catch((e: unknown) => {
                notify("warning", e instanceof Error ? e.message : String(e));
            });
    };

    return (
        <div className="kb-view kb-doc">
            <Resolved state={state} loading="Reading the document…">
                {(read) => (
                    <div className="kb-scroll">
                        <Provenance document={read.document} onRefresh={onRefresh} />
                        {typeof read.text === "string" ? (
                            <Body text={read.text} mime={read.document.mime} />
                        ) : (
                            <div className="kb-empty">
                                The store has this document's metadata and not its text. The blob it
                                points at is missing, which <code>kb rebuild</code> is what repairs.
                            </div>
                        )}
                    </div>
                )}
            </Resolved>
        </div>
    );
}

/* §6: `kb:/S-3`, "a source, with its documents".
 *
 * BUILT OUT OF THE DOCUMENTS, BECAUSE THE CLI HAS NO COMMAND FOR A SOURCE.
 * index-api.md §2 has `GET /sources/{id}`; `kb --help` has `init`, `add`, `ls`,
 * `get`, `collections` and `status`. Every field §1.2 puts on a `Source` except
 * `kind`, `etag` and `status` is derivable from the documents under it —
 * `kb ls --source S-3` carries the locator, the collection and the fetch dates
 * — so this is that derivation, stated once, with the gap named rather than
 * papered over.
 */
/* index-ui §6's `kb:/S-3`: a source, with its documents. Read through
 * `kb sources show`, so the page has the source's own kind and locator and
 * every time its documents were fetched, not only what its documents say. A
 * file source can be read again from here (`kb refresh S-n`); the outcome is
 * said out loud, as a document's refresh is. */
export function SourceView(props: { reference: string }): JSX.Element {
    const { state, refresh } = useQuery<KbSourceRead>("source", { id: props.reference });
    const onRefresh = (): void => {
        call<KbSourceRefreshed>("refreshSource", { id: props.reference })
            .then((r) => {
                notify(
                    "info",
                    r.changed
                        ? `${props.reference} changed on disk and ${r.document} was re-indexed.`
                        : `${props.reference} is unchanged on disk; its fetch date is now ${r.fetchedAt}.`,
                );
                refresh();
            })
            .catch((e: unknown) => notify("warning", e instanceof Error ? e.message : String(e)));
    };
    return (
        <div className="kb-view kb-doc">
            <Resolved state={state} loading="Reading the source…">
                {({ source, documents, history }) => (
                    <div className="kb-scroll">
                        <header className="kb-head">
                            <h1 className="kb-head-title">{source.title === "" ? source.id : source.title}</h1>
                            <div className="kb-head-line">
                                <span className="kb-ref">{source.id}</span>
                                {source.kind === "inline" ? null : (
                                    <button
                                        type="button"
                                        className="kb-link kb-locator"
                                        title={source.locator}
                                        onClick={() => link(source.locator)}
                                    >
                                        <Codicon name="link-external" />
                                        {source.locator}
                                    </button>
                                )}
                                {source.kind === "file" || source.kind === "url" ? (
                                    <button
                                        type="button"
                                        className="kb-link"
                                        title={
                                            source.kind === "url"
                                                ? "Fetch the page again (asked first) and re-index it if it changed"
                                                : "Read the file again and re-index it if it changed"
                                        }
                                        onClick={onRefresh}
                                    >
                                        <Codicon name="sync" /> Refresh
                                    </button>
                                ) : null}
                            </div>
                            <dl className="kb-facts">
                                <div className="kb-fact">
                                    <dt>Kind</dt>
                                    <dd>{source.kind}</dd>
                                </div>
                                <div className="kb-fact">
                                    <dt>Collection</dt>
                                    <dd>{source.collection}</dd>
                                </div>
                                <div className="kb-fact">
                                    <dt>Documents</dt>
                                    <dd>{source.docCount}</dd>
                                </div>
                                <div className="kb-fact">
                                    <dt>Bytes</dt>
                                    <dd>{source.bytes}</dd>
                                </div>
                                <div className="kb-fact">
                                    <dt>Fetched</dt>
                                    <dd title={source.fetchedAt}>
                                        {source.fetchedAt === "" ? "never" : formatDate(source.fetchedAt)}
                                    </dd>
                                </div>
                            </dl>
                        </header>
                        {documents.length === 0 ? (
                            <div className="kb-empty">
                                No documents under {source.id}. A source with nothing in it is what an
                                interrupted ingest leaves behind, and the next ingest of the same locator
                                reuses it.
                            </div>
                        ) : (
                            <div className="kb-list">
                                {documents.map((d) => (
                                    <div
                                        key={d.id}
                                        className="kb-row"
                                        role="button"
                                        tabIndex={0}
                                        onClick={() => open(d.id)}
                                        onKeyDown={(e) => {
                                            if (e.key === "Enter" || e.key === " ") {
                                                e.preventDefault();
                                                open(d.id);
                                            }
                                        }}
                                    >
                                        <div className="kb-row-head">
                                            <span className="kb-row-title">{d.title === "" ? d.id : d.title}</span>
                                        </div>
                                        <div className="kb-row-meta">
                                            <span className="kb-ref">{d.id}</span>
                                            <span className="kb-when" title={d.fetchedAt}>
                                                {formatDate(d.fetchedAt)}
                                            </span>
                                        </div>
                                    </div>
                                ))}
                            </div>
                        )}
                        {history.length === 0 ? null : (
                            <section className="kb-history">
                                <h2 className="kb-section-title">Fetch history</h2>
                                <ul className="kb-history-list">
                                    {[...history].reverse().map((f, i) => (
                                        <li key={i}>
                                            <span className="kb-when" title={f.fetchedAt}>
                                                {formatDate(f.fetchedAt)}
                                            </span>{" "}
                                            <span className="kb-ref">{f.document}</span>{" "}
                                            <span className="kb-muted">{f.changed ? "new text, indexed" : "unchanged"}</span>
                                        </li>
                                    ))}
                                </ul>
                            </section>
                        )}
                    </div>
                )}
            </Resolved>
        </div>
    );
}
