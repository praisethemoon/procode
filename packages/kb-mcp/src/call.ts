/* `tools/call`: an argument object in, the store's own rows out.
 *
 * WHAT GOES WRONG HERE IS NOT WHAT GOES WRONG IN `jsonrpc.ts`, AND THE TWO
 * MUST NOT BE FOLDED TOGETHER.
 *
 *   - An argument object this surface will not accept — an unknown tool, a
 *     missing `q`, a `k` of 500, a key spelled `collections` — is a fault in
 *     the CALL. It throws `RpcError` and reaches the client as a transport
 *     error, because the call never happened and there is nothing to read.
 *   - A call that was well formed and that the store refused — `not_found`,
 *     `model_mismatch`, a command this binary does not have yet — is a RESULT
 *     with `isError: true`. The tool ran; this is what it found out. An agent
 *     has to be able to READ that and do something else, and a client that saw
 *     a transport error instead would show the model a broken tool and the
 *     model would ask the same question again.
 *
 * UNKNOWN KEYS ARE REFUSED, WHICH IS THE ONE STRICTNESS WORTH PAYING FOR. The
 * failure it catches is silent: `collection` mistyped as `collections` is not a
 * search of one collection that fails, it is a search of the whole store that
 * succeeds — the wrong answer, confidently, with no sign that a filter was
 * dropped. §9 asks for `GET /search` "with every filter", and a filter that
 * goes nowhere is a filter that is not there.
 *
 * NOTHING HERE RANKS, MERGES, SUMMARISES OR SHORTENS. §4: the store returns
 * passages and does not answer; the rows below are what `kb-js` read out of
 * the CLI's `--json`, unaltered. The one thing this layer decides is that a
 * search result carries snippets, which it does by never asking for anything
 * else.
 */

import {
    BatchDocument,
    Kb,
    SearchMode,
    SearchOptions,
    fetchPage,
    isKbCrash,
    isKbError,
} from "kb-js";

import { INVALID_PARAMS, RpcError } from "./jsonrpc";
import { findTool } from "./tools";

export interface ToolContent {
    readonly type: "text";
    readonly text: string;
}

export interface ToolResult {
    readonly content: readonly ToolContent[];
    readonly isError?: boolean;
}

/* ------------------------------------------------------------ the answers */

/* The store's rows, as JSON text.
 *
 * INDENTED, AND THAT IS A CHOICE WITH A COST. Two spaces per level is perhaps
 * a quarter more of the caller's context than the compact form, bought for a
 * transcript a person can read when a retrieval went wrong — which is most of
 * what anybody does with a log of an agent's tool calls. §4's discipline keeps
 * the size bounded from the other side: search carries snippets, so the thing
 * being indented is small. */
function rows(payload: unknown): ToolResult {
    return { content: [{ type: "text", text: JSON.stringify(payload, null, 2) }] };
}

/* How much of a failing process's diagnostics travel back. A binary in a loop
 * writing to stderr must not be able to fill the caller's context through a
 * failure path. */
const STDERR_LIMIT = 2000;

/* A refusal the store made — exit 1, §11's vocabulary — and a fault in kb
 * itself, kept apart on the wire the way `kb-js` keeps them apart in its types.
 * A caller that saw one word for both would tell the reader to fix input that
 * was never the problem. */
function whyItFailed(e: unknown): Record<string, unknown> {
    if (isKbError(e)) {
        /* §11's details go with the code: the index_stale structure or the
         * unsupported mime is what an agent acts on, and it should not have to
         * parse the message for it. */
        return {
            ok: false,
            kind: "refused",
            error: e.code,
            message: e.message,
            ...(e.details === null ? {} : { details: e.details }),
        };
    }
    if (isKbCrash(e)) {
        return {
            ok: false,
            kind: "failed",
            message: e.message,
            exitCode: e.exitCode,
            stderr: e.stderr.slice(0, STDERR_LIMIT),
        };
    }
    return { ok: false, kind: "failed", message: e instanceof Error ? e.message : String(e) };
}

function failure(e: unknown): ToolResult {
    return {
        content: [{ type: "text", text: JSON.stringify(whyItFailed(e), null, 2) }],
        isError: true,
    };
}

/* ---------------------------------------------------------- the arguments */

function bad(tool: string, detail: string): RpcError {
    return new RpcError(INVALID_PARAMS, `${tool}: ${detail}`);
}

/* The argument object, with every key checked against the schema's own list.
 * `undefined` and `null` are an empty object, because a tool with no required
 * argument may legitimately be called with neither. */
function fields(tool: string, args: unknown, schema: Record<string, unknown>): Record<string, unknown> {
    if (args === undefined || args === null) {
        return {};
    }
    if (typeof args !== "object" || Array.isArray(args)) {
        throw bad(tool, "arguments must be an object.");
    }
    const value = args as Record<string, unknown>;
    const allowed = Object.keys((schema["properties"] ?? {}) as Record<string, unknown>);
    for (const key of Object.keys(value)) {
        if (!allowed.includes(key)) {
            throw bad(
                tool,
                allowed.length === 0
                    ? `there is no argument called "${key}". It takes no arguments.`
                    : `there is no argument called "${key}". It takes: ${allowed.join(", ")}.`,
            );
        }
    }
    for (const key of (schema["required"] ?? []) as string[]) {
        if (value[key] === undefined || value[key] === null) {
            throw bad(tool, `"${key}" is required.`);
        }
    }
    return value;
}

function asString(tool: string, key: string, v: unknown): string | undefined {
    if (v === undefined || v === null) {
        return undefined;
    }
    if (typeof v !== "string") {
        throw bad(tool, `"${key}" must be a string.`);
    }
    return v;
}

function asEnum<T extends string>(
    tool: string,
    key: string,
    v: unknown,
    values: readonly T[],
): T | undefined {
    const s = asString(tool, key, v);
    if (s === undefined) {
        return undefined;
    }
    if (!(values as readonly string[]).includes(s)) {
        throw bad(tool, `"${key}" must be one of: ${values.join(", ")}.`);
    }
    return s as T;
}

function asNumber(tool: string, key: string, v: unknown): number | undefined {
    if (v === undefined || v === null) {
        return undefined;
    }
    if (typeof v !== "number" || !Number.isFinite(v)) {
        throw bad(tool, `"${key}" must be a number.`);
    }
    return v;
}

function asBoolean(tool: string, key: string, v: unknown): boolean | undefined {
    if (v === undefined || v === null) {
        return undefined;
    }
    if (typeof v !== "boolean") {
        throw bad(tool, `"${key}" must be true or false.`);
    }
    return v;
}

function asInteger(
    tool: string,
    key: string,
    v: unknown,
    min: number,
    max?: number,
): number | undefined {
    const n = asNumber(tool, key, v);
    if (n === undefined) {
        return undefined;
    }
    if (!Number.isInteger(n) || n < min || (max !== undefined && n > max)) {
        throw bad(
            tool,
            max === undefined
                ? `"${key}" must be a whole number of at least ${min}.`
                : `"${key}" must be a whole number between ${min} and ${max}.`,
        );
    }
    return n;
}

function asStrings(tool: string, key: string, v: unknown): string[] | undefined {
    if (v === undefined || v === null) {
        return undefined;
    }
    if (!Array.isArray(v)) {
        throw bad(tool, `"${key}" must be an array of strings.`);
    }
    return v.map((element) => {
        if (typeof element !== "string") {
            throw bad(tool, `"${key}" must be an array of strings.`);
        }
        return element;
    });
}

/* --------------------------------------------------------------- the six */

async function search(kb: Kb, args: Record<string, unknown>): Promise<ToolResult> {
    const q = asString("kb_search", "q", args["q"]) ?? "";
    /* §4's filters, all of them. A filter this surface accepts and does not
     * pass on is the silent wrong answer `fields` is written against, from the
     * other direction — so `argv.test.ts` next door and `call.test.ts` here
     * both count them. */
    const options: SearchOptions = {
        collection: asStrings("kb_search", "collection", args["collection"]),
        mode: asEnum("kb_search", "mode", args["mode"], [
            "hybrid",
            "semantic",
            "keyword",
        ]) as SearchMode | undefined,
        k: asInteger("kb_search", "k", args["k"], 1, 100),
        expand: asInteger("kb_search", "expand", args["expand"], 0),
        source: asString("kb_search", "source", args["source"]),
        mime: asString("kb_search", "mime", args["mime"]),
        since: asString("kb_search", "since", args["since"]),
        minScore: asNumber("kb_search", "minScore", args["minScore"]),
        rerank: asBoolean("kb_search", "rerank", args["rerank"]),
    };
    const answer = await kb.search(q, options);
    /* The hits as the store ranked them, and nothing fetched on top of them.
     * ONE PROCESS PER SEARCH is the observable form of §4's rule: a layer that
     * enriched a hit with its document's text would be a second call per row
     * and a context flooded by a list.
     *
     * AND THE WHOLE ANSWER, NOT ONLY THE ROWS. `mode` says which retrieval
     * path actually ran — §4 makes hybrid the default and §8 makes it refuse
     * without a model, so it is not always the one that was asked for — and
     * `olderThan` is the threshold every hit's `stale` flag was measured
     * against. Dropping either is the defect `cli.test.ts` next door is
     * written against, one layer up: a fact the store printed arriving at a
     * caller as nothing. */
    return rows({
        count: answer.count,
        mode: answer.mode,
        olderThan: answer.olderThan,
        /* Present only when some chunks have no vector yet (§4): the
         * semantic side did not see them, so a hit the caller expected may
         * be missing from a semantic or hybrid answer. */
        ...(answer.unembedded !== undefined ? { unembedded: answer.unembedded } : {}),
        hits: answer.hits,
    });
}

/* §1.1's prefixes, which is how one tool serves two routes. */
const CHUNK_PREFIX = "C-";
const DOCUMENT_PREFIX = "D-";

/* §2's `?include=text,chunks,links`, whole. */
const INCLUDES = ["text", "chunks", "links"];

async function get(kb: Kb, args: Record<string, unknown>): Promise<ToolResult> {
    const id = asString("kb_get", "id", args["id"]) ?? "";
    if (id.startsWith(CHUNK_PREFIX)) {
        /* `include` says nothing about a chunk — a chunk read is the text and
         * its neighbours, which is the whole of what there is — so it is
         * ignored rather than refused, the way the schema says. */
        const expand = asInteger("kb_get", "expand", args["expand"], 0);
        return rows(await kb.chunk(id, { expand }));
    }
    if (id.startsWith(DOCUMENT_PREFIX)) {
        /* Text by default, because this is the tool a caller reaches for when
         * they have decided to read the whole thing. `include: []` is how a
         * caller asks for the metadata alone, and it is a real answer rather
         * than a degenerate one: it is the cheap way to check a document's
         * collection, size and age before deciding to pull it in. */
        const include = asStrings("kb_get", "include", args["include"]);
        for (const name of include ?? []) {
            if (!INCLUDES.includes(name)) {
                throw bad("kb_get", `"include" takes ${INCLUDES.join(", ")}, not "${name}".`);
            }
        }
        const wanted = include ?? ["text"];
        return rows(
            await kb.get(id, {
                text: wanted.includes("text"),
                chunks: wanted.includes("chunks"),
                links: wanted.includes("links"),
            }),
        );
    }
    throw bad(
        "kb_get",
        `"${id}" is neither a chunk (C-n) nor a document (D-n). The prefix says which kind an id is.`,
    );
}

/* §2.1's folder. The two forms of the tool share nothing but the collection's
 * name, so each refuses the other's arguments rather than filing one and
 * quietly dropping the rest. */
function given(v: unknown): boolean {
    return v !== undefined && v !== null;
}

async function addDir(kb: Kb, args: Record<string, unknown>): Promise<ToolResult> {
    if (given(args["documents"])) {
        throw bad("kb_add", 'give either "documents" or "dir", not both.');
    }
    const dir = asString("kb_add", "dir", args["dir"]) ?? "";
    if (dir.trim() === "") {
        throw bad("kb_add", '"dir" must name a folder.');
    }
    const collection = asString("kb_add", "collection", args["collection"]);
    if (collection === undefined || collection.trim() === "") {
        throw bad("kb_add", '"dir" needs "collection": the topic every file of the folder is filed under.');
    }
    /* NEVER FORGETTING. A file gone from the folder is reported under
     * `missing` and stays in the store: forgetting is the reader's decision
     * (§9), and a folder walk is not a way round that. The path goes as the
     * agent wrote it, so a relative one resolves against the directory the
     * CLI runs in, which is the server's own. */
    return rows(await kb.addDir(dir, { collection, forget: false }));
}

/* Web pages by URL: each fetched here (kb itself never goes online) and filed
 * as it arrived, under its URL, with its type and ETag, so the store holds the
 * page and not a summary of it, and Knowledge's Refresh can fetch it again.
 * The pages are fetched together and filed one by one; each has its own
 * answer, and a page that fails is reported without stopping the others. */
async function addUrls(kb: Kb, args: Record<string, unknown>): Promise<ToolResult> {
    if (given(args["documents"]) || given(args["dir"]) || given(args["collection"])) {
        throw bad("kb_add", 'give "urls" on its own: each entry names its own collection.');
    }
    const list = args["urls"];
    if (!Array.isArray(list) || list.length === 0) {
        throw bad("kb_add", '"urls" must be a non-empty array.');
    }
    const wanted = list.map((entry, i) => {
        const where = `urls[${i}]`;
        if (typeof entry !== "object" || entry === null || Array.isArray(entry)) {
            throw bad("kb_add", `${where} must be an object.`);
        }
        const u = entry as Record<string, unknown>;
        for (const key of Object.keys(u)) {
            if (!["url", "collection", "title"].includes(key)) {
                throw bad("kb_add", `${where} has no argument called "${key}".`);
            }
        }
        const url = asString("kb_add", `${where}.url`, u["url"]);
        const collection = asString("kb_add", `${where}.collection`, u["collection"]);
        if (url === undefined || url.trim() === "" || collection === undefined || collection.trim() === "") {
            throw bad("kb_add", `${where} needs url and collection.`);
        }
        return { url: url.trim(), collection, title: asString("kb_add", `${where}.title`, u["title"]) };
    });

    const pages = await Promise.allSettled(wanted.map((w) => fetchPage(w.url)));
    const results: Record<string, unknown>[] = [];
    let pending = 0;
    for (const [i, w] of wanted.entries()) {
        const page = pages[i];
        if (page.status === "rejected") {
            results.push({ url: w.url, ok: false, because: (page.reason as Error).message });
            continue;
        }
        if (page.value.notModified) {
            continue; // not asked for conditionally, so not reachable; keeps the type honest
        }
        const fetched = page.value;
        try {
            const filed = await kb.add(fetched.text, {
                title: w.title?.trim() || fetched.title,
                collection: w.collection,
                url: w.url,
                mime: fetched.mime,
                etag: fetched.etag,
            });
            pending = filed.pending;
            results.push({
                url: w.url,
                ok: true,
                outcome: filed.created ? "filed" : filed.reindexed ? "updated" : "unchanged",
                document: filed.document,
                title: w.title?.trim() || fetched.title,
                collection: filed.collection,
                mime: filed.mime,
                bytes: filed.bytes,
            });
        } catch (e) {
            results.push({ url: w.url, ok: false, because: whyItFailed(e) });
        }
    }
    const filed = results.filter((r) => r["ok"] === true).length;
    const answer: ToolResult = rows({ filed, failed: results.length - filed, results, pending });
    return filed === 0 ? { ...answer, isError: true } : answer;
}

async function add(kb: Kb, args: Record<string, unknown>): Promise<ToolResult> {
    if (given(args["urls"])) {
        return addUrls(kb, args);
    }
    if (given(args["dir"])) {
        return addDir(kb, args);
    }
    if (given(args["collection"])) {
        throw bad("kb_add", '"collection" goes with "dir"; each of the documents names its own.');
    }
    const list = args["documents"];
    if (!Array.isArray(list) || list.length === 0) {
        throw bad("kb_add", '"documents" must be a non-empty array.');
    }
    const documents: BatchDocument[] = list.map((entry, i) => {
        if (typeof entry !== "object" || entry === null || Array.isArray(entry)) {
            throw bad("kb_add", `documents[${i}] must be an object.`);
        }
        const d = entry as Record<string, unknown>;
        const where = `documents[${i}]`;
        for (const key of Object.keys(d)) {
            if (!["title", "content", "collection", "url", "mime", "meta"].includes(key)) {
                throw bad("kb_add", `${where} has no argument called "${key}".`);
            }
        }
        const title = asString("kb_add", `${where}.title`, d["title"]);
        const content = asString("kb_add", `${where}.content`, d["content"]);
        const collection = asString("kb_add", `${where}.collection`, d["collection"]);
        if (title === undefined || content === undefined || collection === undefined) {
            throw bad("kb_add", `${where} needs title, content and collection.`);
        }
        const meta = d["meta"];
        if (meta !== undefined && meta !== null) {
            if (typeof meta !== "object" || Array.isArray(meta)) {
                throw bad("kb_add", `${where}.meta must be an object.`);
            }
        }
        /* NO DESTINATION. The CLI files into the `.kb/` it finds by walking
         * up from the server's working directory, and when there is none it
         * refuses with `not_found` naming `kb init`. That refusal comes back
         * below as an `isError` result the agent can read, with `filed: 0` —
         * not as a transport error, because the call was well formed and the
         * store is what said no. */
        return {
            title,
            content,
            collection,
            url: asString("kb_add", `${where}.url`, d["url"]),
            mime: asString("kb_add", `${where}.mime`, d["mime"]),
            meta: (meta ?? undefined) as Readonly<Record<string, unknown>> | undefined,
        };
    });

    /* §2's `POST /documents/batch`: one call, one lock, one index rebuild, and
     * all or nothing — a document the store refuses refuses the batch, and
     * nothing is filed. The answer says so, so an agent never has to work out
     * which of its documents made it in. */
    try {
        /* No embedding budget is sent: the store's own default keeps an
         * agent's call short, and whatever it did not reach is `pending` —
         * searchable by keyword at once, and embedded later (§2). */
        const batch = await kb.addBatch(documents);
        return rows({ filed: batch.added.length, added: batch.added, pending: batch.pending });
    } catch (e) {
        return {
            content: [
                {
                    type: "text",
                    text: JSON.stringify(
                        {
                            ok: false,
                            filed: 0,
                            added: [],
                            because: whyItFailed(e),
                            note: "the documents are filed as one batch: this one was refused, so none of them is in the store.",
                        },
                        null,
                        2,
                    ),
                },
            ],
            isError: true,
        };
    }
}

/* §9's row for this tool is TWO routes, `GET /collections` and `GET /stats`,
 * and the CLI prints them as two commands whose rows overlap and neither of
 * which contains the other: `collections` carries §5's oldest fetch date and
 * `stats` carries the chunk count and a store-wide total. Both are asked, and
 * the answers are joined on the identity §1.3 already defines — a collection
 * name, which is unique within the one store.
 *
 * THAT JOIN IS NOT THIS LAYER COMPUTING A FIELD. Nothing is summed, ranked or
 * inferred; two rows about the same thing become one row about it, and every
 * number in it is a number the store printed. The alternative — picking one
 * command and calling it the answer — would either report a chunk count of
 * zero for a full topic or drop the one date that says whether a topic has
 * been looked at this year. */
async function collections(kb: Kb): Promise<ToolResult> {
    const list = await kb.collections();
    const stats = await kb.stats();
    const chunks = new Map(stats.collections.map((c) => [c.name, c.chunks]));
    return rows({
        count: list.length,
        collections: list.map((c) => ({ ...c, chunks: chunks.get(c.name) })),
        totals: stats.totals,
    });
}

const LINK_TYPES = ["supersedes", "cites", "analogue_of", "implements", "see_also"] as const;

async function links(kb: Kb, args: Record<string, unknown>): Promise<ToolResult> {
    const op = asEnum("kb_links", "op", args["op"], ["list", "add"] as const);
    if (op === "list") {
        const document = asString("kb_links", "document", args["document"]);
        if (document === undefined || document === "") {
            throw bad("kb_links", 'op "list" needs the document whose links to read.');
        }
        for (const key of ["from", "to", "type"]) {
            if (args[key] !== undefined) {
                throw bad("kb_links", `"${key}" belongs to op "add", not to "list".`);
            }
        }
        return rows(await kb.links(document));
    }
    const from = asString("kb_links", "from", args["from"]);
    const to = asString("kb_links", "to", args["to"]);
    const type = asEnum("kb_links", "type", args["type"], LINK_TYPES);
    if (from === undefined || to === undefined || type === undefined) {
        throw bad("kb_links", 'op "add" needs from, to and type.');
    }
    if (args["document"] !== undefined) {
        throw bad("kb_links", '"document" belongs to op "list", not to "add".');
    }
    return rows(await kb.link(from, type, to));
}

/* The rows AND the threshold the store used. "18 documents are stale" means
 * nothing without "older than what", and a caller that sent no `olderThan`
 * cannot say which default it got — so the answer carries the store's own
 * `olderThan` and the instant it computed from it. */
async function stale(kb: Kb, args: Record<string, unknown>): Promise<ToolResult> {
    return rows(
        await kb.stale({
            olderThan: asString("kb_stale", "olderThan", args["olderThan"]),
            collection: asString("kb_stale", "collection", args["collection"]),
        }),
    );
}

type Handler = (kb: Kb, args: Record<string, unknown>) => Promise<ToolResult>;

/* The dispatch table. Its keys are checked against `TOOLS` by test, in both
 * directions: a tool with no handler is a tool that answers nothing, and a
 * handler with no tool is a seventh route into the store that `tools/list`
 * never mentions. */
export const HANDLERS: Readonly<Record<string, Handler>> = Object.freeze({
    kb_search: search,
    kb_get: get,
    kb_add: add,
    kb_collections: collections,
    kb_links: links,
    kb_stale: stale,
});

export async function callTool(kb: Kb, name: string, args: unknown): Promise<ToolResult> {
    const tool = findTool(name);
    const handler = HANDLERS[name];
    if (tool === undefined || handler === undefined) {
        /* A fault in the call, so it goes back as a transport error: there is
         * no tool, so there is no result for the model to read. */
        throw new RpcError(INVALID_PARAMS, `There is no tool called "${name}".`);
    }
    const checked = fields(name, args, tool.inputSchema);
    try {
        return await handler(kb, checked);
    } catch (e) {
        /* An `RpcError` raised while reading the arguments is still a fault in
         * the call and keeps travelling as one. Everything else is the store
         * having answered, or failed to. */
        if (e instanceof RpcError) {
            throw e;
        }
        return failure(e);
    }
}
