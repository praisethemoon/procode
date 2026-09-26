/* The answers the store gives, shaped by the specification.
 *
 * THE HIT IS index-api.md §4 COPIED FIELD FOR FIELD, and it is written here as
 * a literal rather than built from a helper so that comparing this block
 * against the document is something a person can do in one pass:
 *
 *   { chunk: "C-99812", document: "D-241", source: "S-3",
 *     title, heading, snippet, collection,
 *     matched: ["keyword", "semantic"],
 *     scores: { bm25, vector, fused },
 *     fetchedAt, stale }
 *
 * `kb search` IS BEING BUILT IN PARALLEL WITH THIS PACKAGE, so these fixtures
 * are the contract until it lands: they are what §4 says a hit is, and the
 * tests over them say what this reader does with one. When the command exists,
 * `cli.test.ts` drives the real binary and these stay as the statement of the
 * shape.
 *
 * The document rows are the other half and are NOT from §1.2 alone: they are
 * what `kb ls --json` actually prints, which carries `locator` and
 * `chunkBase` on top of §1.2's fields. `types.ts` records why the wider shape
 * is the right one to read.
 */

export const HIT = {
    chunk: "C-99812",
    document: "D-241",
    source: "S-3",
    title: "I/O Completion Ports",
    heading: "Creating a completion port",
    snippet: "CreateIoCompletionPort associates an open file handle with a port.",
    collection: "win32-iocp",
    matched: ["keyword", "semantic"],
    scores: { bm25: 11.25, vector: 0.82, fused: 0.031 },
    fetchedAt: "2026-06-01T09:15:00Z",
    stale: false,
};

/* A second hit, found by semantics alone, and old enough for §5's badge.
 * Two hits with different `matched`, different collections and different
 * verdicts, because a fixture where every row is the same row proves nothing
 * about a reader that renders them. */
export const HIT_OLD = {
    chunk: "C-4101",
    document: "D-88",
    source: "S-1",
    title: "io_uring and you",
    heading: null,
    snippet: "The submission queue is mapped once and written by the application.",
    collection: "io-uring",
    matched: ["semantic"],
    scores: { bm25: 0, vector: 0.74, fused: 0.016 },
    fetchedAt: "2024-01-02T00:00:00Z",
    stale: true,
};

export const DOCUMENT = {
    id: "D-241",
    source: "S-3",
    collection: "win32-iocp",
    path: "",
    title: "I/O Completion Ports",
    mime: "text/markdown",
    locator: "https://learn.microsoft.test/win32/iocp",
    contentHash: "a".repeat(64),
    bytes: 4096,
    fetchedAt: "2026-06-01T09:15:00Z",
    indexedAt: "2026-06-01T09:15:01Z",
    chunkCount: 3,
    chunkBase: 99810,
    meta: { authors: ["MSDN"], year: 2026, section: ["Win32", "IOCP"] },
};

export const DOCUMENT_OLD = {
    ...DOCUMENT,
    id: "D-88",
    source: "S-1",
    collection: "io-uring",
    title: "io_uring and you",
    locator: "https://kernel.test/io_uring.pdf",
    contentHash: "b".repeat(64),
    fetchedAt: "2024-01-02T00:00:00Z",
    indexedAt: "2024-01-02T00:00:00Z",
    meta: {},
};

export const CHUNK = {
    id: "C-99812",
    document: "D-241",
    ordinal: 2,
    heading: "Creating a completion port",
    span: { start: 1024, end: 2048 },
    tokens: 256,
};

export const COLLECTIONS = [
    { name: "win32-iocp", sources: 2, documents: 7, chunks: 41, bytes: 90210 },
    { name: "io-uring", sources: 1, documents: 3, chunks: 19, bytes: 40000 },
];

/* §7's status, flat: one store, the one `.kb/` found by walking up. */
export const STATUS = {
    path: "/work/project/.kb",
    present: true,
    readable: true,
    sources: 2,
    documents: 7,
    chunks: 41,
    contentBytes: 90210,
    diskBytes: 120000,
    indexBytes: 8000,
    nextIds: { source: "S-3", document: "D-8", chunk: "C-42" },
    chunking: {
        recorded: true,
        chunker: "structural-1",
        chunkTokens: 400,
        chunkOverlap: 60,
        current: true,
    },
    model: { recorded: null, available: null, missing: "no embedding model in /home/ana/.kb/models", current: null },
    torn: false,
    olderThan: "90d",
};

/* And the answer where there is none: `path: null`, `present: false`, and
 * nothing else but the threshold. */
export const STATUS_NONE = { path: null, present: false, olderThan: "90d" };
