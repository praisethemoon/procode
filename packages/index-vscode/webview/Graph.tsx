/* `kb:/graph`: the links between documents (index-api §6), drawn.
 *
 * A node is a document, coloured by its collection; an edge is a link, drawn
 * in its type's style and pointing from → to. Hovering a document lights its
 * links and neighbours; clicking opens it. A collection narrows the graph to
 * its documents and what they link to outside it, shown faded. Documents with
 * no link are left out unless asked for: a graph of isolated dots says
 * nothing a list does not.
 *
 * A LARGE GRAPH SETTLES IN SLICES. The layout is advanced a few steps at a
 * time, each slice a few milliseconds, with a frame drawn in between — so the
 * tab stays responsive and the reader watches it settle instead of a frozen
 * editor. Once settled, the positions are kept by the host under a key made
 * from the graph, and reopening the tab over the same store draws at once.
 */

import { useLayoutEffect, useMemo, useState } from "react";

import type { KbDocument, KbEdge } from "kb-js/pure";

import { Graph as Shape, Point, buildGraph, layoutKey, restoredPositions, startLayout, storedPositions } from "../src/view/graph";
import { Resolved, useQuery } from "./parts";
import { cachedLayout, open, rememberLayout } from "./rpc";

const W = 960;
const H = 620;

/* The link types §6 names, each drawn its own way; an unknown one is plain. */
const EDGE_CLASS: Readonly<Record<string, string>> = {
    supersedes: "kb-e-supersedes",
    cites: "kb-e-cites",
    analogue_of: "kb-e-analogue",
    implements: "kb-e-implements",
    see_also: "kb-e-see",
    imports: "kb-e-imports",
};

/* Collections cycle through the theme's accent colours, never fixed ones,
 * ordered so that neighbours differ in hue (primary, info and link are all
 * blues in most themes, so they are spread apart). */
const PALETTE = ["primary", "success", "warning", "danger", "info", "link"];

interface Data {
    documents: KbDocument[];
    edges: KbEdge[];
}

/* How long one slice of layout may hold the main thread. */
const SLICE_MS = 12;

/* Layouts settled in this tab, so a filter flipped back, or a store change
 * that moved no link, draws without asking the host. A handful, newest last. */
const settled = new Map<string, Map<string, Point>>();
const SETTLED_KEPT = 6;

function keep(key: string, pos: Map<string, Point>): void {
    settled.delete(key);
    settled.set(key, pos);
    while (settled.size > SETTLED_KEPT) settled.delete(settled.keys().next().value as string);
}

interface Placed {
    key: string;
    pos: Map<string, Point>;
    settling: boolean;
}

/* The graph's positions: kept ones when there are any, otherwise a layout
 * run in slices, each slice's state drawn as it comes. Null until the first
 * positions exist. */
function useLayout(graph: Shape): Placed | null {
    const key = useMemo(() => layoutKey(graph, W, H), [graph]);
    const [placed, setPlaced] = useState<Placed | null>(null);
    useLayoutEffect(() => {
        const known = settled.get(key);
        if (known !== undefined) {
            setPlaced({ key, pos: known, settling: false });
            return;
        }
        let stopped = false;
        let frame = 0;
        const run = startLayout(graph, W, H);
        const slice = (): void => {
            if (stopped) return;
            const until = performance.now() + SLICE_MS;
            do run.advance(1);
            while (!run.done && performance.now() < until);
            const pos = run.positions();
            setPlaced({ key, pos, settling: !run.done });
            if (run.done) {
                keep(key, pos);
                rememberLayout(key, storedPositions(graph, pos));
            } else {
                frame = requestAnimationFrame(slice);
            }
        };
        cachedLayout(key).then(
            (stored) => {
                const pos = restoredPositions(graph, stored);
                if (pos === null || stopped) return slice();
                keep(key, pos);
                setPlaced({ key, pos, settling: false });
            },
            () => slice(),
        );
        return () => {
            stopped = true;
            cancelAnimationFrame(frame);
        };
    }, [key, graph]);
    return placed !== null && placed.key === key ? placed : null;
}

function GraphView(props: { data: Data }): JSX.Element {
    const [collection, setCollection] = useState("");
    const [unlinked, setUnlinked] = useState(false);
    const [hover, setHover] = useState<string | null>(null);

    const collections = useMemo(() => [...new Set(props.data.documents.map((d) => d.collection))].sort(), [props.data]);
    const colour = (c: string) => PALETTE[collections.indexOf(c) % PALETTE.length];
    const graph = useMemo(() => buildGraph(props.data.documents, props.data.edges, { collection, unlinked }), [props.data, collection, unlinked]);
    const placed = useLayout(graph);

    const near = useMemo(() => {
        if (hover === null) return null;
        const s = new Set([hover]);
        for (const e of graph.edges) {
            if (e.from === hover) s.add(e.to);
            if (e.to === hover) s.add(e.from);
        }
        return s;
    }, [hover, graph]);

    return (
        <div className="kb-graph">
            <div className="kb-graph-bar">
                <label>
                    Collection{" "}
                    <select value={collection} onChange={(e) => setCollection(e.currentTarget.value)}>
                        <option value="">All</option>
                        {collections.map((c) => (
                            <option key={c} value={c}>
                                {c}
                            </option>
                        ))}
                    </select>
                </label>
                {/* A view filter, not a write: a toggle button, so the collections
                  * tab keeps the only input in this package. */}
                <button
                    type="button"
                    className={`kb-toggle${unlinked ? " kb-toggle-on" : ""}`}
                    aria-pressed={unlinked}
                    onClick={() => setUnlinked(!unlinked)}
                >
                    Show documents with no links
                </button>
                <span className="kb-muted">
                    {graph.nodes.length} documents, {graph.edges.length} links
                    {placed === null || placed.settling ? " · laying out…" : ""}
                </span>
            </div>
            {graph.nodes.length === 0 ? (
                <div className="kb-empty">
                    No links {collection ? `in ${collection}` : "yet"}. Documents are linked with <code>kb links add</code> or an
                    agent's <code>kb_links</code>: supersedes, cites, analogue_of, implements, see_also. A folder filed
                    with <code>kb add --dir</code> links its files by what they import.
                </div>
            ) : placed === null ? (
                <div className="kb-empty">Laying out {graph.nodes.length} documents…</div>
            ) : (
                <svg className="kb-graph-svg" viewBox={`0 0 ${W} ${H}`} role="img" aria-label="Links between documents">
                    <defs>
                        <marker id="kb-arrow" viewBox="0 0 10 10" refX="10" refY="5" markerWidth="6" markerHeight="6" orient="auto-start-reverse">
                            <path d="M0,0 L10,5 L0,10 z" className="kb-arrowhead" />
                        </marker>
                    </defs>
                    {graph.edges.map((e, i) => {
                        const a = placed.pos.get(e.from)!;
                        const b = placed.pos.get(e.to)!;
                        // Stop short of the target's circle so the arrow shows.
                        const d = Math.max(1, Math.hypot(b.x - a.x, b.y - a.y));
                        const r = 9;
                        const lit = near === null || (near.has(e.from) && near.has(e.to) && (e.from === hover || e.to === hover));
                        return (
                            <line
                                key={i}
                                x1={a.x}
                                y1={a.y}
                                x2={b.x - ((b.x - a.x) / d) * r}
                                y2={b.y - ((b.y - a.y) / d) * r}
                                className={`kb-edge ${EDGE_CLASS[e.type] ?? ""}${lit ? "" : " kb-faint"}`}
                                markerEnd="url(#kb-arrow)"
                            >
                                <title>
                                    {e.from} {e.type} {e.to}
                                </title>
                            </line>
                        );
                    })}
                    {graph.nodes.map((n) => {
                        const p = placed.pos.get(n.id)!;
                        const lit = near === null || near.has(n.id);
                        return (
                            <g
                                key={n.id}
                                className={`kb-node kb-c-${colour(n.collection)}${n.inScope ? "" : " kb-outside"}${lit ? "" : " kb-faint"}`}
                                transform={`translate(${p.x},${p.y})`}
                                role="button"
                                tabIndex={0}
                                onMouseEnter={() => setHover(n.id)}
                                onMouseLeave={() => setHover(null)}
                                onFocus={() => setHover(n.id)}
                                onBlur={() => setHover(null)}
                                onClick={() => open(n.id)}
                                onKeyDown={(e) => {
                                    if (e.key === "Enter") open(n.id);
                                }}
                            >
                                <title>
                                    {n.id} · {n.title} · {n.collection}
                                </title>
                                <circle r={Math.min(12, 6 + n.degree)} />
                                <text x={14} y={4}>
                                    {n.title.length > 32 ? `${n.title.slice(0, 31)}…` : n.title}
                                </text>
                            </g>
                        );
                    })}
                </svg>
            )}
            <div className="kb-graph-legend">
                {collections.map((c) => (
                    <span key={c} className={`kb-legend-item kb-c-${colour(c)}`}>
                        <svg width="10" height="10" aria-hidden="true">
                            <circle cx="5" cy="5" r="5" />
                        </svg>
                        {c}
                    </span>
                ))}
                <span className="kb-legend-sep" />
                {Object.entries(EDGE_CLASS).map(([type, cls]) => (
                    <span key={type} className="kb-legend-item">
                        <svg width="26" height="10" aria-hidden="true">
                            <line x1="0" y1="5" x2="26" y2="5" className={`kb-edge ${cls}`} />
                        </svg>
                        {type}
                    </span>
                ))}
            </div>
        </div>
    );
}

export function Graph(): JSX.Element {
    const { state } = useQuery<Data>("graph");
    return (
        <div className="kb-view">
            <Resolved state={state} loading="Reading the links…">
                {(data) => <GraphView data={data} />}
            </Resolved>
        </div>
    );
}
