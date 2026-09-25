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
    KbRefresh,
    KbScores,
    KbSource,
    KbStaleList,
    KbStats,
    KbStatus,
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

export function readDocument(v: unknown): KbDocument {
    const d = obj(v);
    return {
        id: str(d["id"]),
        source: str(d["source"]),
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

export function readStaleList(payload: Record<string, unknown>): KbStaleList {
    const documents = arr(payload["documents"]).map(readDocument);
    return {
        documents,
        /* The store's own count, and the array's length only when it did not
         * give one. A store that pages would report a total the array does not
         * have, and answering `documents.length` would quietly redefine the
         * field. */
        count: "count" in payload ? num(payload["count"]) : documents.length,
        olderThan: str(payload["olderThan"]),
        staleBefore: str(payload["staleBefore"]),
    };
}

export function readRefresh(payload: Record<string, unknown>): KbRefresh {
    return {
        action: str(payload["action"]),
        refetched: num(payload["refetched"]),
        reembedded: num(payload["reembedded"]),
        sources: arr(payload["sources"]).map((v) => {
            const s = obj(v);
            return {
                id: str(s["id"]),
                kind: str(s["kind"]),
                locator: str(s["locator"]),
                collection: str(s["collection"]),
                staleDocuments: num(s["staleDocuments"]),
                fetchedAt: str(s["fetchedAt"]),
                refetchBy: str(s["refetchBy"]),
            };
        }),
        count: num(payload["count"]),
        staleDocuments: num(payload["staleDocuments"]),
        olderThan: str(payload["olderThan"]),
        staleBefore: str(payload["staleBefore"]),
        /* The sentence that says it was a report. Read last and never
         * defaulted away: a surface that shows the two zeroes without this
         * tells a reader the corpus is up to date. */
        note: str(payload["note"]),
    };
}

export function readStats(payload: Record<string, unknown>): KbStats {
    const totals = obj(payload["totals"]);
    return {
        collections: arr(payload["collections"]).map((v) => {
            const c = obj(v);
            return {
                name: str(c["name"]),
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

export function readStatus(payload: Record<string, unknown>): KbStatus {
    const t = payload;
    const status: Record<string, unknown> = {
        path: typeof t["path"] === "string" ? t["path"] : null,
        present: bool(t["present"]),
        model: t["model"] ?? null,
        olderThan: str(t["olderThan"]),
    };
    /* Everything below is absent when there is no store, and absent is
     * different from zero: "no store here" and "a store with no documents in
     * it" are two different things for a reader to be told. */
    if (status["present"] === true) {
        status["readable"] = bool(t["readable"]);
        if (typeof t["error"] === "string") {
            status["error"] = t["error"];
        }
        if (t["readable"] === true) {
            status["sources"] = num(t["sources"]);
            status["documents"] = num(t["documents"]);
            status["chunks"] = num(t["chunks"]);
            status["contentBytes"] = num(t["contentBytes"]);
            status["diskBytes"] = num(t["diskBytes"]);
            status["indexBytes"] = num(t["indexBytes"]);
            const ids = obj(t["nextIds"]);
            status["nextIds"] = {
                source: str(ids["source"]),
                document: str(ids["document"]),
                chunk: str(ids["chunk"]),
            };
            const ch = obj(t["chunking"]);
            status["chunking"] = {
                recorded: bool(ch["recorded"]),
                chunker: str(ch["chunker"]),
                chunkTokens: num(ch["chunkTokens"]),
                chunkOverlap: num(ch["chunkOverlap"]),
                current: bool(ch["current"]),
            };
            status["torn"] = bool(t["torn"]);
        }
    }
    return status as unknown as KbStatus;
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
        outgoing: arr(payload["outgoing"]).map(readLink),
        incoming: arr(payload["incoming"]).map(readLink),
    };
}

export function readLinkWritten(payload: Record<string, unknown>): KbLinkWritten {
    return {
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
