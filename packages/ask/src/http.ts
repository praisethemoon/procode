/* ask's MCP server over Streamable HTTP, on 127.0.0.1 (specs/ask.md §4).
 *
 * Only what ask needs of the transport. A client POSTs one JSON-RPC message
 * at a time. A request is answered with JSON, except a tool call, which is
 * answered with an event stream: its progress notifications, then its
 * result. Notifications get 202 and no body. There is no stream the server
 * opens on its own, so GET is 405, and no batches.
 *
 * ONLY THIS MACHINE'S CLIENTS, AND NO WEB PAGE. The server listens on
 * 127.0.0.1 only. A request whose Host is not this address and port is
 * refused, which stops a page that rebinds its own name to 127.0.0.1. So is
 * any request carrying an Origin: browsers send one, agents do not. The body
 * must be application/json, which a page cannot send across origins without
 * a preflight this server never answers.
 *
 * A call is cancelled when its client says so (notifications/cancelled) or
 * hangs up before the result; either way the form it opened is cancelled
 * and its tab closes. Calls are told apart by session (Mcp-Session-Id, handed
 * out at initialize) and JSON-RPC id, since every client numbers from 1.
 */

import { randomUUID } from "node:crypto";
import * as http from "node:http";

import type { Ctx } from "./mcp";
import { handle } from "./mcp";

type Json = Record<string, unknown>;

export const MAX_BODY = 4 * 1024 * 1024;

export interface ServeOptions {
    /** The ctx every call gets, less what the transport fills in. */
    readonly ctx: Omit<Ctx, "notify" | "signal" | "progressToken">;
    /** The port to try first; the next ones are tried while it is taken. */
    readonly port: number;
    /** How many ports to try, from `port` up. */
    readonly tries?: number;
    readonly path?: string;
}

export interface Served {
    readonly port: number;
    readonly url: string;
    close(): Promise<void>;
}

export const MCP_PATH = "/procode/ask/mcp";

function listen(server: http.Server, port: number): Promise<boolean> {
    return new Promise((resolve, reject) => {
        const onError = (e: NodeJS.ErrnoException) => {
            server.off("listening", onListening);
            if (e.code === "EADDRINUSE" || e.code === "EACCES") resolve(false);
            else reject(e);
        };
        const onListening = () => {
            server.off("error", onError);
            resolve(true);
        };
        server.once("error", onError);
        server.once("listening", onListening);
        server.listen(port, "127.0.0.1");
    });
}

export async function serve(opts: ServeOptions): Promise<Served> {
    const path = opts.path ?? MCP_PATH;
    const inflight = new Map<string, AbortController>();
    let port = 0;

    const refuse = (res: http.ServerResponse, status: number, message: string, headers: http.OutgoingHttpHeaders = {}) => {
        res.writeHead(status, { "content-type": "text/plain; charset=utf-8", ...headers }).end(message + "\n");
    };

    const server = http.createServer((req, res) => {
        const host = req.headers.host ?? "";
        if (host !== `127.0.0.1:${port}` && host !== `localhost:${port}`) return refuse(res, 403, "ask: unknown host");
        if (req.headers.origin !== undefined) return refuse(res, 403, "ask: no browser requests");
        if ((req.url ?? "").split("?")[0] !== path) return refuse(res, 404, "ask: not found");
        if (req.method !== "POST") return refuse(res, 405, "ask: POST only", { allow: "POST" });
        if (!/^application\/json\b/i.test(req.headers["content-type"] ?? "")) return refuse(res, 415, "ask: application/json only");

        const chunks: Buffer[] = [];
        let size = 0;
        req.on("data", (c: Buffer) => {
            size += c.length;
            if (size > MAX_BODY) {
                refuse(res, 413, "ask: too large");
                req.destroy();
            } else chunks.push(c);
        });
        req.on("end", () => {
            if (res.writableEnded) return;
            let msg: unknown;
            try {
                msg = JSON.parse(Buffer.concat(chunks).toString("utf8"));
            } catch {
                return json(res, 400, { jsonrpc: "2.0", id: null, error: { code: -32700, message: "parse error" } });
            }
            if (typeof msg !== "object" || msg === null || Array.isArray(msg)) {
                return json(res, 400, { jsonrpc: "2.0", id: null, error: { code: -32600, message: "one JSON-RPC message per request" } });
            }
            void dispatch(msg as Json, req, res);
        });
    });

    function json(res: http.ServerResponse, status: number, body: unknown, headers: http.OutgoingHttpHeaders = {}): void {
        res.writeHead(status, { "content-type": "application/json", ...headers }).end(JSON.stringify(body));
    }

    async function dispatch(msg: Json, req: http.IncomingMessage, res: http.ServerResponse): Promise<void> {
        const session = String(req.headers["mcp-session-id"] ?? "");
        const id = msg["id"];
        if (id === undefined || id === null) {
            // A notification, or a response to nothing we asked.
            if (msg["method"] === "notifications/cancelled") {
                const target = ((msg["params"] ?? {}) as Json)["requestId"];
                inflight.get(`${session}\0${JSON.stringify(target)}`)?.abort();
            }
            res.writeHead(202).end();
            return;
        }
        if (msg["method"] !== "tools/call") {
            const out = await handle(msg, opts.ctx as Ctx);
            const headers = msg["method"] === "initialize" ? { "mcp-session-id": randomUUID() } : {};
            json(res, 200, out, headers);
            return;
        }
        // A tool call: a stream, so progress can go out while it waits.
        const key = `${session}\0${JSON.stringify(id)}`;
        const ac = new AbortController();
        inflight.set(key, ac);
        res.writeHead(200, { "content-type": "text/event-stream", "cache-control": "no-cache" });
        res.flushHeaders();
        const send = (m: unknown) => {
            if (!res.writableEnded) res.write(`data: ${JSON.stringify(m)}\n\n`);
        };
        res.on("close", () => {
            if (!res.writableEnded) ac.abort(); // the client hung up before the result
        });
        const out = await handle(msg, { ...(opts.ctx as Ctx), notify: send, signal: ac.signal });
        inflight.delete(key);
        if (out && !ac.signal.aborted) send(out);
        res.end();
    }

    const tries = opts.tries ?? 20;
    for (let i = 0; i < tries; i++) {
        const want = opts.port === 0 ? 0 : opts.port + i;
        if (await listen(server, want)) {
            port = (server.address() as { port: number }).port;
            return {
                port,
                url: `http://127.0.0.1:${port}${path}`,
                close: () =>
                    new Promise<void>((resolve) => {
                        for (const ac of inflight.values()) ac.abort();
                        server.closeAllConnections();
                        server.close(() => resolve());
                    }),
            };
        }
    }
    throw new Error(`ask: no free port from ${opts.port} to ${opts.port + tries - 1}`);
}

/** A port for a folder, the same every time it is opened, so the address
 *  written into its .mcp.json stays right across restarts. 40000–48999. */
export function portFor(folder: string): number {
    let h = 0x811c9dc5;
    for (const c of folder) h = Math.imul(h ^ c.charCodeAt(0), 0x01000193) >>> 0;
    return 40000 + (h % 9000);
}
