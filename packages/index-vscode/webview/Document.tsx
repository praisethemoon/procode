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

import { KbChunk, KbDocument, isStale } from "kb-js/pure";

import { documentFacts, formatDate, metaEntries } from "../src/view/facts";
import { headingId } from "../src/view/headings";
import { mimeLabel } from "../src/view/mime";
import { Body } from "./Body";
import { Codicon, Resolved, StaleBadge, useQuery } from "./parts";
import { call, link, notify, onHostEvent, open, setTitle, tag } from "./rpc";

interface Read {
    document: KbDocument;
    text?: string | null;
    chunks?: KbChunk[] | null;
}

/* §3.2's scroll, and the two ways it can fail to land.
 *
 * THE HEADING MAY NOT BE IN THE DOCUMENT AS AN ELEMENT AT ALL. A chunk in a
 * plain-text document has no heading (index-api.md §1.2 writes it `heading?`),
 * and a sliding-window chunk in a PDF has one only by accident. So a target
 * that resolves to nothing leaves the document at the top — which is where it
 * would have been anyway — rather than throwing or scrolling somewhere
 * arbitrary.
 *
 * IT WAITS FOR THE BODY. The reveal message can arrive before the text has come
 * back from the store, and `getElementById` on a document that has not rendered
 * answers null. The effect re-runs when the text changes, so the scroll happens
 * on whichever of the two arrives second. */
function useReveal(ready: boolean): void {
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
        if (!ready || pending === null) {
            return;
        }
        /* The chunk id becomes a heading here rather than in the host: the
         * document has already read its own chunk list, so the lookup is local
         * and costs no second call into the store. */
        const chunk = window.__KB_CHUNKS__?.find((c) => c.id === pending);
        const id = chunk?.heading === undefined || chunk.heading === null ? "" : headingId(chunk.heading);
        const el = id === "" ? null : document.getElementById(id);
        if (el !== null) {
            el.scrollIntoView({ block: "start" });
        }
        /* Cleared whether or not it landed. A target that stays pending would
         * re-scroll on every later render, which would fight a reader who had
         * scrolled somewhere else. */
        setPending(null);
    }, [ready, pending]);
}

declare global {
    interface Window {
        /* The chunk list of the document currently rendered, so the reveal
         * handler can turn a chunk id into a heading without threading it
         * through every component between the two. It is set by the one
         * component that has the list and read by the one hook that needs it;
         * a context would be four files for one lookup. */
        __KB_CHUNKS__?: readonly KbChunk[];
    }
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
    const ready = state.status === "ok" && typeof state.value.text === "string";
    useReveal(ready);

    useEffect(() => {
        if (state.status === "ok") {
            window.__KB_CHUNKS__ = state.value.chunks ?? [];
            /* §1 of UI.md's discipline, which §3 inherits: a tab's title is
             * read from the store rather than stored on the tab. */
            setTitle(props.reference, state.value.document.title);
        }
        return () => {
            window.__KB_CHUNKS__ = undefined;
        };
    }, [state, props.reference]);

    const onRefresh = (): void => {
        call("refresh", { document: props.reference })
            .then(() => refresh())
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
export function SourceView(props: { reference: string }): JSX.Element {
    const { state } = useQuery<KbDocument[]>("ls", { source: props.reference });
    return (
        <div className="kb-view kb-doc">
            <Resolved state={state} loading="Reading the source…">
                {(documents) => {
                    if (documents.length === 0) {
                        return (
                            <div className="kb-empty">
                                No documents under {props.reference}. A source with nothing in it is
                                what an interrupted ingest leaves behind, and the next ingest of the
                                same locator reuses it.
                            </div>
                        );
                    }
                    const first = documents[0];
                    const bytes = documents.reduce((n, d) => n + d.bytes, 0);
                    return (
                        <div className="kb-scroll">
                            <header className="kb-head">
                                <h1 className="kb-head-title">
                                    {first.title === "" ? props.reference : first.title}
                                </h1>
                                <div className="kb-head-line">
                                    <span className="kb-ref">{props.reference}</span>
                                    {first.locator === "" ? null : (
                                        <button
                                            type="button"
                                            className="kb-link kb-locator"
                                            title={first.locator}
                                            onClick={() => link(first.locator)}
                                        >
                                            <Codicon name="link-external" />
                                            {first.locator}
                                        </button>
                                    )}
                                </div>
                                <dl className="kb-facts">
                                    <div className="kb-fact">
                                        <dt>Collection</dt>
                                        <dd>{first.collection}</dd>
                                    </div>
                                    <div className="kb-fact">
                                        <dt>Documents</dt>
                                        <dd>{documents.length}</dd>
                                    </div>
                                    <div className="kb-fact">
                                        <dt>Bytes</dt>
                                        <dd>{bytes}</dd>
                                    </div>
                                </dl>
                            </header>
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
                                            <span className="kb-row-title">
                                                {d.title === "" ? d.id : d.title}
                                            </span>
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
                        </div>
                    );
                }}
            </Resolved>
        </div>
    );
}
