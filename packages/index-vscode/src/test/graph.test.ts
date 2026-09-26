import * as assert from "node:assert/strict";
import { test } from "node:test";

import type { KbDocument, KbEdge } from "kb-js/pure";

import { buildGraph, layout } from "../view/graph";

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
