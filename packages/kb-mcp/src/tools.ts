/* index-api.md §9's tool surface: six, and the reasons there is no seventh.
 *
 * §9 IS A LIST OF WHAT IS EXPOSED AND A LIST OF WHAT IS NOT, AND THE SECOND
 * LIST IS THE LOAD-BEARING ONE. `rebuild`, `reindex`, `compact`, `promote`,
 * `demote` and every `DELETE` are absent on purpose: an agent files knowledge
 * into the workspace store and reads from it, while forgetting — and deciding
 * what the store is for and where it lives — are the reader's decisions. A
 * tool that let an agent drop a collection would be a defect against this
 * section rather than a feature, and it would be one that nobody notices until
 * the day it runs. `NOT_EXPOSED` names them so a test can refuse
 * them by name, and `guards.test.ts` fails if a seventh tool of any kind
 * appears.
 *
 * THERE IS NO TOOL THAT ANSWERS A QUESTION. §4: the store returns passages, it
 * does not summarise, synthesise or answer — that is the caller's work, and
 * doing it here would bury the provenance that makes the store worth having.
 * Every result below is the store's own rows.
 *
 * THE SCHEMAS ARE ENFORCED AND NOT DECORATIVE. `call.ts` checks each argument
 * object against the same rules the schema publishes, unknown keys included. A
 * schema that is only advertised is a schema that drifts, and the way it
 * drifts is a filter quietly going nowhere: `collections` for `collection`
 * costs nothing at the call and silently searches the whole store.
 */

/* THERE IS ONE STORE AND NO TOOL NAMES IT. §1.4's store is the first `.kb/`
 * at or above the server's working directory, found by the CLI walking up, and
 * that is the only store any tool below reads or writes. There was once a
 * second, global tier and a `store` selector on every read; both are gone, and
 * `store` is now what any other unknown key is — refused by `call.ts`, so a
 * caller still sending it learns it is talking to a different server than the
 * one it was written against instead of having the key silently ignored. */

export interface ToolDefinition {
    readonly name: string;
    readonly description: string;
    readonly inputSchema: Record<string, unknown>;
}

/* §4's search, with every filter the route takes. */
const KB_SEARCH: ToolDefinition = {
    name: "kb_search",
    description:
        "Search the local knowledge base for passages. Returns ranked SNIPPETS only, never whole documents: " +
        "each hit carries its chunk and document ids, title, heading, collection, " +
        "how old it is, and which retrieval paths found it. Read a whole passage with kb_get. " +
        "Retrieval is hybrid by default — keyword and semantic fused — because this corpus is dense with exact " +
        "identifiers that embeddings place on top of their opposites. " +
        "When the answer carries unembedded: N, that many chunks (from a large document filed recently) have " +
        "no embedding yet, so the semantic side did not see them; keyword matching still finds them, so for " +
        "an exact name search with mode keyword.",
    inputSchema: {
        type: "object",
        properties: {
            q: { type: "string", description: "The query." },
            collection: {
                type: "array",
                items: { type: "string" },
                description:
                    "Narrow to these collections. A collection is a flat topic scope such as win32-iocp or papers.",
            },
            mode: {
                type: "string",
                enum: ["hybrid", "semantic", "keyword"],
                description:
                    "Retrieval path. Default: hybrid. Use keyword for an exact identifier, semantic for something you cannot name.",
            },
            k: {
                type: "integer",
                minimum: 1,
                maximum: 100,
                description: "How many hits to return. Default is the store's; maximum 100.",
            },
            expand: {
                type: "integer",
                minimum: 0,
                description: "Also return this many neighbouring chunks around each hit.",
            },
            source: { type: "string", description: "Only hits from this source, e.g. S-3." },
            mime: { type: "string", description: "Only hits from documents of this media type." },
            since: {
                type: "string",
                description: "Only documents fetched at or after this ISO-8601 instant.",
            },
            minScore: { type: "number", description: "Drop hits fused below this score." },
            rerank: {
                type: "boolean",
                description:
                    "Rescore the top 10 hits with a cross-encoder and reorder them by its score, which each rescored " +
                    "hit carries as scores.rerank. Better ordering, but several seconds slower per search, and it " +
                    "needs the reranker model file in ~/.kb/models (refused as model_missing without it). Default: false.",
            },
        },
        required: ["q"],
        additionalProperties: false,
    },
};

/* §4's `GET /chunks/{id}` and §2's `GET /documents/{id}`, told apart by the
 * prefix on the id.
 *
 * WHICH IS WHAT §1.1 PUT THE PREFIX THERE FOR: "the prefix carries the kind,
 * so a reference is self-describing in a search result, a citation, a log line
 * or a prompt". A `kind` argument beside the id would be a second statement of
 * the same fact, and the two would eventually disagree.
 *
 * ONE ID PER CALL, AND THAT IS THE SNIPPET DISCIPLINE. §4: a list must not be
 * able to flood a caller's context. Search is the list and it returns
 * snippets; this is how a caller asks for a whole thing, deliberately, one at
 * a time. An `ids` array here would be a list of whole documents, which is the
 * exact shape the discipline exists to prevent. */
const KB_GET: ToolDefinition = {
    name: "kb_get",
    description:
        "Read one item from the knowledge base in full. A chunk id (C-n) returns that passage whole with its " +
        "neighbours; a document id (D-n) returns the document with its text. One id per call: this is the " +
        "deliberate way to pull a whole passage into context after kb_search has shown you the snippet.",
    inputSchema: {
        type: "object",
        properties: {
            id: {
                type: "string",
                description: "A chunk id such as C-99812, or a document id such as D-241.",
            },
            include: {
                type: "array",
                items: { type: "string", enum: ["text", "chunks", "links"] },
                description:
                    "For a document: text returns the whole content (the default), chunks lists how it was split, links gives its relationships. Pass [] for the metadata alone. Ignored for a chunk id.",
            },
            expand: {
                type: "integer",
                minimum: 0,
                description: "For a chunk id: how many neighbouring chunks to return either side.",
            },
        },
        required: ["id"],
        additionalProperties: false,
    },
};

/* §2's `POST /documents` and `POST /documents/batch` — the load-bearing route —
 * and §2.1's folder, `POST /sources` with kind `dir`.
 *
 * A FOLDER IS FILED WITHOUT FORGETTING, ALWAYS. §2.1's walk forgets a file that
 * is gone from the folder unless told not to, and forgetting is not an agent's
 * decision (§9) — so `call.ts` sends `--no-forget` on every folder filing and
 * there is no argument that turns it off. A file that is gone comes back named
 * under `missing`, which is a fact the agent can pass on to the reader, who is
 * the one who can decide to forget it.
 *
 * IT FILES INTO THE STORE THE SERVER'S WORKING DIRECTORY FINDS, AND NOWHERE
 * ELSE. When there is no `.kb/` at or above that directory the CLI refuses with
 * `not_found` and the refusal reaches the agent as a tool error naming
 * `kb init`. It does not create one: where a store lives, and whether a
 * workspace has one at all, is the reader's decision (§9 withholds `init` for
 * exactly that reason), and a store conjured by the first filing would land in
 * whatever directory the server happened to be started from. */
const KB_ADD: ToolDefinition = {
    name: "kb_add",
    description:
        "File content you have already read into the knowledge base, so the next question on the topic is " +
        "answered from disk instead of fetched again. Hand over the text you have; nothing is re-fetched. " +
        "Filing is idempotent by content hash. All the documents of one call are filed together or not at " +
        "all: if the store refuses one, none is filed. Documents go to this workspace's store; if the workspace has " +
        "none this fails, and creating one is the user's decision, not yours. " +
        "To file a whole folder of source or documentation instead, pass dir and collection and no documents: " +
        "each file the folder holds as git would track it becomes a document at its path, and filing the same " +
        "folder again re-indexes only what changed. Files gone from the folder since it was last filed are " +
        "reported as missing, not forgotten; forgetting them is the user's call. " +
        "A filing embeds for a limited time and answers pending: N for the chunks it did not reach. A large " +
        "document is searchable by keyword at once; its embeddings finish later (on the next filing, or when " +
        "the user runs Finish embedding in the Knowledge view), and until then semantic search may miss it.",
    inputSchema: {
        type: "object",
        properties: {
            documents: {
                type: "array",
                minItems: 1,
                items: {
                    type: "object",
                    properties: {
                        title: { type: "string", description: "What this document is called." },
                        content: { type: "string", description: "The text itself." },
                        collection: {
                            type: "string",
                            description:
                                "The topic scope to file it under, such as win32-iocp. Created on first use. Flat: collections do not nest.",
                        },
                        url: {
                            type: "string",
                            description:
                                "Where the text came from. Recorded as provenance and used to guess the media type.",
                        },
                        mime: {
                            type: "string",
                            description:
                                "The media type, e.g. text/markdown. Pass it when you know it: it decides how the document is split and how it renders.",
                        },
                        meta: {
                            type: "object",
                            description:
                                "Free-form per-document facts: a paper's authors and year, a page's section path, a file's language.",
                        },
                    },
                    required: ["title", "content", "collection"],
                    additionalProperties: false,
                },
                description: "The documents to file. Give either documents or dir, not both.",
            },
            dir: {
                type: "string",
                description:
                    "A folder to file whole, absolute or relative to the server's working directory. Needs collection; " +
                    "each document's title, type and content come from its file.",
            },
            collection: {
                type: "string",
                description:
                    "With dir: the topic scope every file of the folder is filed under. Documents name their own.",
            },
        },
        /* Either `documents` or `dir` with `collection`, which a flat
         * `required` cannot say; `call.ts` checks it and names the missing
         * half. */
        additionalProperties: false,
    },
};

/* §7's `GET /collections` and `GET /stats`, which the CLI answers at once. */
const KB_COLLECTIONS: ToolDefinition = {
    name: "kb_collections",
    description:
        "List the collections in the knowledge base with their document, chunk and byte counts and when each " +
        "was last added to, so you can see what has already been researched and which scope to search or file " +
        "into. A collection is a topic within this workspace's store.",
    inputSchema: {
        type: "object",
        properties: {},
        additionalProperties: false,
    },
};

/* §6's optional layer. Not a graph database: links connect documents only. */
const KB_LINKS: ToolDefinition = {
    name: "kb_links",
    description:
        "Read or state a relationship between two documents. analogue_of is the one that motivated this: " +
        "IOCP and io_uring and kqueue solve the same problem three ways, and no amount of semantic " +
        "similarity will state that. Links connect documents only.",
    inputSchema: {
        type: "object",
        properties: {
            op: {
                type: "string",
                enum: ["list", "add"],
                description:
                    "list reads a document's links in both directions; add states a new one.",
            },
            document: {
                type: "string",
                description: "For list: whose links to read, e.g. D-241.",
            },
            from: { type: "string", description: "For add: the document the link starts at." },
            to: { type: "string", description: "For add: the document it points at." },
            type: {
                type: "string",
                enum: ["supersedes", "cites", "analogue_of", "implements", "see_also"],
                description: "For add: what the relationship is.",
            },
        },
        required: ["op"],
        additionalProperties: false,
    },
};

/* §5's `GET /stale`. `POST /refresh` is beside it in the specification and is
 * NOT here: §9's row for this tool is the read alone, and a refetch is a write
 * that reaches the network on the store's behalf. */
const KB_STALE: ToolDefinition = {
    name: "kb_stale",
    description:
        "List documents older than a threshold, newest sources first. Documentation moves, and a passage " +
        "that cannot say how old it is will eventually be believed when it should not be. This says which " +
        "ones to distrust; refetching them is the reader's call, not yours.",
    inputSchema: {
        type: "object",
        properties: {
            olderThan: {
                type: "string",
                description: "A duration such as 90d. Default: the store's own threshold.",
            },
            collection: { type: "string", description: "Narrow to one collection." },
        },
        additionalProperties: false,
    },
};

/* The six, in §9's own order. */
export const TOOLS: readonly ToolDefinition[] = Object.freeze([
    KB_SEARCH,
    KB_GET,
    KB_ADD,
    KB_COLLECTIONS,
    KB_LINKS,
    KB_STALE,
]);

export const TOOL_NAMES: readonly string[] = Object.freeze(TOOLS.map((t) => t.name));

/* What §9 withholds, by the name it withholds it under. Written down because a
 * rule that lives only in prose is a rule that gets relaxed by somebody who
 * never read the prose — and because the failure is silent: a store with a
 * collection deleted out of it looks exactly like a store that never had one. */
export const NOT_EXPOSED: readonly string[] = Object.freeze([
    "rebuild",
    "reindex",
    "compact",
    "promote",
    "demote",
    "delete",
    "refresh",
    "init",
    "rename",
]);

export function findTool(name: string): ToolDefinition | undefined {
    return TOOLS.find((t) => t.name === name);
}
