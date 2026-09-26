/* The shapes index-api.md describes, as TypeScript.
 *
 * WRITTEN OFF THE SPECIFICATION AND WIDENED TO WHAT THE CLI ACTUALLY EMITS,
 * which is not the same set and the difference is deliberate. §1.2's `Document`
 * has no `locator` or `chunkBase`; `kb get --json` carries both, because a
 * reader has to be able to say where a row came from without a second call.
 * Nothing here narrows the CLI's answer
 * — a field this file does not name is a field a caller cannot read, and a
 * reader that silently dropped provenance is the failure §3 of the UI spec is
 * written against.
 *
 * EVERY FIELD IS `readonly`. This layer hands back what the store said; a
 * caller that mutated a row would be editing a copy of the truth and then
 * acting on it.
 */

/* §1.1: one identifier per entity, public, prefixed. The prefix carries the
 * kind, so a reference is self-describing wherever it turns up. */
export type SourceId = string;
export type DocumentId = string;
export type ChunkId = string;

/* §1.2. `kind` is what was ingested from: a URL, a file, or content handed
 * in directly. There is no directory walk: kb holds reference material, not
 * the workspace's own code. */
export type SourceKind = "url" | "file" | "inline";

export interface KbSource {
    readonly id: SourceId;
    readonly kind: SourceKind | string;
    readonly locator: string;
    readonly title: string;
    readonly collection: string;
    readonly fetchedAt: string;
    readonly contentHash: string;
    readonly docCount: number;
    readonly bytes: number;
    /* The ETag the latest fetch was filed with, when its fetcher gave one. */
    readonly etag: string | null;
    /* "ok", or "fetch_failed" once reading it again failed. */
    readonly status: string;
}

export interface KbDocument {
    readonly id: DocumentId;
    readonly source: SourceId;
    readonly collection: string;
    readonly path: string;
    readonly title: string;
    readonly mime: string;
    /* The source's locator, copied onto the row so a list costs one call.
     * §3.1 of the UI spec shows it on every document and a header that had to
     * fetch the source to draw itself would be a second call per row. */
    readonly locator: string;
    readonly contentHash: string;
    readonly bytes: number;
    readonly fetchedAt: string;
    readonly indexedAt: string;
    readonly chunkCount: number;
    /* The first chunk id of the contiguous range reserved at ingest. Chunk
     * ids are not derived from position (§1.1), so this is how `C-99812`
     * survives a rebuild. */
    readonly chunkBase: number;
    /* §1.2: "free-form per-document ... filterable but not schema-bound". So
     * it is an object of unknown values and nothing here asserts otherwise. */
    readonly meta: Readonly<Record<string, unknown>>;
}

/* §1.2's retrieval unit. `heading` and `span` point back into the document, so
 * a hit can be shown in place rather than as a floating fragment (§3). */
export interface KbChunk {
    readonly id: ChunkId;
    readonly document: DocumentId;
    readonly ordinal: number;
    readonly heading: string | null;
    readonly span: { readonly start: number; readonly end: number };
    readonly tokens: number;
    /* Present when the caller asked for the text; `kb get --include chunks`
     * without `text` describes the chunks without carrying them. */
    readonly text?: string;
}

/* §4's hit, verbatim:
 *
 *   { chunk, document, source, title, heading, snippet, collection,
 *     matched, scores: { bm25, vector, fused }, fetchedAt, stale }
 *
 * `matched` NAMES WHICH RETRIEVAL PATHS PRODUCED THE HIT, and §4 says why it
 * is on the wire at all: "a result found by both is a different kind of result
 * from one found by either". §2 of the UI spec shows it on the row. */
export const RETRIEVAL_PATHS = ["keyword", "semantic"] as const;
export type RetrievalPath = (typeof RETRIEVAL_PATHS)[number];

export interface KbScores {
    readonly bm25: number;
    readonly vector: number;
    readonly fused: number;
}

export interface KbHit {
    readonly chunk: ChunkId;
    readonly document: DocumentId;
    readonly source: SourceId;
    readonly title: string;
    readonly heading: string | null;
    readonly snippet: string;
    readonly collection: string;
    /* NOT NARROWED TO THE TWO §4 NAMES. Hybrid is two paths today and the
     * field exists to say which of them fired; a third path added later would
     * be dropped by a reader that only knew these two, and a row would then
     * claim it was found by nothing. `RETRIEVAL_PATHS` is what is known, not
     * what is permitted. */
    readonly matched: readonly string[];
    readonly scores: KbScores;
    readonly fetchedAt: string;
    /* §5: computed against the staleness threshold by the store, not here. A
     * reader that recomputed it would have a second answer to a question the
     * store has already answered. */
    readonly stale: boolean;
}

/* §7's `GET /collections`. */
export interface KbCollection {
    readonly name: string;
    readonly documents: number;
    readonly bytes: number;
    /* §5's freshness, per topic: when the oldest document in this collection
     * was fetched. It is what makes a collection list answer "has this already
     * been researched, and how long ago" rather than only "does it exist". */
    readonly oldestFetchedAt?: string;
    /* PRESENT ONLY WHEN THE STORE SAID SO, AND `kb collections` DOES NOT. The
     * chunk count lives on `kb stats`, and a reader that defaulted it to zero
     * would report an empty topic as confidently as a full one. Absent is the
     * honest answer to a question this row was never asked. */
    readonly sources?: number;
    readonly chunks?: number;
}

/* §7's `GET /status`. There is one store, the first `.kb/` at or above the
 * working directory; `path: null` and `present: false` mean there is none, and
 * everything else is absent then, because "no store here" and "a store with no
 * documents in it" are different answers. */
export interface KbStatus {
    readonly path: string | null;
    readonly present: boolean;
    readonly readable?: boolean;
    readonly error?: string;
    readonly sources?: number;
    readonly documents?: number;
    readonly chunks?: number;
    readonly contentBytes?: number;
    readonly diskBytes?: number;
    readonly indexBytes?: number;
    readonly nextIds?: {
        readonly source: SourceId;
        readonly document: DocumentId;
        readonly chunk: ChunkId;
    };
    readonly chunking?: {
        readonly recorded: boolean;
        readonly chunker: string;
        readonly chunkTokens: number;
        readonly chunkOverlap: number;
        /* Whether the parameters the store was built with still match what
         * the running binary would produce. §8's question one layer down: a
         * store whose chunker differs owes a reindex exactly as one built
         * with another model would. */
        readonly current: boolean;
    };
    /* §8's model identity, or null where there is no model yet. */
    readonly model: unknown;
    readonly torn?: boolean;
    /* The staleness threshold the counts were taken against. */
    readonly olderThan: string;
}

/* What `kb add` answers. One shape for every outcome — created, updated, or
 * unchanged with `fetchedAt` moved on — so a caller never has to work out
 * which keys are present before it can read the answer. */
export interface KbAdded {
    readonly document: DocumentId;
    readonly source: SourceId;
    readonly contentHash: string;
    readonly bytes: number;
    readonly collection: string;
    readonly mime: string;
    /* Which of the chunker's paths the document took (§3), not the algorithm
     * identity that `index/model.json` records. */
    readonly splitter: string;
    readonly chunkCount: number;
    readonly chunkBase: number;
    readonly created: boolean;
    readonly reindexed: boolean;
    readonly blobWritten: boolean;
    readonly fetchedAt: string;
}

/* `kb get` with `--include text,chunks,links`. The document is always there;
 * the other three are present exactly when they were asked for, because §4's
 * discipline is that a list must not be able to flood a caller's context and
 * the same rule holds for a read that was not asked to carry the text. */
export interface KbDocumentRead {
    readonly document: KbDocument;
    readonly text?: string | null;
    readonly chunks?: readonly KbChunk[] | null;
    /* §6's edges, resolved to rows, the same shape `kb links` answers. */
    readonly links?: { readonly outgoing: readonly KbLink[]; readonly incoming: readonly KbLink[] };
}

/* `kb sources show S-n`: the source, its documents, and every time they were
 * filed — `changed` when the text was new, false when the same text was read
 * again — oldest first. */
export interface KbFetch {
    readonly document: DocumentId;
    readonly fetchedAt: string;
    readonly changed: boolean;
}

export interface KbSourceRead {
    readonly source: KbSource;
    readonly documents: readonly KbDocument[];
    readonly history: readonly KbFetch[];
}

/* `kb refresh S-n`: whether the text had changed, and so was re-indexed. */
export interface KbSourceRefreshed {
    readonly action: string;
    readonly source: SourceId;
    readonly document: DocumentId;
    readonly changed: boolean;
    readonly contentHash: string;
    readonly fetchedAt: string;
}

/* What `kb forget` answers: every id that left the fold. `note` says the text
 * stays in the store until `kb compact`. */
export interface KbForgotten {
    readonly documents: readonly DocumentId[];
    readonly sources: readonly SourceId[];
    readonly note: string;
}

/* §4's `GET /chunks/{id}`: "the full chunk text and its neighbours". */
export interface KbChunkRead {
    readonly chunk: KbChunk;
    readonly neighbours: readonly KbChunk[];
}

/* §6's small, optional layer over documents. Not a graph database: links
 * connect documents only, and entity nodes are deliberately absent until there
 * is a traversal retrieval cannot answer. */
export const LINK_TYPES = ["supersedes", "cites", "analogue_of", "implements", "see_also"] as const;

export type LinkType = (typeof LINK_TYPES)[number];

export function isLinkType(v: unknown): v is LinkType {
    return typeof v === "string" && (LINK_TYPES as readonly string[]).includes(v);
}

/* One edge, with the row at the far end of it.
 *
 * §6 says the read resolves links "to rows", and that is the whole reason this
 * is not just a pair of ids: a caller that had to fetch each neighbour to learn
 * its title would make one call per edge to draw a list. `document` is the far
 * end — the `to` of an outgoing edge, the `from` of an incoming one — and is
 * null when the store has an edge pointing at something it cannot resolve,
 * which is a dangling link worth showing rather than hiding.
 *
 * `type` IS NOT NARROWED TO `LinkType`. §6 names five today; a sixth added
 * later would be dropped by a reader that only knew the five, and an edge would
 * then claim a relationship it does not have. `LINK_TYPES` is what is known,
 * not what is permitted — the same rule `KbHit.matched` follows. */
export interface KbLink {
    readonly from: DocumentId;
    readonly to: DocumentId;
    readonly type: string;
    readonly document: KbDocument | null;
    /* The store's own word for whether the far end is a document it still
     * holds. Read rather than derived from `document === null`, because the
     * two answer different questions — "the store could not resolve it" and
     * "this answer did not carry it" — and deriving one from the other would
     * make a row that omitted the document look like a dangling edge. */
    readonly resolved: boolean;
    readonly createdAt: string;
}

export interface KbLinks {
    readonly document: DocumentId;
    readonly outgoing: readonly KbLink[];
    readonly incoming: readonly KbLink[];
}

/* What `kb links add` answers. `changed` is false when the edge was already
 * there: §2's ingest is idempotent by content hash and this is the same
 * posture one layer along, so a caller can state a relationship twice without
 * having to check first. */
export interface KbLinkWritten {
    readonly from: DocumentId;
    readonly to: DocumentId;
    readonly type: string;
    readonly changed: boolean;
    readonly at: string;
}

/* §5's `GET /stale`, whole.
 *
 * NOT JUST THE ROWS. The threshold the store used is part of the answer: "18
 * documents are stale" means nothing without "older than what", and a caller
 * that had to know which `olderThan` it sent — or worse, did not send one and
 * so cannot say — is a caller reporting a number it cannot explain. `count` is
 * the store's own, not `documents.length`, because a store that limited the
 * list would report a total the array does not have. */
export interface KbStaleList {
    readonly documents: readonly KbDocument[];
    readonly count: number;
    readonly olderThan: string;
    readonly staleBefore: string;
}

/* §5's `POST /refresh`, and the one field that matters most.
 *
 * `note` IS WHY THIS TYPE EXISTS. §12.2 resolved against putting an HTTP
 * client and TLS in the binary, so refresh REPORTS what would be refetched and
 * fetches nothing. `refetched` and `reembedded` are therefore always zero
 * today — which means a reader that invented those two keys and read nothing
 * else got the right numbers for the wrong reason and would go on getting them
 * the day the command started acting. `action`, `note` and the source rows are
 * what say it was a report, and they are read here so that a surface cannot
 * announce a refresh that did not happen.
 *
 * `action` IS NOT NARROWED TO `"report"`, for the same reason `matched` is not
 * narrowed to two paths: the day it becomes an action, a reader that only knew
 * one word would drop the one that changed. */
export interface KbRefreshSource {
    readonly id: SourceId;
    readonly kind: SourceKind | string;
    readonly locator: string;
    readonly collection: string;
    readonly staleDocuments: number;
    readonly fetchedAt: string;
    /* The route that WOULD bring this source up to date, named per kind. The
     * store answering "here is how", rather than a caller having to work it
     * out from `kind`. */
    readonly refetchBy: string;
}

export interface KbRefresh {
    readonly action: string;
    readonly refetched: number;
    readonly reembedded: number;
    readonly sources: readonly KbRefreshSource[];
    readonly count: number;
    readonly staleDocuments: number;
    readonly olderThan: string;
    readonly staleBefore: string;
    readonly note: string;
}

/* §7's `GET /stats`. `KbCollection` is the row `kb collections` prints and
 * this is the row `kb stats` prints; they overlap and neither contains the
 * other, which is why both exist. */
export interface KbCollectionStats {
    readonly name: string;
    readonly documents: number;
    readonly chunks: number;
    readonly bytes: number;
}

export interface KbStats {
    readonly collections: readonly KbCollectionStats[];
    readonly totals: {
        readonly documents: number;
        readonly chunks: number;
        readonly bytes: number;
    };
}
