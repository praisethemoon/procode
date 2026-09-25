/* How a message starts and stops on a pipe.
 *
 * NEWLINE-DELIMITED JSON, NOT `Content-Length` HEADERS. MCP's stdio transport
 * frames one JSON-RPC message per line; LSP frames with headers, the two look
 * alike from a distance, and a server that picks the wrong one does not fail
 * loudly — it reads nothing, answers nothing, and the client waits. That is the
 * failure this file exists to make impossible to introduce quietly, so the rule
 * is stated in both directions: a message NEVER contains a newline, and a line
 * is ALWAYS a whole message.
 *
 * A MESSAGE ARRIVES IN AS MANY PIECES AS THE KERNEL FEELS LIKE. `data` on a
 * pipe is not a message boundary and never was: one read can carry half a
 * request, three requests, or two and a half. The bug is invisible on a
 * loopback where every write fits in one buffer and appears the first time a
 * caller sends a document worth filing. So the reader buffers, and the split
 * tests drive the split at every offset rather than at a plausible one.
 *
 * AND THE BYTES CAN SPLIT INSIDE A CHARACTER. `chunk.toString("utf8")` on a
 * boundary that falls between the two bytes of a `é` yields a replacement
 * character, and the message that comes back out is not the one that went in —
 * which for a knowledge base full of quoted prose is silent corruption of the
 * thing being filed. `StringDecoder` holds the incomplete sequence until the
 * rest of it arrives, and that is the whole reason it is here.
 *
 * NOTHING IN THIS FILE KNOWS WHAT JSON-RPC IS. It moves lines. What a line
 * means is `jsonrpc.ts`, and keeping the two apart is what lets the framing be
 * tested against bytes that are not valid JSON at all.
 */

import { StringDecoder } from "node:string_decoder";

export class FramingError extends Error {
    constructor(message: string) {
        super(message);
        this.name = "FramingError";
    }
}

/* What one message may weigh. A peer that opens the pipe and writes for ever
 * without a newline is an out-of-memory condition rather than a slow client,
 * and `buffer += chunk` has no opinion about which it is looking at.
 *
 * The number is generous on purpose: `kb_add` carries a document's whole text
 * through here, and a cap that refused a large page would be a cap that turns
 * a legitimate call into a dead connection. It is a runaway detector, not a
 * quota. */
export const DEFAULT_MAX_LINE_BYTES = 64 * 1024 * 1024;

export class LineReader {
    private readonly decoder = new StringDecoder("utf8");
    private pending = "";
    /* Bytes of `pending`, kept as a counter rather than measured. Measuring it
     * per chunk is O(pending) and turns one slow peer into quadratic work; the
     * counter is exact because the only two things that change `pending` are a
     * chunk arriving and a line leaving, and both are accounted for. */
    private bytes = 0;

    constructor(private readonly maxBytes: number = DEFAULT_MAX_LINE_BYTES) {}

    /* The complete messages in what has arrived so far, and nothing else. */
    push(chunk: Buffer): string[] {
        this.bytes += chunk.length;
        this.pending += this.decoder.write(chunk);
        if (!this.pending.includes("\n")) {
            this.guard();
            return [];
        }
        const parts = this.pending.split("\n");
        /* The last piece is whatever came after the last newline, which is the
         * beginning of the next message or nothing at all. */
        this.pending = parts[parts.length - 1];
        this.bytes = Buffer.byteLength(this.pending, "utf8");
        this.guard();
        return parts.slice(0, -1).map(clean).filter(isMessage);
    }

    /* The stream ended. A last message with no newline after it is still a
     * message: the peer closed the pipe, so no more of it is coming, and
     * dropping it would answer a well-formed request with silence. If it was
     * truncated instead, it fails to parse and the caller is told that — which
     * is the more useful of the two wrong answers. */
    end(): string[] {
        this.pending += this.decoder.end();
        const last = clean(this.pending);
        this.pending = "";
        this.bytes = 0;
        return isMessage(last) ? [last] : [];
    }

    private guard(): void {
        if (this.bytes > this.maxBytes) {
            throw new FramingError(
                `a message longer than ${this.maxBytes} bytes arrived without a newline, so it is either a runaway or a peer that is not framing at all.`,
            );
        }
    }
}

/* A `\r` before the newline is a peer writing CRLF. Tolerated on the way in —
 * it costs one character to accept and a client that does it is otherwise
 * perfectly well behaved — and never written on the way out. */
function clean(line: string): string {
    return line.endsWith("\r") ? line.slice(0, -1) : line;
}

/* A blank line is not a message. Some peers write one as a keepalive, and
 * answering it with a parse error would be a server arguing with punctuation. */
function isMessage(line: string): boolean {
    return line.trim().length > 0;
}

/* One message, as the bytes that go on the wire.
 *
 * THE NEWLINE CHECK IS NOT PARANOIA ABOUT `JSON.stringify`. That function
 * escapes every newline inside a string, so a payload cannot smuggle one
 * through; what it cannot stop is somebody deciding the output would be easier
 * to read indented. `JSON.stringify(m, null, 2)` here is a one-character edit
 * that breaks every client and passes every test that does not look at the
 * bytes. This is the test that looks at the bytes. */
export function encode(message: unknown): string {
    const line = JSON.stringify(message);
    if (typeof line !== "string") {
        throw new FramingError("a message that JSON cannot represent cannot be sent.");
    }
    if (line.includes("\n")) {
        throw new FramingError(
            "a message with a newline in it would be read as two messages by the other end.",
        );
    }
    return `${line}\n`;
}
