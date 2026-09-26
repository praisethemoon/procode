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
 * Up to a few hundred documents every pair pushes on every other; past that a
 * distant cluster pushes as one (Barnes–Hut), and the steps can be taken a
 * slice at a time so the webview never freezes while thousands settle.
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

/* Which way the push-apart is computed. `exact` compares every pair — the
 * picture a small store has always had; `approximate` treats a far-away
 * cluster as one heavy node at its centre (Barnes–Hut), which is what lets
 * thousands of documents settle in about a second; `auto` picks by size. */
export type LayoutMethod = "auto" | "exact" | "approximate";

/* Up to this many nodes every pair is compared, so a store of a few hundred
 * linked documents draws exactly the picture it drew before there was an
 * approximation. */
export const EXACT_UP_TO = 300;

/* How far a cluster must be before it counts as one node: its width over its
 * distance. Lower is closer to exact and slower. */
const THETA = 0.8;

/* A cell of the quadtree never gets deeper than this: nodes piled on one spot
 * would otherwise be split forever. They share a leaf instead. */
const MAX_DEPTH = 24;

/* A layout in progress, advanced a few steps at a time so a caller can yield
 * between them — the webview draws each intermediate state and keeps the tab
 * responsive. The steps are the same however they are batched: the picture
 * depends on the graph and the frame, never on how fast the machine was. */
export interface LayoutRun {
    readonly done: boolean;
    /* Steps taken, out of `total`. */
    readonly taken: number;
    readonly total: number;
    advance(count: number): void;
    /* The current positions, scaled to the frame. */
    positions(): Map<string, Point>;
}

export function layout(
    graph: Graph,
    width: number,
    height: number,
    steps = 300,
    method: LayoutMethod = "auto",
): Map<string, Point> {
    const run = startLayout(graph, width, height, steps, method);
    run.advance(steps);
    return run.positions();
}

/* A force layout — linked documents pull together, all of them push apart, a
 * weak pull to the centre keeps islands on screen — from positions derived
 * from each id, cooling linearly over `steps`. */
export function startLayout(
    graph: Graph,
    width: number,
    height: number,
    steps = 300,
    method: LayoutMethod = "auto",
): LayoutRun {
    const n = graph.nodes.length;
    const ids = graph.nodes.map((node) => node.id);
    const index = new Map(ids.map((id, i) => [id, i]));
    const x = new Float64Array(n);
    const y = new Float64Array(n);
    const cx = width / 2;
    const cy = height / 2;
    ids.forEach((id, i) => {
        const a = hash01(id) * Math.PI * 2;
        const r = (0.2 + 0.3 * hash01(id + "r")) * Math.min(width, height);
        x[i] = cx + Math.cos(a) * r;
        y[i] = cy + Math.sin(a) * r;
    });
    if (n === 1) {
        x[0] = cx;
        y[0] = cy;
    }
    /* Edges as index pairs, in the graph's order; one whose end is not a node
     * has nothing to pull. */
    const ends: number[] = [];
    for (const e of graph.edges) {
        const a = index.get(e.from);
        const b = index.get(e.to);
        if (a !== undefined && b !== undefined) ends.push(a, b);
    }
    const approximate = method === "approximate" || (method === "auto" && n > EXACT_UP_TO);
    const tree = approximate ? new QuadTree(n) : null;
    const k = Math.sqrt((width * height) / Math.max(1, n)) * 0.75; // the ideal distance
    let temperature = Math.min(width, height) / 8;
    const cool = temperature / (steps + 1);
    const dispX = new Float64Array(n);
    const dispY = new Float64Array(n);
    /* Nothing to settle for fewer than two: the start is the answer. */
    const total = n < 2 ? 0 : steps;
    let taken = 0;

    const step = (): void => {
        dispX.fill(0);
        dispY.fill(0);
        if (tree !== null) tree.repel(x, y, k * k, dispX, dispY);
        else repelExactly(x, y, k * k, dispX, dispY);
        for (let e = 0; e < ends.length; e += 2) {
            const a = ends[e];
            const b = ends[e + 1];
            const dx = x[a] - x[b];
            const dy = y[a] - y[b];
            const d = Math.max(0.01, Math.hypot(dx, dy));
            const f = (d * d) / k; // links pull together
            dispX[a] -= (dx / d) * f;
            dispY[a] -= (dy / d) * f;
            dispX[b] += (dx / d) * f;
            dispY[b] += (dy / d) * f;
        }
        for (let i = 0; i < n; i++) {
            let vx = dispX[i];
            let vy = dispY[i];
            vx += (cx - x[i]) * 0.02 * k; // a weak pull to the centre
            vy += (cy - y[i]) * 0.02 * k;
            const d = Math.max(0.01, Math.hypot(vx, vy));
            const move = Math.min(d, temperature);
            x[i] = Math.min(width - 20, Math.max(20, x[i] + (vx / d) * move));
            y[i] = Math.min(height - 20, Math.max(20, y[i] + (vy / d) * move));
        }
        temperature = Math.max(0.5, temperature - cool);
    };

    return {
        get done() {
            return taken >= total;
        },
        get taken() {
            return taken;
        },
        total,
        advance(count: number) {
            for (let c = 0; c < count && taken < total; c++, taken++) step();
        },
        positions() {
            const pos = new Map<string, { x: number; y: number }>();
            ids.forEach((id, i) => pos.set(id, { x: x[i], y: y[i] }));
            return n < 2 ? pos : fit(pos, width, height);
        },
    };
}

/* Every pair, once: O(n²) per step. */
function repelExactly(x: Float64Array, y: Float64Array, kk: number, dispX: Float64Array, dispY: Float64Array): void {
    const n = x.length;
    for (let i = 0; i < n; i++) {
        for (let j = i + 1; j < n; j++) {
            let dx = x[i] - x[j];
            let dy = y[i] - y[j];
            let d = Math.hypot(dx, dy);
            if (d < 0.01) {
                dx = 0.01;
                dy = 0;
                d = 0.01;
            }
            const f = kk / d; // everything pushes apart
            dispX[i] += (dx / d) * f;
            dispY[i] += (dy / d) * f;
            dispX[j] -= (dx / d) * f;
            dispY[j] -= (dy / d) * f;
        }
    }
}

/* Barnes–Hut: the nodes sorted into a quadtree, each cell knowing how many
 * nodes it holds and their centre. A node feels a cell that is far enough
 * away (narrower than THETA times its distance) as that many nodes at that
 * centre, and opens a nearer one. O(n log n) per step. Rebuilt every step,
 * into arrays kept between steps. */
class QuadTree {
    private order: Int32Array;
    private scratch: Int32Array;
    private quadrant: Uint8Array;
    private cap = 0;
    private count = 0;
    private mass = new Float64Array(0);
    private comX = new Float64Array(0);
    private comY = new Float64Array(0);
    private size = new Float64Array(0);
    /* A leaf's nodes are `order[start, end)`; an inner cell has start = -1. */
    private start = new Int32Array(0);
    private end = new Int32Array(0);
    private child = new Int32Array(0);
    private stack = new Int32Array(64);

    constructor(n: number) {
        this.order = new Int32Array(n);
        this.scratch = new Int32Array(n);
        this.quadrant = new Uint8Array(n);
        this.grow(Math.max(16, 4 * n));
    }

    private grow(cap: number): void {
        const more = <T extends Float64Array | Int32Array>(old: T, make: (c: number) => T, per = 1): T => {
            const next = make(cap * per);
            next.set(old);
            return next;
        };
        this.mass = more(this.mass, (c) => new Float64Array(c));
        this.comX = more(this.comX, (c) => new Float64Array(c));
        this.comY = more(this.comY, (c) => new Float64Array(c));
        this.size = more(this.size, (c) => new Float64Array(c));
        this.start = more(this.start, (c) => new Int32Array(c));
        this.end = more(this.end, (c) => new Int32Array(c));
        this.child = more(this.child, (c) => new Int32Array(c), 4);
        this.cap = cap;
    }

    repel(x: Float64Array, y: Float64Array, kk: number, dispX: Float64Array, dispY: Float64Array): void {
        const n = x.length;
        let x0 = Infinity;
        let y0 = Infinity;
        let x1 = -Infinity;
        let y1 = -Infinity;
        for (let i = 0; i < n; i++) {
            this.order[i] = i;
            if (x[i] < x0) x0 = x[i];
            if (x[i] > x1) x1 = x[i];
            if (y[i] < y0) y0 = y[i];
            if (y[i] > y1) y1 = y[i];
        }
        this.count = 0;
        const side = Math.max(x1 - x0, y1 - y0, 1e-6);
        this.build(x, y, 0, n, x0, y0, side, 0);
        const theta2 = THETA * THETA;
        for (let i = 0; i < n; i++) {
            const xi = x[i];
            const yi = y[i];
            let fx = 0;
            let fy = 0;
            let top = 0;
            this.stack[top++] = 0;
            while (top > 0) {
                const c = this.stack[--top];
                const s = this.start[c];
                if (s >= 0) {
                    // A leaf: its nodes one by one, as the exact path does.
                    for (let t = s; t < this.end[c]; t++) {
                        const j = this.order[t];
                        if (j === i) continue;
                        let dx = xi - x[j];
                        let dy = yi - y[j];
                        let d = Math.hypot(dx, dy);
                        if (d < 0.01) {
                            // Piled on one spot: the lower index goes one way, the higher the other.
                            dx = i < j ? 0.01 : -0.01;
                            dy = 0;
                            d = 0.01;
                        }
                        const f = kk / (d * d);
                        fx += dx * f;
                        fy += dy * f;
                    }
                    continue;
                }
                const dx = xi - this.comX[c];
                const dy = yi - this.comY[c];
                const d2 = dx * dx + dy * dy;
                if (this.size[c] * this.size[c] < theta2 * d2) {
                    // Far enough: the whole cell as one node at its centre.
                    const f = (kk * this.mass[c]) / d2;
                    fx += dx * f;
                    fy += dy * f;
                    continue;
                }
                if (top + 4 > this.stack.length) {
                    const bigger = new Int32Array(this.stack.length * 2);
                    bigger.set(this.stack);
                    this.stack = bigger;
                }
                for (let q = 3; q >= 0; q--) {
                    const ch = this.child[c * 4 + q];
                    if (ch >= 0) this.stack[top++] = ch;
                }
            }
            dispX[i] += fx;
            dispY[i] += fy;
        }
    }

    /* The cell for `order[lo, hi)`, whose square starts at (x0, y0) and is
     * `side` wide. Nodes are partitioned into the four quadrants in place,
     * keeping their order within each, so the tree is the same every time. */
    private build(x: Float64Array, y: Float64Array, lo: number, hi: number, x0: number, y0: number, side: number, depth: number): number {
        if (this.count === this.cap) this.grow(this.cap * 2);
        const c = this.count++;
        this.size[c] = side;
        this.child.fill(-1, c * 4, c * 4 + 4);
        if (hi - lo === 1 || depth >= MAX_DEPTH) {
            let sx = 0;
            let sy = 0;
            for (let t = lo; t < hi; t++) {
                sx += x[this.order[t]];
                sy += y[this.order[t]];
            }
            this.start[c] = lo;
            this.end[c] = hi;
            this.mass[c] = hi - lo;
            this.comX[c] = sx / (hi - lo);
            this.comY[c] = sy / (hi - lo);
            return c;
        }
        this.start[c] = -1;
        const half = side / 2;
        const mx = x0 + half;
        const my = y0 + half;
        const counts = [0, 0, 0, 0];
        for (let t = lo; t < hi; t++) {
            const i = this.order[t];
            const q = (x[i] >= mx ? 1 : 0) + (y[i] >= my ? 2 : 0);
            this.quadrant[t] = q;
            counts[q]++;
        }
        const at = [lo, lo + counts[0], lo + counts[0] + counts[1], lo + counts[0] + counts[1] + counts[2]];
        const from = [...at];
        for (let t = lo; t < hi; t++) this.scratch[at[this.quadrant[t]]++] = this.order[t];
        this.order.set(this.scratch.subarray(lo, hi), lo);
        let m = 0;
        let sx = 0;
        let sy = 0;
        for (let q = 0; q < 4; q++) {
            if (counts[q] === 0) continue;
            const qx = q & 1 ? mx : x0;
            const qy = q & 2 ? my : y0;
            const ch = this.build(x, y, from[q], from[q] + counts[q], qx, qy, half, depth + 1);
            this.child[c * 4 + q] = ch;
            m += this.mass[ch];
            sx += this.comX[ch] * this.mass[ch];
            sy += this.comY[ch] * this.mass[ch];
        }
        this.mass[c] = m;
        this.comX[c] = sx / m;
        this.comY[c] = sy / m;
        return c;
    }
}

/* What a layout is a function of, as a string to cache it under: the frame,
 * the step count, the method, and the node ids and edges in the order the
 * layout reads them (order changes the floating-point sums, so it is part of
 * the input). Two 32-bit hashes over the lot, and the counts in the clear. */
export function layoutKey(graph: Graph, width: number, height: number, steps = 300, method: LayoutMethod = "auto"): string {
    let a = 2166136261;
    let b = 5381;
    const feed = (s: string): void => {
        for (let i = 0; i < s.length; i++) {
            const ch = s.charCodeAt(i);
            a = Math.imul(a ^ ch, 16777619);
            b = (Math.imul(b, 33) + ch) | 0;
        }
        // A separator no id contains, so "D-1","D-23" and "D-12","D-3" differ.
        a = Math.imul(a ^ 0x1f, 16777619);
        b = (Math.imul(b, 33) + 0x1f) | 0;
    };
    for (const node of graph.nodes) feed(node.id);
    feed("|");
    for (const e of graph.edges) {
        feed(e.from);
        feed(e.to);
    }
    const hex = (h: number) => (h >>> 0).toString(16).padStart(8, "0");
    return `layout1:${width}x${height}:${steps}:${method}:${graph.nodes.length}n${graph.edges.length}e:${hex(a)}${hex(b)}`;
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

/* Positions as the host keeps them: [id, x, y] per node, in the graph's order. */
export function storedPositions(graph: Graph, pos: ReadonlyMap<string, Point>): [string, number, number][] {
    return graph.nodes.flatMap((node) => {
        const p = pos.get(node.id);
        return p === undefined ? [] : [[node.id, p.x, p.y] as [string, number, number]];
    });
}

/* Kept positions back as a layout of `graph`, or null unless they place every
 * one of its nodes: the key is a hash, and a shelf that answered for another
 * graph must not draw nodes on top of each other at the origin. */
export function restoredPositions(graph: Graph, stored: unknown): Map<string, Point> | null {
    if (!Array.isArray(stored)) return null;
    const byId = new Map<string, Point>();
    for (const row of stored) {
        if (!Array.isArray(row) || typeof row[0] !== "string" || !Number.isFinite(row[1]) || !Number.isFinite(row[2])) return null;
        byId.set(row[0], { x: row[1], y: row[2] });
    }
    const out = new Map<string, Point>();
    for (const node of graph.nodes) {
        const p = byId.get(node.id);
        if (p === undefined) return null;
        out.set(node.id, p);
    }
    return out;
}
