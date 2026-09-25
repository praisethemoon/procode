/* Two streams, one server, and the order things happen in.
 *
 * ONE MESSAGE AT A TIME, IN ARRIVAL ORDER. JSON-RPC permits answering out of
 * order and a client matches responses by id, so this is not correctness — it
 * is about the store underneath. A `Kb` call starts a `kb` process; two writes
 * running at once are two processes contending for one store's lock, and §11
 * has `store_locked` for what the loser gets. An agent that sent two `kb_add`
 * calls without waiting would get a refusal caused by nothing but this
 * server's own concurrency, which is a failure it cannot fix and cannot
 * understand. The queue is a promise chain and is the whole of the mechanism.
 *
 * STDOUT CARRIES MESSAGES AND NOTHING ELSE. A stray `console.log` anywhere in
 * the process writes a line into the middle of the protocol, and what the
 * client does with it ranges from ignoring it to closing the session; what it
 * never does is tell anybody which module printed. So the output stream is an
 * argument rather than a global, `guards.test.ts` refuses `console.log` and
 * `process.stdout.write` by name across `src/`, and diagnostics go to stderr.
 *
 * A FRAMING FAILURE IS FATAL AND SAYS SO. Once a message longer than the cap
 * has arrived without a newline, the stream cannot be resynchronised — there is
 * no boundary left to find — so reading stops. Carrying on would mean parsing
 * the tail of somebody's document as if it were a request.
 */

import { Readable, Writable } from "node:stream";

import { FramingError, LineReader, encode } from "./framing";
import { Dispatch, handle } from "./jsonrpc";

export interface ServeOptions {
    /* What one message may weigh; `framing.ts` explains the number. */
    maxLineBytes?: number;
    /* Where a fault that cannot be answered on the wire goes. Defaults to
     * stderr, which is the one channel a stdio server can talk on without
     * corrupting the other. */
    onError?(error: Error): void;
}

/* Runs until the input ends and everything it carried has been answered. */
export function serve(
    input: Readable,
    output: Writable,
    dispatch: Dispatch,
    options: ServeOptions = {},
): Promise<void> {
    const reader = new LineReader(options.maxLineBytes);
    const report =
        options.onError ??
        ((error: Error): void => {
            process.stderr.write(`kb-mcp: ${error.message}\n`);
        });

    return new Promise<void>((resolve) => {
        /* The tail of the chain: every message waits for the one before it,
         * and `end` waits for all of them. */
        let queue: Promise<void> = Promise.resolve();
        let ended = false;

        const take = (line: string): void => {
            queue = queue.then(async () => {
                try {
                    const response = await handle(line, dispatch);
                    if (response !== null) {
                        output.write(encode(response));
                    }
                } catch (e) {
                    /* `handle` turns a handler's failure into an error
                     * response, so reaching here means the response itself
                     * could not be written — a message that will not encode,
                     * or a closed pipe. There is nowhere to answer. */
                    report(e instanceof Error ? e : new Error(String(e)));
                }
            });
        };

        const finish = (): void => {
            if (ended) {
                return;
            }
            ended = true;
            queue.then(() => resolve(), () => resolve());
        };

        input.on("data", (chunk: Buffer) => {
            let lines: string[];
            try {
                lines = reader.push(chunk);
            } catch (e) {
                report(e instanceof FramingError ? e : new Error(String(e)));
                /* Nothing after this point can be trusted to be a message. */
                input.destroy();
                finish();
                return;
            }
            for (const line of lines) {
                take(line);
            }
        });

        input.on("end", () => {
            for (const line of reader.end()) {
                take(line);
            }
            finish();
        });

        input.on("close", finish);
        input.on("error", (e: Error) => {
            report(e);
            finish();
        });
    });
}
