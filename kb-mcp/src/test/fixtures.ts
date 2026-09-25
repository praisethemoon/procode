/* What the store answers, shaped by index-api.md.
 *
 * THE HIT IS §4 COPIED FIELD FOR FIELD, and the snippet in it is deliberately
 * SHORT while `DOCUMENT_TEXT` below is deliberately long and contains a phrase
 * that appears nowhere else. That is how the snippet discipline is stated as a
 * fact rather than as an intention: a search result that carried the whole
 * document would contain `THE-WHOLE-DOCUMENT`, and the test looks for it.
 */

export const SNIPPET = "CreateIoCompletionPort associates an open file handle with a port.";

/* Long, and containing a phrase that can only have come from the full text.
 * A search result must never contain it; a kb_get of the document must. */
export const DOCUMENT_TEXT = `# I/O Completion Ports

THE-WHOLE-DOCUMENT

${"CreateIoCompletionPort binds a handle to a completion port. ".repeat(40)}
`;

export const HIT = {
    chunk: "C-99812",
    document: "D-241",
    source: "S-3",
    title: "I/O Completion Ports",
    heading: "Creating a completion port",
    snippet: SNIPPET,
    collection: "win32-iocp",
    store: "project",
    alsoGlobal: false,
    matched: ["keyword", "semantic"],
    scores: { bm25: 11.25, vector: 0.82, fused: 0.031 },
    fetchedAt: "2026-06-01T09:15:00Z",
    stale: false,
};

/* The second tier, found by semantics alone, and old enough for §5's badge.
 * Two hits that differ in tier, in `matched` and in staleness, because a
 * fixture where every row is the same row proves nothing about the reader. */
export const HIT_GLOBAL = {
    chunk: "C-4101",
    document: "D-88",
    source: "S-1",
    title: "io_uring and you",
    heading: null,
    snippet: "The submission queue is mapped once and written by the application.",
    collection: "io-uring",
    store: "global",
    alsoGlobal: false,
    matched: ["semantic"],
    scores: { bm25: 0, vector: 0.74, fused: 0.016 },
    fetchedAt: "2024-01-02T00:00:00Z",
    stale: true,
};

export const DOCUMENT = {
    id: "D-241",
    source: "S-3",
    store: "project",
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
    store: "global",
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
    text: DOCUMENT_TEXT,
};

export const COLLECTIONS = [
    { name: "win32-iocp", store: "project", sources: 2, documents: 7, chunks: 41, bytes: 90210 },
    { name: "io-uring", store: "global", sources: 1, documents: 3, chunks: 19, bytes: 40000 },
];

export const ADDED = {
    store: "project",
    document: "D-242",
    source: "S-9",
    contentHash: "c".repeat(64),
    bytes: 57,
    collection: "win32-iocp",
    mime: "text/markdown",
    splitter: "markdown",
    chunkCount: 1,
    chunkBase: 99820,
    created: true,
    reindexed: true,
    blobWritten: true,
    fetchedAt: "2026-09-25T00:00:00Z",
};

export const LINKS = {
    outgoing: [{ from: "D-241", to: "D-88", type: "analogue_of", document: DOCUMENT_OLD }],
    incoming: [{ from: "D-88", to: "D-241", type: "cites", document: DOCUMENT_OLD }],
};
