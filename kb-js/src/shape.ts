/* Turning the CLI's JSON into the types in `types.ts`.
 *
 * WHY THIS IS NOT A CAST. `JSON.parse` answers `any`, and a cast would make
 * every field a promise this package cannot keep: a row that arrived without a
 * `title` would be typed `string` and be `undefined` at the moment a UI
 * measured its length. These readers make the claim true instead — one field
 * at a time, with an answer for the absent case.
 *
 * THE ABSENT CASE IS A DEFAULT, NEVER A DROPPED ROW. A document whose title
 * the store did not carry is still a document, and a reader that filtered it
 * out would hide the one row somebody needs to go and fix. The empty string is
 * visibly empty; a missing row is invisible.
 *
 * THE ONE PLACE THAT IS NOT MERELY A DEFAULT IS THE TIER. §1.4 makes a
 * document's tier part of its provenance, so an unrecognised one reads as
 * `global`: attributing an unnamed tier to the project would claim a
 * provenance nobody stated, and the two are not symmetric — `project` means
 * "this belongs to this codebase", which is a claim, and `global` is the
 * absence of it.
 *
 * NOTHING HERE VALIDATES SEMANTICS. `fetchedAt` is whatever string the store
 * wrote, `meta` is free-form per §1.2, and a code this package has not heard
 * of stays the word the store used. A binding that corrected the store would
 * be a second opinion about what is in it.
 */

import {
    KbChunk,
    KbChunkRead,
    KbCollection,
    KbDocument,
    KbDocumentRead,
    KbHit,
    KbLink,
    KbLinkWritten,
    KbLinks,
    KbScores,
    KbSource,
    KbStats,
    KbStatus,
    KbTierStatus,
    Store,
} from "./types";

export function str(v: unknown, fallback = ""): string {
    return typeof v === "string" ? v : fallback;
}

export function num(v: unknown, fallback = 0): number {
    return typeof v === "number" && Number.isFinite(v) ? v : fallback;
}

export function bool(v: unknown, fallback = false): boolean {
    return typeof v === "boolean" ? v : fallback;
}

export function obj(v: unknown): Record<string, unknown> {
    return typeof v === "object" && v !== null && !Array.isArray(v)
        ? (v as Record<string, unknown>)
        : {};
}

export function arr(v: unknown): unknown[] {
    return Array.isArray(v) ? v : [];
}

/* A string that is present and non-empty, or null. `heading` is genuinely
 * optional on a chunk (§1.2 writes it `heading?`), and a chunk in the middle of
 * a plain-text document has none — so the absence has to be expressible rather
 * than flattened to "". */
export function strOrNull(v: unknown): string | null {
    return typeof v === "string" && v.length > 0 ? v : null;
}

export function readStore(v: unknown): Store {
    return v === "project" ? "project" : "global";
}

export function readDocument(v: unknown): KbDocument {
    const d = obj(v);
    return {
        id: str(d["id"]),
        source: str(d["source"]),
        store: readStore(d["store"]),
        collection: str(d["collection"]),
        path: str(d["path"]),
        title: str(d["title"]),
        mime: str(d["mime"]),
        locator: str(d["locator"]),
        contentHash: str(d["contentHash"]),
        bytes: num(d["bytes"]),
        fetchedAt: str(d["fetchedAt"]),
        indexedAt: str(d["indexedAt"]),
        chunkCount: num(d["chunkCount"]),
        chunkBase: num(d["chunkBase"]),
        meta: obj(d["meta"]),
    };
}

export function readSource(v: unknown): KbSource {
    const s = obj(v);
    return {
        id: str(s["id"]),
        store: readStore(s["store"]),
        kind: str(s["kind"]),
        locator: str(s["locator"]),
        title: str(s["title"]),
        collection: str(s["collection"]),
        fetchedAt: str(s["fetchedAt"]),
        contentHash: str(s["contentHash"]),
        docCount: num(s["docCount"]),
        bytes: num(s["bytes"]),
        status: str(s["status"]),
    };
}

export function readChunk(v: unknown): KbChunk {
    const c = obj(v);
    const span = obj(c["span"]);
    const chunk: KbChunk = {
        id: str(c["id"]),
        document: str(c["document"]),
        ordinal: num(c["ordinal"]),
        heading: strOrNull(c["heading"]),
        span: { start: num(span["start"]), end: num(span["end"]) },
        tokens: num(c["tokens"]),
    };
    /* `text` is present exactly when it was asked for, and the difference
     * matters: an empty string is a chunk with no text in it and an absent key
     * is a chunk whose text was not fetched. */
    return typeof c["text"] === "string" ? { ...chunk, text: c["text"] } : chunk;
}

function readScores(v: unknown): KbScores {
    const s = obj(v);
    return { bm25: num(s["bm25"]), vector: num(s["vector"]), fused: num(s["fused"]) };
}

export function readHit(v: unknown): KbHit {
    const h = obj(v);
    return {
        chunk: str(h["chunk"]),
        document: str(h["document"]),
        source: str(h["source"]),
        title: str(h["title"]),
        heading: strOrNull(h["heading"]),
        snippet: str(h["snippet"]),
        collection: str(h["collection"]),
        store: readStore(h["store"]),
        alsoGlobal: bool(h["alsoGlobal"]),
        /* Every string the store named, in the order it named them. Not
         * filtered against the two §4 knows about: see `KbHit.matched`. */
        matched: arr(h["matched"]).filter((m): m is string => typeof m === "string"),
        scores: readScores(h["scores"]),
        fetchedAt: str(h["fetchedAt"]),
        stale: bool(h["stale"]),
    };
}

export function readCollection(v: unknown): KbCollection {
    const c = obj(v);
    const row: Record<string, unknown> = {
        name: str(c["name"]),
        store: readStore(c["store"]),
        documents: num(c["documents"]),
        bytes: num(c["bytes"]),
    };
    /* Each of these exactly when the store carried it. A zero here would be a
     * claim about a count nobody asked this command for. */
    if (typeof c["oldestFetchedAt"] === "string") {
        row["oldestFetchedAt"] = c["oldestFetchedAt"];
    }
    for (const key of ["sources", "chunks"]) {
        if (typeof c[key] === "number") {
            row[key] = c[key];
        }
    }
    return row as unknown as KbCollection;
}

export function readStats(payload: Record<string, unknown>): KbStats {
    const totals = obj(payload["totals"]);
    return {
        collections: arr(payload["collections"]).map((v) => {
            const c = obj(v);
            return {
                name: str(c["name"]),
                store: readStore(c["store"]),
                documents: num(c["documents"]),
                chunks: num(c["chunks"]),
                bytes: num(c["bytes"]),
            };
        }),
        totals: {
            documents: num(totals["documents"]),
            chunks: num(totals["chunks"]),
            bytes: num(totals["bytes"]),
        },
    };
}

function readTier(v: unknown): KbTierStatus {
    const t = obj(v);
    const tier: Record<string, unknown> = {
        store: readStore(t["store"]),
        path: typeof t["path"] === "string" ? t["path"] : null,
        present: bool(t["present"]),
        model: t["model"] ?? null,
    };
    /* Everything below is absent on a tier that is not there, and absent is
     * different from zero: "no store here" and "a store with no documents in
     * it" are two different things for a reader to be told. */
    if (tier["present"] === true) {
        tier["readable"] = bool(t["readable"]);
        if (typeof t["error"] === "string") {
            tier["error"] = t["error"];
        }
        if (t["readable"] === true) {
            tier["sources"] = num(t["sources"]);
            tier["documents"] = num(t["documents"]);
            tier["chunks"] = num(t["chunks"]);
            tier["contentBytes"] = num(t["contentBytes"]);
            tier["diskBytes"] = num(t["diskBytes"]);
            tier["indexBytes"] = num(t["indexBytes"]);
            const ids = obj(t["nextIds"]);
            tier["nextIds"] = {
                source: str(ids["source"]),
                document: str(ids["document"]),
                chunk: str(ids["chunk"]),
            };
            const ch = obj(t["chunking"]);
            tier["chunking"] = {
                recorded: bool(ch["recorded"]),
                chunker: str(ch["chunker"]),
                chunkTokens: num(ch["chunkTokens"]),
                chunkOverlap: num(ch["chunkOverlap"]),
                current: bool(ch["current"]),
            };
            tier["torn"] = bool(t["torn"]);
        }
    }
    return tier as unknown as KbTierStatus;
}

export function readStatus(payload: Record<string, unknown>): KbStatus {
    const write = payload["defaultWrite"];
    return {
        tiers: arr(payload["tiers"]).map(readTier),
        defaultWrite: write === "project" || write === "global" ? write : "none",
    };
}

export function readDocumentRead(payload: Record<string, unknown>): KbDocumentRead {
    const read: Record<string, unknown> = { document: readDocument(payload["document"]) };
    if ("text" in payload) {
        read["text"] = typeof payload["text"] === "string" ? payload["text"] : null;
    }
    if ("chunks" in payload) {
        read["chunks"] = payload["chunks"] === null ? null : arr(payload["chunks"]).map(readChunk);
    }
    return read as unknown as KbDocumentRead;
}

/* §6's edge. `document` is the resolved row at the far end and stays null when
 * the store did not carry one — a dangling edge is a fact about the store, and
 * a reader that invented an empty row for it would be reporting a document
 * that is not there. */
export function readLink(v: unknown): KbLink {
    const l = obj(v);
    return {
        from: str(l["from"]),
        to: str(l["to"]),
        type: str(l["type"]),
        document: readLinkTarget(l["document"]),
        resolved: bool(l["resolved"]),
        createdAt: str(l["createdAt"]),
    };
}

function readLinkTarget(v: unknown): KbDocument | null {
    return typeof v === "object" && v !== null && !Array.isArray(v) ? readDocument(v) : null;
}

export function readLinks(payload: Record<string, unknown>): KbLinks {
    return {
        document: str(payload["document"]),
        store: readStore(payload["store"]),
        outgoing: arr(payload["outgoing"]).map(readLink),
        incoming: arr(payload["incoming"]).map(readLink),
    };
}

export function readLinkWritten(payload: Record<string, unknown>): KbLinkWritten {
    return {
        store: readStore(payload["store"]),
        from: str(payload["from"]),
        to: str(payload["to"]),
        type: str(payload["type"]),
        changed: bool(payload["changed"]),
        at: str(payload["at"]),
    };
}

export function readChunkRead(payload: Record<string, unknown>): KbChunkRead {
    return {
        chunk: readChunk(payload["chunk"]),
        neighbours: arr(payload["neighbours"]).map(readChunk),
    };
}
