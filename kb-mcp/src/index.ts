/* `kb-mcp`: index-api.md §9's six tools, spoken as JSON-RPC 2.0 over stdio.
 *
 * NO SDK, HAND-ROLLED, which is the shape `coboard-mcp` took and is the same
 * decision for the same reason: the protocol this server needs is a handshake,
 * a list and a call, and a dependency tree to speak it would be a larger thing
 * to audit than the thing it exposes. The whole of it is `framing.ts` (bytes),
 * `jsonrpc.ts` (meaning) and `server.ts` (methods), and each is readable in one
 * sitting.
 *
 * IT DOES NOT SPAWN THE CLI. `kb-js` does that, because §10 says retrieval has
 * exactly one implementation: searching requires embedding the query, which
 * requires the model, and two inference paths that disagree produce a store
 * that answers differently depending on which door the caller came through.
 * An agent and a reader must get the same answer to the same question, and
 * this is the door the agent comes through.
 */

export { FramingError, LineReader, DEFAULT_MAX_LINE_BYTES, encode } from "./framing";
export {
    Dispatch,
    INTERNAL_ERROR,
    INVALID_PARAMS,
    INVALID_REQUEST,
    Incoming,
    METHOD_NOT_FOUND,
    PARSE_ERROR,
    RpcError,
    RpcId,
    classify,
    errorResponse,
    handle,
    resultResponse,
} from "./jsonrpc";
export { HANDLERS, ToolContent, ToolResult, callTool } from "./call";
export {
    INSTRUCTIONS,
    PROTOCOL_VERSION,
    SERVER_NAME,
    SERVER_VERSION,
    SUPPORTED_PROTOCOL_VERSIONS,
    Server,
} from "./server";
export { NOT_EXPOSED, TOOLS, TOOL_NAMES, ToolDefinition, findTool } from "./tools";
export { ServeOptions, serve } from "./transport";
