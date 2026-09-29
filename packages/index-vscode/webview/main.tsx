/* The webview entry point: one bundle, two kinds of document.
 *
 * The host writes a `ViewTag` into the page before this script runs, so a view
 * never has a moment where it is mounted and does not know what it is for. An
 * `entity` document carries the reference its tab is a place for, and that
 * reference decides which view renders — off `parseTarget`, which is the same
 * function the extension host used to make the URI.
 */

import { StrictMode } from "react";
import { createRoot } from "react-dom/client";

import { parseTarget } from "../src/uri";
import { Collections } from "./Collections";
import { Graph } from "./Graph";
import { DocumentView, SourceView } from "./Document";
import { SearchPage } from "./Search";
import { Sidebar } from "./Sidebar";
import { tag } from "./rpc";

function Entity(props: { reference: string }): JSX.Element {
    const target = parseTarget(props.reference);
    if (target === null) {
        return (
            <div className="kb-notice">
                <h2>{props.reference} is not a Knowledge reference.</h2>
                <div className="kb-small">
                    References are the public identifiers and nothing else: D-241 for a document,
                    S-3 for a source, or collections.
                </div>
            </div>
        );
    }
    if (target.sort === "place") {
        return target.place === "graph" ? <Graph /> : target.place === "search" ? <SearchPage /> : <Collections />;
    }
    if (target.kind === "source") {
        return <SourceView reference={target.id} />;
    }
    if (target.kind === "document") {
        return <DocumentView reference={target.id} />;
    }
    /* A chunk resolves as a reference and is not a place (`uri.ts`): a tab
     * whose whole content was one chunk would be a passage with its provenance
     * cut off, which is the thing §3.1 exists to prevent. `editor.ts` refuses
     * to open one, so this is the case that should be unreachable — said
     * plainly rather than rendered as a blank tab. */
    return (
        <div className="kb-notice">
            <h2>{target.id} is a chunk, and a chunk is read inside its document.</h2>
            <div className="kb-small">
                Open the document it belongs to; a search result scrolls to the chunk's heading.
            </div>
        </div>
    );
}

function App(): JSX.Element {
    return tag.view === "sidebar" ? <Sidebar /> : <Entity reference={tag.reference ?? ""} />;
}

const host = document.getElementById("root");
if (host !== null) {
    createRoot(host).render(
        <StrictMode>
            <App />
        </StrictMode>,
    );
}
