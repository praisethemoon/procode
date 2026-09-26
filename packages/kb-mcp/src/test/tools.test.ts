/* §9's surface, stated as a closed set.
 *
 * "SIX TOOLS" IS A CEILING AND NOT A COUNT. The section is as much about what
 * is withheld as about what is offered: `rebuild`, `reindex`, `compact`,
 * `promote`, `demote` and every `DELETE` are absent because an agent files
 * knowledge into the workspace store and reads from it, while forgetting — and
 * deciding what the store is for and where it lives — are the reader's
 * decisions. A seventh tool is therefore a defect against the
 * specification even when it is useful, and especially when it is: the useful
 * ones are exactly the ones somebody adds without reading §9.
 *
 * So the list is asserted whole, by name and in order, and the failure message
 * says what the rule is rather than what the number was.
 */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import { HANDLERS } from "../call";
import { NOT_EXPOSED, TOOLS, TOOL_NAMES, ToolDefinition, findTool } from "../tools";

/* §9's table, transcribed. */
const SIX = ["kb_search", "kb_get", "kb_add", "kb_collections", "kb_links", "kb_stale"];

test("there are exactly six tools, and they are §9's six", () => {
    assert.deepEqual(
        [...TOOL_NAMES],
        SIX,
        "index-api.md §9 fixes this surface at six tools; a seventh is a route into the store that the section decided not to have",
    );
    assert.equal(TOOLS.length, 6);
});

test("nothing §9 withholds is reachable through a tool name or a handler", () => {
    /* The named refusals, checked as substrings: `kb_rebuild`, `kb_delete_doc`
     * and `kb_promote` all fail this, and so does anything that spells one of
     * them differently while still being one. */
    for (const forbidden of NOT_EXPOSED) {
        for (const name of [...TOOL_NAMES, ...Object.keys(HANDLERS)]) {
            assert.ok(
                !name.includes(forbidden),
                `"${name}" exposes ${forbidden}, which §9 withholds: an agent reads and files, and forgetting is the reader's decision`,
            );
        }
    }
});

test("every tool has a handler and every handler has a tool", () => {
    /* A tool with no handler answers nothing; a handler with no tool is a
     * route into the store that `tools/list` never mentions, which is the
     * seventh tool arriving by the back door. */
    assert.deepEqual(Object.keys(HANDLERS).sort(), [...TOOL_NAMES].sort());
});

test("the list cannot be added to at run time", () => {
    /* Frozen, because the guarantee above is worth nothing if a module loaded
     * later can push onto the array that `tools/list` returns. */
    assert.throws(() => (TOOLS as ToolDefinition[]).push({} as ToolDefinition));
    assert.equal(TOOLS.length, 6);
});

test("every tool says what it is for, and no description promises an answer", () => {
    /* §4: the store returns passages and does not summarise, synthesise or
     * answer. A description that said otherwise would be a promise the store
     * cannot keep, made in the one place a model actually reads. */
    for (const tool of TOOLS) {
        assert.ok(tool.description.length > 40, `${tool.name} has no real description`);
        assert.ok(
            !/\b(summaris|summariz|synthesis|answers your question)/i.test(tool.description),
            `${tool.name} promises to summarise or answer; the store returns passages`,
        );
    }
});

test("every schema is an object schema that names its own properties", () => {
    for (const tool of TOOLS) {
        const schema = tool.inputSchema;
        assert.equal(schema["type"], "object", `${tool.name} does not take an object`);
        assert.equal(
            typeof schema["properties"],
            "object",
            `${tool.name} names no properties, so nothing can be checked against it`,
        );
        assert.equal(
            schema["additionalProperties"],
            false,
            `${tool.name} accepts keys it does not name, which is how a filter goes silently nowhere`,
        );
    }
});

test("kb_search takes every filter §4 names", () => {
    /* §9's row is `GET /search` WITH EVERY FILTER. A missing one is not a
     * smaller tool, it is a tool that searches more of the store than the
     * caller asked it to and says nothing about it. */
    const schema = findTool("kb_search")?.inputSchema ?? {};
    assert.deepEqual(
        Object.keys(schema["properties"] as Record<string, unknown>).sort(),
        [
            "collection",
            "expand",
            "k",
            "mime",
            "minScore",
            "mode",
            "q",
            "rerank",
            "since",
            "source",
        ].sort(),
    );
    assert.deepEqual(schema["required"], ["q"]);
});

test("kb_get takes one id and not a list of them", () => {
    /* §4's discipline: search returns snippets because a list must not be able
     * to flood a caller's context, and this is the deliberate, one-at-a-time
     * way to ask for the whole thing. An `ids` array here would be a list of
     * whole documents, which is the exact shape the rule exists to prevent. */
    const properties = (findTool("kb_get")?.inputSchema["properties"] ?? {}) as Record<
        string,
        Record<string, unknown>
    >;
    assert.equal(properties["id"]["type"], "string");
    assert.equal("ids" in properties, false);
    assert.deepEqual(findTool("kb_get")?.inputSchema["required"], ["id"]);
});

test("kb_add takes documents or a folder, and nothing that names a destination", () => {
    /* There is one store, the one the server's working directory finds, and a
     * write lands in it or is refused. An argument that chose somewhere else —
     * the `store` that once picked a tier — would be a second answer to a
     * question the working directory already answered. `dir` is where the
     * content comes FROM, and `collection` is the folder's topic. */
    const schema = findTool("kb_add")?.inputSchema ?? {};
    const properties = schema["properties"] as Record<string, unknown>;
    assert.deepEqual(Object.keys(properties), ["documents", "dir", "collection"]);
    /* Neither form is required on its own, so the schema cannot say "one of";
     * `call.ts` does. */
    assert.equal("required" in schema, false);
    const item = (properties["documents"] as Record<string, unknown>)["items"] as Record<
        string,
        unknown
    >;
    assert.equal("store" in (item["properties"] as Record<string, unknown>), false);
    assert.deepEqual((item["required"] as string[]).sort(), ["collection", "content", "title"]);
});

test("kb_add offers no way to forget, and says a folder's gone files are missing", () => {
    /* §9: forgetting is the reader's decision. The CLI's `--no-forget` is sent
     * on every folder filing and there is no argument to turn it off — and the
     * description says what happens instead, because it is the one place a
     * model is told. */
    const tool = findTool("kb_add");
    const keys = Object.keys((tool?.inputSchema["properties"] ?? {}) as Record<string, unknown>);
    assert.equal(keys.some((k) => /forget/i.test(k)), false);
    assert.match(String(tool?.description), /reported as missing, not forgotten/);
});

test("kb_add and kb_search say what pending and unembedded mean", () => {
    /* The one place a model is told that a large filing is searchable by
     * keyword at once and semantically only later, and what a count of chunks
     * without a vector in a search answer is warning it about. */
    assert.match(String(findTool("kb_add")?.description), /pending: N/);
    assert.match(String(findTool("kb_add")?.description), /searchable by keyword at once/);
    assert.match(String(findTool("kb_add")?.description), /Finish embedding/);
    assert.match(String(findTool("kb_search")?.description), /unembedded: N/);
});

test("kb_links names §6's five relationship types and no sixth", () => {
    const properties = (findTool("kb_links")?.inputSchema["properties"] ?? {}) as Record<
        string,
        Record<string, unknown>
    >;
    assert.deepEqual(properties["type"]["enum"], [
        "supersedes",
        "cites",
        "analogue_of",
        "implements",
        "see_also",
    ]);
    assert.deepEqual(properties["op"]["enum"], ["list", "add"]);
});

test("kb_stale reads and does not refetch", () => {
    /* §5 puts `GET /stale` and `POST /refresh` side by side; §9's row for this
     * tool is the read alone. A refetch is a write that reaches the network on
     * the store's behalf. */
    const tool = findTool("kb_stale");
    assert.deepEqual(
        Object.keys((tool?.inputSchema["properties"] ?? {}) as Record<string, unknown>).sort(),
        ["collection", "olderThan"],
    );
    /* And the description says so, because the one place a model is told what
     * a tool will not do is the sentence it reads before calling it. */
    assert.match(String(tool?.description), /reader's call, not yours/);
});

test("no schema offers an argument that would reach something §9 withholds", () => {
    /* A `refresh: true` on kb_stale, or a `rebuild` on kb_collections, would
     * be a seventh tool wearing a sixth one's name — which is how the list
     * stays at six while the surface grows. */
    for (const tool of TOOLS) {
        for (const key of Object.keys(
            (tool.inputSchema["properties"] ?? {}) as Record<string, unknown>,
        )) {
            for (const forbidden of NOT_EXPOSED) {
                assert.ok(
                    !key.toLowerCase().includes(forbidden),
                    `${tool.name} takes "${key}", which reaches ${forbidden}`,
                );
            }
        }
    }
});

test("no tool offers a store selector, because there is one store", () => {
    /* The global tier and the `store` argument that chose between tiers are
     * gone. A schema that still advertised it would invite a model to send a
     * key the call then refuses, which is a tool that teaches its own misuse. */
    for (const tool of TOOLS) {
        const properties = (tool.inputSchema["properties"] ?? {}) as Record<string, unknown>;
        assert.equal("store" in properties, false, `${tool.name} still offers "store"`);
        assert.doesNotMatch(tool.description, /\btier|global/i, `${tool.name} still describes tiers`);
    }
    assert.deepEqual(findTool("kb_collections")?.inputSchema["properties"], {});
});
