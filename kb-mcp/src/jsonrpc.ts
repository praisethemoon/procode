/* JSON-RPC 2.0, and the one distinction the rest of this package rests on.
 *
 * A PROTOCOL ERROR IS AN `error` RESPONSE. A TOOL FAILURE IS A `result`. They
 * are not the same thing and an agent cannot act on them the same way. "There
 * is no tool called kb_serach" is a fault in the call — the model wrote
 * something the server does not have, and the transport says so with a code.
 * "kb refused: not_found" is the tool working correctly and reporting what the
 * store said — the model has to READ that, and a client that turned it into a
 * transport error would show the model a failed call instead of an answer, and
 * the model would retry the same question. So: this file produces the first
 * kind, `call.ts` produces the second, and the boundary between them is the
 * one thing a mutation test here is aimed at.
 *
 * THE ID DECIDES WHETHER ANYTHING IS SENT BACK AT ALL. A message with no `id`
 * is a notification, and a notification gets NO response — not a result, and
 * not an error either, even when the method does not exist. This is the rule
 * people break, because answering seems helpful; a client that receives a
 * response it has no id to match is a client with an unhandled message, and
 * some of them close the connection over it.
 *
 * BATCHES. JSON-RPC 2.0 allows an array of messages and answers with an array
 * of the responses that are owed — which is not one per element, because the
 * notifications in it are owed nothing. A batch of nothing but notifications
 * produces no output at all, and an EMPTY array is itself an invalid request.
 * MCP's own revisions have gone back and forth on whether a client may send
 * one; a server that handles them correctly is compatible with every revision,
 * and a server that mishandles them corrupts the stream for the rest of the
 * session rather than failing the one call.
 *
 * NOTHING HERE KNOWS WHAT MCP IS. `server.ts` supplies the methods.
 */

/* The five codes the specification reserves. */
export const PARSE_ERROR = -32700;
export const INVALID_REQUEST = -32600;
export const METHOD_NOT_FOUND = -32601;
export const INVALID_PARAMS = -32602;
export const INTERNAL_ERROR = -32603;

export type RpcId = string | number;

/* Something the caller must be told about through the transport, with the code
 * the transport reserves for it. A handler throws one of these when the CALL
 * is wrong; when the call is right and the work failed, it returns a result
 * that says so. */
export class RpcError extends Error {
    readonly code: number;
    readonly data: unknown;

    constructor(code: number, message: string, data?: unknown) {
        super(message);
        this.name = "RpcError";
        this.code = code;
        this.data = data;
    }
}

export type Incoming =
    | { kind: "request"; id: RpcId; method: string; params: unknown }
    | { kind: "notification"; method: string; params: unknown }
    /* A response to a request we never made. Nothing is sent back: answering a
     * response with an error is how two implementations start talking past
     * each other for ever. */
    | { kind: "ignore" }
    | { kind: "invalid"; id: RpcId | null; error: RpcError };

function isObject(v: unknown): v is Record<string, unknown> {
    return typeof v === "object" && v !== null && !Array.isArray(v);
}

/* An id that can be matched. JSON-RPC allows null and MCP forbids it, and the
 * reason is this: null is also what a server sends when it could not work out
 * whose request failed, so a client that used it as an id could not tell its
 * own call from a message about nobody's. */
function readId(v: unknown): RpcId | null {
    if (typeof v === "string") {
        return v;
    }
    return typeof v === "number" && Number.isFinite(v) ? v : null;
}

export function classify(value: unknown): Incoming {
    if (!isObject(value)) {
        return {
            kind: "invalid",
            id: null,
            error: new RpcError(INVALID_REQUEST, "a message must be a JSON object."),
        };
    }
    const hasId = "id" in value;
    const id = hasId ? readId(value["id"]) : null;

    /* A result or an error where a method should be is the other end
     * answering something. We never ask, so there is nothing to match it to. */
    if (!("method" in value) && hasId && ("result" in value || "error" in value)) {
        return { kind: "ignore" };
    }

    if (value["jsonrpc"] !== "2.0") {
        return {
            kind: "invalid",
            id,
            error: new RpcError(INVALID_REQUEST, 'a message must carry jsonrpc: "2.0".'),
        };
    }
    if (typeof value["method"] !== "string") {
        return {
            kind: "invalid",
            id,
            error: new RpcError(INVALID_REQUEST, "a message must carry a method name."),
        };
    }
    /* Present, and structured. The specification allows an object or an array
     * and nothing else; a string or a number here is a client that built the
     * message by hand and got it wrong, and it is better told than guessed at. */
    if ("params" in value && value["params"] !== undefined && value["params"] !== null) {
        const params = value["params"];
        if (!isObject(params) && !Array.isArray(params)) {
            return {
                kind: "invalid",
                id,
                error: new RpcError(INVALID_REQUEST, "params must be an object or an array."),
            };
        }
    }

    if (!hasId) {
        return { kind: "notification", method: value["method"], params: value["params"] };
    }
    if (id === null) {
        return {
            kind: "invalid",
            id: null,
            error: new RpcError(
                INVALID_REQUEST,
                "a request id must be a string or a number, and must not be null.",
            ),
        };
    }
    return { kind: "request", id, method: value["method"], params: value["params"] };
}

export function resultResponse(id: RpcId, result: unknown): Record<string, unknown> {
    return { jsonrpc: "2.0", id, result };
}

export function errorResponse(id: RpcId | null, error: RpcError): Record<string, unknown> {
    const body: Record<string, unknown> = { code: error.code, message: error.message };
    if (error.data !== undefined) {
        body["data"] = error.data;
    }
    return { jsonrpc: "2.0", id, error: body };
}

/* What a method does. It returns the result, or throws an `RpcError` when the
 * CALL was wrong. Anything else it throws is this server's own bug and becomes
 * an internal error — with its message, because a server that swallows the one
 * sentence naming its own fault is a server nobody can debug from a log. */
export type Dispatch = (method: string, params: unknown) => Promise<unknown>;

async function answer(message: Incoming, dispatch: Dispatch): Promise<unknown | null> {
    if (message.kind === "ignore") {
        return null;
    }
    if (message.kind === "invalid") {
        /* An invalid message with no usable id cannot be matched by the
         * client, but it is still sent: a client that gets an error back knows
         * it sent nonsense, and one that gets silence waits. */
        return errorResponse(message.id, message.error);
    }
    if (message.kind === "notification") {
        try {
            await dispatch(message.method, message.params);
        } catch {
            /* Nothing is owed and nothing can be sent. A notification for a
             * method that does not exist is a client being forward-compatible,
             * which is the shape MCP's own `notifications/*` take. */
        }
        return null;
    }
    try {
        return resultResponse(message.id, await dispatch(message.method, message.params));
    } catch (e) {
        if (e instanceof RpcError) {
            return errorResponse(message.id, e);
        }
        return errorResponse(
            message.id,
            new RpcError(INTERNAL_ERROR, e instanceof Error ? e.message : String(e)),
        );
    }
}

/* One line in, one line's worth of response out — or nothing, when nothing is
 * owed. Returns the object to send, never the text: `framing.ts` owns the
 * bytes and this owns the meaning. */
export async function handle(line: string, dispatch: Dispatch): Promise<unknown | null> {
    let value: unknown;
    try {
        value = JSON.parse(line);
    } catch (e) {
        return errorResponse(
            null,
            new RpcError(PARSE_ERROR, e instanceof Error ? e.message : "invalid JSON."),
        );
    }

    if (!Array.isArray(value)) {
        return answer(classify(value), dispatch);
    }

    /* An empty batch is the one case the specification calls out by name: it
     * is a single Invalid Request, not an empty array of them. */
    if (value.length === 0) {
        return errorResponse(
            null,
            new RpcError(INVALID_REQUEST, "an empty batch asks for nothing."),
        );
    }

    /* In order, and sequentially. The elements of a batch may be two writes
     * into the same store, and a store is a set of append-only logs with a
     * lock on it — running them at once would turn a batch into a contention
     * test the caller did not ask for. */
    const responses: unknown[] = [];
    for (const element of value) {
        const response = await answer(classify(element), dispatch);
        if (response !== null) {
            responses.push(response);
        }
    }
    /* All notifications: nothing is owed, so nothing is written. An empty
     * array here would be a response the client cannot match to anything. */
    return responses.length === 0 ? null : responses;
}
