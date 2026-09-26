/* The MCP methods, and the refusal of everything else.
 *
 * FIVE METHODS AND NO SIXTH. `initialize`, `notifications/initialized`,
 * `ping`, `tools/list`, `tools/call`. `resources/*` and `prompts/*` are not
 * here because the capabilities this server announces do not include them, and
 * a method that is answered without being announced is a contract stated twice
 * — a client reads the capabilities to decide what to ask for, and a server
 * that quietly answers more is a server whose behaviour and whose advertisement
 * can drift apart. Anything unknown is `-32601`, which is the answer that lets
 * a client find out what is here rather than hang waiting for one.
 *
 * THE VERSION THAT COMES BACK IS THE ONE BEING SPOKEN, NOT THE ONE ASKED FOR.
 * MCP's revisions differ for this server in exactly one place: whether a client
 * may send a JSON-RPC batch. `2024-11-05` and `2025-03-26` permit one and
 * `2025-06-18` removed it, so those first two are echoed and anything else —
 * newer, older, absent, nonsense — is answered with this server's own. That is
 * what the handshake is for: a client told `2024-11-05` knows not to expect
 * what a later revision added, where a client told its own version back would
 * assume the whole of it. The framing handles a batch regardless, because a
 * server that mishandles one corrupts the stream for the rest of the session
 * rather than failing the one call.
 *
 * NO HANDSHAKE GATE. A `tools/call` that arrives before `initialize` is
 * answered rather than refused. The refusal would protect nothing — this
 * server holds no per-session state to be confused, and `Kb` starts a fresh
 * process per call — and it would add a failure mode whose symptom is a tool
 * that works from one client and not from another.
 */

import { Kb } from "kb-js";

import { ToolResult, callTool } from "./call";
import { Dispatch, INVALID_PARAMS, METHOD_NOT_FOUND, RpcError } from "./jsonrpc";
import { TOOLS } from "./tools";

export const SERVER_NAME = "kb-mcp";
export const SERVER_VERSION = "0.1.0";

/* What is answered when the client's version is not one this server knows. */
export const PROTOCOL_VERSION = "2024-11-05";

/* The revisions that are echoed back unchanged: the ones in which a JSON-RPC
 * batch is part of the transport, which is the only difference that reaches
 * this server. */
export const SUPPORTED_PROTOCOL_VERSIONS: readonly string[] = Object.freeze([
    "2024-11-05",
    "2025-03-26",
]);

/* Sent once at the handshake and shown to the model before it has called
 * anything. §2's intended flow, in the two sentences that change behaviour:
 * search before fetching, and file what you read. */
export const INSTRUCTIONS =
    "A local, offline knowledge base of documentation, source and papers, searched with kb_search and read " +
    "with kb_get. Search it BEFORE fetching anything from the network: research done earlier is indexed here " +
    "rather than discarded. File what you read back with kb_add as you go, so the second question on a topic " +
    "is answered from disk. It also holds folders of code filed whole (kb_add with dir), searchable by name " +
    "and by meaning. For a question where the best passage matters more than a second of latency, pass " +
    "rerank: true to kb_search. It returns passages with their provenance — it does not summarise or answer; " +
    "that is your work.";

export class Server {
    constructor(private readonly kb: Kb) {}

    /* The one entry point `jsonrpc.ts` calls. It returns a result, or throws an
     * `RpcError` when the call itself was wrong. */
    readonly dispatch: Dispatch = async (method: string, params: unknown): Promise<unknown> => {
        switch (method) {
            case "initialize":
                return this.initialize(params);
            /* A notification. Nothing is owed and nothing is sent; it is named
             * here so that it is visibly handled rather than falling through
             * to a method-not-found that is then discarded for having no id. */
            case "notifications/initialized":
                return {};
            case "ping":
                return {};
            case "tools/list":
                return { tools: TOOLS };
            case "tools/call":
                return this.call(params);
            default:
                throw new RpcError(METHOD_NOT_FOUND, `There is no method called "${method}".`);
        }
    };

    private initialize(params: unknown): Record<string, unknown> {
        const asked = read(params)["protocolVersion"];
        const version =
            typeof asked === "string" && SUPPORTED_PROTOCOL_VERSIONS.includes(asked)
                ? asked
                : PROTOCOL_VERSION;
        return {
            protocolVersion: version,
            /* Tools, and nothing else. The list never changes — §9 fixes it at
             * six — so there is no `listChanged` to announce. */
            capabilities: { tools: {} },
            serverInfo: { name: SERVER_NAME, version: SERVER_VERSION },
            instructions: INSTRUCTIONS,
        };
    }

    private async call(params: unknown): Promise<ToolResult> {
        const p = read(params);
        const name = p["name"];
        if (typeof name !== "string" || name === "") {
            throw new RpcError(INVALID_PARAMS, "tools/call needs the name of a tool.");
        }
        return callTool(this.kb, name, p["arguments"]);
    }
}

/* Params as an object. `_meta` and anything else a client attaches at this
 * level is left alone: the strictness that matters is over a tool's own
 * arguments, where a key nobody reads is a filter that silently does nothing,
 * and MCP reserves the right to add fields here. */
function read(params: unknown): Record<string, unknown> {
    return typeof params === "object" && params !== null && !Array.isArray(params)
        ? (params as Record<string, unknown>)
        : {};
}
