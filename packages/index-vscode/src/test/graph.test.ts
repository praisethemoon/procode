import * as assert from "node:assert/strict";
import { test } from "node:test";

import type { KbDocument, KbEdge } from "kb-js/pure";

import { EXACT_UP_TO, Graph, buildGraph, layout, layoutKey, restoredPositions, startLayout, storedPositions } from "../view/graph";

const doc = (id: string, collection: string): KbDocument => ({ id, title: `title ${id}`, collection }) as unknown as KbDocument;
const edge = (from: string, type: string, to: string, resolved = true): KbEdge => ({ from, to, type, resolved });

const docs = [doc("D-1", "win32"), doc("D-2", "win32"), doc("D-3", "uring"), doc("D-4", "uring"), doc("D-5", "bsd")];
const edges = [edge("D-1", "cites", "D-2"), edge("D-2", "analogue_of", "D-3"), edge("D-4", "see_also", "D-9", false)];

test("linked documents only, forgotten ends dropped", () => {
    const g = buildGraph(docs, edges);
    assert.deepEqual(g.nodes.map((n) => n.id), ["D-1", "D-2", "D-3"]);
    assert.equal(g.edges.length, 2);
    assert.equal(g.nodes.find((n) => n.id === "D-2")!.degree, 2);
    assert.deepEqual(buildGraph(docs, edges, { unlinked: true }).nodes.map((n) => n.id), ["D-1", "D-2", "D-3", "D-4", "D-5"]);
});

test("a collection keeps its documents and what they link to outside it, marked", () => {
    const g = buildGraph(docs, edges, { collection: "uring" });
    assert.deepEqual(g.nodes.map((n) => [n.id, n.inScope]), [["D-2", false], ["D-3", true]]);
    assert.deepEqual(g.edges, [{ from: "D-2", to: "D-3", type: "analogue_of" }]);
});

test("the layout is the same every time, inside the frame, and pulls linked documents together", () => {
    const g = buildGraph([...docs, doc("D-6", "bsd")], [...edges, edge("D-5", "cites", "D-6")], { unlinked: true });
    const a = layout(g, 800, 600);
    const b = layout(g, 800, 600);
    assert.deepEqual([...a.entries()], [...b.entries()]);
    for (const p of a.values()) {
        assert.ok(p.x >= 39.9 && p.x <= 620.1 && p.y >= 39.9 && p.y <= 560.1, `${p.x},${p.y} is outside the frame's margins`);
    }
    const dist = (x: string, y: string) => Math.hypot(a.get(x)!.x - a.get(y)!.x, a.get(x)!.y - a.get(y)!.y);
    assert.ok(dist("D-1", "D-2") < dist("D-1", "D-6"), "a linked pair sits closer than an unlinked one");
    assert.equal(layout({ nodes: [], edges: [] }, 800, 600).size, 0);
});

/* A store-shaped graph of `n` documents with about `per` links each, drawn
 * from a seeded generator so every run lays out the same thing. */
function synthetic(n: number, per: number, seed: number): Graph {
    let s = seed >>> 0;
    const next = () => {
        s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
        return s / 4294967296;
    };
    const nodes = Array.from({ length: n }, (_, i) => ({ id: `D-${i + 1}`, title: `t${i + 1}`, collection: "c", inScope: true, degree: 0 }));
    const edges = [];
    for (let i = 1; i <= n; i++) {
        for (let j = 0; j < per; j++) {
            const t = 1 + Math.floor(next() * n);
            if (t !== i) edges.push({ from: `D-${i}`, to: `D-${t}`, type: "cites" });
        }
    }
    return { nodes, edges };
}

const spread = (pos: Map<string, { x: number; y: number }>) => {
    const xs = [...pos.values()].map((p) => p.x);
    const ys = [...pos.values()].map((p) => p.y);
    return Math.hypot(Math.max(...xs) - Math.min(...xs), Math.max(...ys) - Math.min(...ys));
};

test("up to the threshold every pair is compared, as it always was", () => {
    const g = synthetic(EXACT_UP_TO, 2, 3);
    assert.deepEqual([...layout(g, 960, 620).entries()], [...layout(g, 960, 620, 300, "exact").entries()]);
});

test("the approximation draws nearly the picture the exact layout draws", () => {
    const g = synthetic(150, 2, 11);
    const exact = layout(g, 960, 620, 300, "exact");
    const near = layout(g, 960, 620, 300, "approximate");
    let moved = 0;
    for (const [id, p] of exact) moved += Math.hypot(p.x - near.get(id)!.x, p.y - near.get(id)!.y);
    const relative = moved / exact.size / spread(exact);
    assert.ok(relative < 0.1, `nodes moved ${(relative * 100).toFixed(1)}% of the layout's extent on average`);
});

test("two thousand documents settle within budget, the same every time, links drawn short", () => {
    const g = synthetic(2000, 3, 42);
    const t0 = performance.now();
    const a = layout(g, 960, 620);
    const ms = performance.now() - t0;
    assert.ok(ms < 2000, `2,000 nodes took ${ms.toFixed(0)} ms`);

    // In one go or a slice at a time, the same positions to the last bit.
    const run = startLayout(g, 960, 620);
    while (!run.done) run.advance(7);
    assert.deepEqual([...run.positions().entries()], [...a.entries()]);

    for (const p of a.values()) {
        assert.ok(p.x >= 39.9 && p.x <= 780.1 && p.y >= 39.9 && p.y <= 580.1, `${p.x},${p.y} is outside the frame's margins`);
    }
    const dist = (x: string, y: string) => Math.hypot(a.get(x)!.x - a.get(y)!.x, a.get(x)!.y - a.get(y)!.y);
    const linked = g.edges.reduce((sum, e) => sum + dist(e.from, e.to), 0) / g.edges.length;
    let apart = 0;
    for (let i = 1; i <= 1000; i++) apart += dist(`D-${i}`, `D-${((i * 7919) % 2000) + 1}`);
    apart /= 1000;
    assert.ok(linked < apart * 0.75, `linked pairs average ${linked.toFixed(0)}px, unrelated ones ${apart.toFixed(0)}px`);
});

test("a layout is kept under a key that changes with the graph and nothing else", () => {
    const g = synthetic(50, 2, 5);
    const key = layoutKey(g, 960, 620);
    assert.equal(layoutKey(synthetic(50, 2, 5), 960, 620), key);
    assert.notEqual(layoutKey({ ...g, edges: g.edges.slice(1) }, 960, 620), key);
    assert.notEqual(layoutKey({ ...g, nodes: g.nodes.slice(1) }, 960, 620), key);
    assert.notEqual(layoutKey(g, 800, 620), key);
    // A retitled document is the same picture.
    assert.equal(layoutKey({ ...g, nodes: g.nodes.map((n) => ({ ...n, title: "renamed" })) }, 960, 620), key);

    const pos = layout(g, 960, 620);
    const kept = JSON.parse(JSON.stringify(storedPositions(g, pos))) as unknown;
    assert.deepEqual(restoredPositions(g, kept), pos);
    assert.equal(restoredPositions(synthetic(51, 2, 5), kept), null, "a node the shelf does not place is a miss");
    assert.equal(restoredPositions(g, null), null);
    assert.equal(restoredPositions(g, [["D-1", "x", 0]]), null);
});
