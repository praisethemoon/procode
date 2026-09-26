/* The graph of links (index-api §6), as positions to draw. Pure.
 *
 * WHICH DOCUMENTS. With no collection chosen, every document with a link;
 * with one, that collection's linked documents and whatever they link to
 * outside it (drawn as context). An edge whose end was forgotten is not
 * drawn: there is nothing at that end to put a node on.
 *
 * WHERE THEY GO. A force layout — linked documents pull together, all of them
 * push apart, a weak pull to the centre keeps islands on screen — run a fixed
 * number of steps from positions derived from each id, so the same store
 * always draws the same picture and a reader's mental map survives a reload.
 */

import type { KbDocument, KbEdge } from "kb-js/pure";

export interface GraphNode {
    readonly id: string;
    readonly title: string;
    readonly collection: string;
    /* In the chosen collection (or no collection is chosen). */
    readonly inScope: boolean;
    readonly degree: number;
}

export interface GraphEdge {
    readonly from: string;
    readonly to: string;
    readonly type: string;
}

export interface Graph {
    readonly nodes: readonly GraphNode[];
    readonly edges: readonly GraphEdge[];
}

export function buildGraph(
    documents: readonly KbDocument[],
    edges: readonly KbEdge[],
    options: { collection?: string; unlinked?: boolean } = {},
): Graph {
    const byId = new Map(documents.map((d) => [d.id, d]));
    const scope = options.collection ?? "";
    const inScope = (id: string) => scope === "" || byId.get(id)?.collection === scope;
    const live = edges.filter((e) => e.resolved && byId.has(e.from) && byId.has(e.to) && e.from !== e.to);
    const kept = live.filter((e) => inScope(e.from) || inScope(e.to));
    const degree = new Map<string, number>();
    for (const e of kept) {
        degree.set(e.from, (degree.get(e.from) ?? 0) + 1);
        degree.set(e.to, (degree.get(e.to) ?? 0) + 1);
    }
    const ids = new Set(degree.keys());
    if (options.unlinked) {
        for (const d of documents) if (inScope(d.id)) ids.add(d.id);
    }
    const nodes = [...ids]
        .map((id) => byId.get(id)!)
        .sort((a, b) => Number(a.id.slice(2)) - Number(b.id.slice(2)))
        .map((d) => ({ id: d.id, title: d.title || d.id, collection: d.collection, inScope: inScope(d.id), degree: degree.get(d.id) ?? 0 }));
    return { nodes, edges: kept.map((e) => ({ from: e.from, to: e.to, type: e.type })) };
}

/* A number in [0, 1) from a string, the same every time. */
function hash01(s: string): number {
    let h = 2166136261;
    for (let i = 0; i < s.length; i++) {
        h ^= s.charCodeAt(i);
        h = Math.imul(h, 16777619);
    }
    return (h >>> 0) / 4294967296;
}

export interface Point {
    readonly x: number;
    readonly y: number;
}

export function layout(graph: Graph, width: number, height: number, steps = 300): Map<string, Point> {
    const n = graph.nodes.length;
    const pos = new Map<string, { x: number; y: number }>();
    if (n === 0) return pos;
    const cx = width / 2;
    const cy = height / 2;
    graph.nodes.forEach((node) => {
        const a = hash01(node.id) * Math.PI * 2;
        const r = (0.2 + 0.3 * hash01(node.id + "r")) * Math.min(width, height);
        pos.set(node.id, { x: cx + Math.cos(a) * r, y: cy + Math.sin(a) * r });
    });
    if (n === 1) {
        pos.set(graph.nodes[0].id, { x: cx, y: cy });
        return pos;
    }
    const k = Math.sqrt((width * height) / n) * 0.75; // the ideal distance
    let temperature = Math.min(width, height) / 8;
    const cool = temperature / (steps + 1);
    const ids = graph.nodes.map((node) => node.id);
    for (let step = 0; step < steps; step++) {
        const disp = new Map(ids.map((id) => [id, { x: 0, y: 0 }]));
        for (let i = 0; i < n; i++) {
            const a = pos.get(ids[i])!;
            for (let j = i + 1; j < n; j++) {
                const b = pos.get(ids[j])!;
                let dx = a.x - b.x;
                let dy = a.y - b.y;
                let d = Math.hypot(dx, dy);
                if (d < 0.01) {
                    dx = 0.01;
                    dy = 0;
                    d = 0.01;
                }
                const f = (k * k) / d; // everything pushes apart
                const da = disp.get(ids[i])!;
                const db = disp.get(ids[j])!;
                da.x += (dx / d) * f;
                da.y += (dy / d) * f;
                db.x -= (dx / d) * f;
                db.y -= (dy / d) * f;
            }
        }
        for (const e of graph.edges) {
            const a = pos.get(e.from)!;
            const b = pos.get(e.to)!;
            const dx = a.x - b.x;
            const dy = a.y - b.y;
            const d = Math.max(0.01, Math.hypot(dx, dy));
            const f = (d * d) / k; // links pull together
            const da = disp.get(e.from)!;
            const db = disp.get(e.to)!;
            da.x -= (dx / d) * f;
            da.y -= (dy / d) * f;
            db.x += (dx / d) * f;
            db.y += (dy / d) * f;
        }
        for (const id of ids) {
            const p = pos.get(id)!;
            const v = disp.get(id)!;
            v.x += (cx - p.x) * 0.02 * k; // a weak pull to the centre
            v.y += (cy - p.y) * 0.02 * k;
            const d = Math.max(0.01, Math.hypot(v.x, v.y));
            const move = Math.min(d, temperature);
            p.x = Math.min(width - 20, Math.max(20, p.x + (v.x / d) * move));
            p.y = Math.min(height - 20, Math.max(20, p.y + (v.y / d) * move));
        }
        temperature = Math.max(0.5, temperature - cool);
    }
    return fit(pos, width, height);
}

/* The settled layout, scaled to fill the frame: a force layout settles
 * wherever its forces balance, often a small huddle in the middle. The right
 * margin is wider, because labels are drawn to the right of their node. */
function fit(pos: Map<string, { x: number; y: number }>, width: number, height: number): Map<string, Point> {
    const left = 40;
    const right = width - 180;
    const top = 40;
    const bottom = height - 40;
    const xs = [...pos.values()].map((p) => p.x);
    const ys = [...pos.values()].map((p) => p.y);
    const [x0, x1, y0, y1] = [Math.min(...xs), Math.max(...xs), Math.min(...ys), Math.max(...ys)];
    const scale = Math.min(x1 > x0 ? (right - left) / (x1 - x0) : 1, y1 > y0 ? (bottom - top) / (y1 - y0) : 1);
    const ox = (left + right) / 2 - ((x0 + x1) / 2) * scale;
    const oy = (top + bottom) / 2 - ((y0 + y1) / 2) * scale;
    const out = new Map<string, Point>();
    for (const [id, p] of pos) out.set(id, { x: p.x * scale + ox, y: p.y * scale + oy });
    return out;
}
