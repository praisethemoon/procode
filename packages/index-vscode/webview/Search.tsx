/* The search page, `kb:/search`: the store searched with room for the results.
 *
 * ONE BOX IN THE MIDDLE UNTIL THE FIRST SEARCH, then the box at the top and the
 * results filling the page. Beside the box, the collections to search: All,
 * or the ones picked. Each result is shown as a search engine shows one:
 * its title and score, the section the match is in, and the snippet with
 * the query's words marked (index-ui.md §5; the sidebar's rows stay as §2
 * has them).
 *
 * WHAT IT SEARCHES FOR FIRST arrives the way a chunk arrives at a document:
 * the host's `reveal`, which here carries the query typed in the sidebar.
 */

import { useEffect, useState } from "react";

import { KbCollection, KbHit } from "kb-js/pure";

import { formatDate } from "../src/view/facts";
import { unembeddedNote } from "../src/view/rows";
import { markTerms, scoreLabel, searchPageQuery, sectionOf, toggleCollection } from "../src/view/search";
import { Codicon, Resolved, StaleBadge, useQuery } from "./parts";
import { onHostEvent, open } from "./rpc";

function CollectionPicker(props: { chosen: readonly string[]; onChoose: (next: string[]) => void }): JSX.Element {
    const { state } = useQuery<KbCollection[]>("collections");
    return (
        <div className="kb-search-scope" role="group" aria-label="Collections to search">
            <button
                type="button"
                className="kb-scope-chip"
                aria-pressed={props.chosen.length === 0}
                onClick={() => props.onChoose([])}
            >
                All
            </button>
            {state.status === "ok"
                ? state.value.map((c) => (
                      <button
                          key={c.name}
                          type="button"
                          className="kb-scope-chip"
                          aria-pressed={props.chosen.includes(c.name)}
                          title={`${c.documents} document${c.documents === 1 ? "" : "s"}`}
                          onClick={() => props.onChoose(toggleCollection(props.chosen, c.name))}
                      >
                          {c.name === "" ? "—" : c.name}
                      </button>
                  ))
                : null}
        </div>
    );
}

/* One result, as a search engine shows one: the document's title with its
 * score, the section the match is in, the snippet with the query's words
 * marked, and where it came from. Opening it lands on the passage. */
function Hit(props: { hit: KbHit; q: string }): JSX.Element {
    const h = props.hit;
    const section = sectionOf(h.heading);
    const score = scoreLabel(h.scores);
    const go = (): void => open(h.document, h.chunk);
    return (
        <div
            className="kb-hit"
            role="button"
            tabIndex={0}
            onClick={go}
            onKeyDown={(e) => {
                if (e.key === "Enter" || e.key === " ") {
                    e.preventDefault();
                    go();
                }
            }}
        >
            <div className="kb-hit-head">
                <span className="kb-hit-title">{h.title === "" ? h.document : h.title}</span>
                {h.stale ? <StaleBadge /> : null}
                <span className="kb-hit-score" title={score.detail}>
                    {score.text}
                </span>
            </div>
            {section === null ? null : (
                <div className="kb-hit-section" title={section}>
                    <Codicon name="list-tree" />
                    {section}
                </div>
            )}
            <div className="kb-hit-snippet">
                {markTerms(h.snippet, props.q).map((m, i) =>
                    m.hit ? <mark key={i}>{m.text}</mark> : <span key={i}>{m.text}</span>,
                )}
            </div>
            <div className="kb-row-meta">
                <span className="kb-chip">{h.collection === "" ? "—" : h.collection}</span>
                <span className="kb-ref">{h.document}</span>
                <span className="kb-when" title={h.fetchedAt}>
                    {formatDate(h.fetchedAt)}
                </span>
                {h.matched.map((m) => (
                    <span key={m} className={`kb-match kb-match-${m}`}>
                        {m}
                    </span>
                ))}
            </div>
        </div>
    );
}

function Results(props: { q: string; chosen: readonly string[] }): JSX.Element {
    const { state } = useQuery<{ hits: KbHit[]; count: number; unembedded?: number }>(
        "search",
        searchPageQuery(props.q, props.chosen),
    );
    return (
        <Resolved state={state} loading="Searching…">
            {(result) => {
                const note = unembeddedNote(result.unembedded);
                return (
                    <>
                        {note === null ? null : <div className="kb-notice kb-unembedded">{note}</div>}
                        {result.hits.length === 0 ? (
                            <div className="kb-empty">Nothing matched “{props.q}”.</div>
                        ) : (
                            <div className="kb-hits">
                                {result.hits.map((h, i) => (
                                    <Hit key={`${h.chunk}:${i}`} hit={h} q={props.q} />
                                ))}
                                <div className="kb-count">
                                    {result.hits.length} match{result.hits.length === 1 ? "" : "es"}
                                </div>
                            </div>
                        )}
                    </>
                );
            }}
        </Resolved>
    );
}

export function SearchPage(): JSX.Element {
    const [q, setQ] = useState("");
    const [asked, setAsked] = useState("");
    const [chosen, setChosen] = useState<string[]>([]);

    useEffect(
        () =>
            onHostEvent((r) => {
                if (r.kind === "reveal" && r.chunk.trim() !== "") {
                    setQ(r.chunk);
                    setAsked(r.chunk.trim());
                }
            }),
        [],
    );

    const box = (
        <form
            className="kb-search-page-box"
            role="search"
            onSubmit={(e) => {
                e.preventDefault();
                setAsked(q.trim());
            }}
        >
            <span className="kb-search">
                <Codicon name="search" />
                <input
                    className="kb-search-input"
                    type="text"
                    value={q}
                    placeholder="Search the knowledge base"
                    spellCheck={false}
                    autoFocus
                    aria-label="Search the knowledge base"
                    onChange={(e) => setQ(e.currentTarget.value)}
                    onKeyDown={(e) => {
                        if (e.key === "Escape" && q !== "") {
                            e.preventDefault();
                            setQ("");
                        }
                    }}
                />
            </span>
            <CollectionPicker chosen={chosen} onChoose={setChosen} />
        </form>
    );

    if (asked === "") {
        return (
            <div className="kb-view kb-search-page kb-search-page-front">
                <h1 className="kb-search-page-title">Search the knowledge base</h1>
                {box}
            </div>
        );
    }
    return (
        <div className="kb-view kb-search-page">
            <div className="kb-search-page-top">{box}</div>
            <div className="kb-scroll kb-search-page-results">
                <Results q={asked} chosen={chosen} />
            </div>
        </div>
    );
}
