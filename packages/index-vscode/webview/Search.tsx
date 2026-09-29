/* The search page, `kb:/search`: the store searched with room for the results.
 *
 * ONE BOX IN THE MIDDLE UNTIL THE FIRST SEARCH, then the box at the top and the
 * results filling the page. Beside the box, the collections to search: All,
 * or the ones picked. The rows are the sidebar's (`KnowledgeRow`), so a
 * result reads the same wherever it was found, unmarked as §2 has every row.
 *
 * WHAT IT SEARCHES FOR FIRST arrives the way a chunk arrives at a document:
 * the host's `reveal`, which here carries the query typed in the sidebar.
 */

import { useEffect, useState } from "react";

import { KbCollection, KbHit } from "kb-js/pure";

import { searchRows, unembeddedNote } from "../src/view/rows";
import { searchPageQuery, toggleCollection } from "../src/view/search";
import { Codicon, Resolved, useQuery } from "./parts";
import { onHostEvent, tag } from "./rpc";
import { List } from "./Sidebar";

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

function Results(props: { q: string; chosen: readonly string[] }): JSX.Element {
    const { state } = useQuery<{ hits: KbHit[]; count: number; unembedded?: number }>(
        "search",
        searchPageQuery(props.q, props.chosen),
    );
    return (
        <Resolved state={state} loading="Searching…">
            {(result) => {
                const rows = searchRows(result.hits, Date.now(), tag.staleAfterDays);
                const note = unembeddedNote(result.unembedded);
                return (
                    <>
                        {note === null ? null : <div className="kb-notice kb-unembedded">{note}</div>}
                        {rows.length === 0 ? (
                            <div className="kb-empty">Nothing matched “{props.q}”.</div>
                        ) : (
                            <List rows={rows} />
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
