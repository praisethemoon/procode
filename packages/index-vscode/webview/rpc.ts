/* The webview half of the wire.
 *
 * `acquireVsCodeApi` is callable exactly once per document, so the handle is
 * taken here and nowhere else.
 *
 * A REFUSAL AND A FAULT REJECT WITH DIFFERENT THINGS, which is index-api.md
 * §10's three exit codes kept apart all the way to the screen. `StoreRefusal`
 * carries §11's code and is something the reader asked for that the store will
 * not do; `HostFault` is a bug in kb or in this extension. A UI that caught one
 * type would tell a reader to fix input that was never the problem.
 */

import { CannotStart, Operation, Request, Response, StoredPositions, ViewTag, WireError, isResponse } from "../src/protocol";

interface VsCodeApi {
    postMessage(message: unknown): void;
    getState(): unknown;
    setState(state: unknown): void;
}

declare function acquireVsCodeApi(): VsCodeApi;
declare const __KNOWLEDGE__: ViewTag;

export const tag: ViewTag = __KNOWLEDGE__;

const api: VsCodeApi = acquireVsCodeApi();

export class StoreRefusal extends Error {
    readonly error: WireError;
    constructor(error: WireError) {
        super(error.message);
        this.name = "StoreRefusal";
        this.error = error;
    }
}

export class HostFault extends Error {
    /* Set when kb never started, with why, and the command it was started as. */
    readonly cannotStart: CannotStart | null;
    readonly command: string | null;
    constructor(message: string, cannotStart: CannotStart | null = null, command: string | null = null) {
        super(message);
        this.name = "HostFault";
        this.cannotStart = cannotStart;
        this.command = command;
    }
}

type Pending = { resolve(value: unknown): void; reject(reason: unknown): void };

const pending = new Map<number, Pending>();
let nextId = 1;
const listeners = new Set<(r: Response) => void>();

window.addEventListener("message", (ev: MessageEvent) => {
    const data: unknown = ev.data;
    if (!isResponse(data)) {
        return;
    }
    if (data.kind === "changed" || data.kind === "scope" || data.kind === "reveal") {
        for (const l of listeners) {
            l(data);
        }
        return;
    }
    const p = pending.get(data.id);
    if (p === undefined) {
        return;
    }
    pending.delete(data.id);
    if (data.kind === "failed") {
        p.reject(new StoreRefusal(data.error));
    } else if (data.kind === "crash") {
        p.reject(new HostFault(data.message, data.cannotStart ?? null, data.command ?? null));
    } else if (data.kind === "result") {
        p.resolve(data.value);
    } else {
        p.resolve(data.confirmed);
    }
});

function send(request: Request): void {
    api.postMessage(request);
}

function ask<T>(build: (id: number) => Request): Promise<T> {
    const id = nextId++;
    return new Promise<T>((resolve, reject) => {
        pending.set(id, { resolve: resolve as (v: unknown) => void, reject });
        send(build(id));
    });
}

/* One operation. The name is `protocol.ts`'s and this client knows no others
 * — a typo does not compile. */
export function call<T = unknown>(op: Operation, input: unknown = {}): Promise<T> {
    return ask<T>((id) => ({ kind: "call", id, op, input }));
}

export function confirm(title: string, detail: string, confirmLabel: string): Promise<boolean> {
    return ask<boolean>((id) => ({ kind: "confirm", id, title, detail, confirm: confirmLabel }));
}

/* §6's open, with §3.2's scroll target when the row came from a search. */
export function open(reference: string, chunk: string | null = null, preview = false): void {
    send({ kind: "open", reference, chunk, preview });
}

/* §3.1's locator, "as a link that opens the original externally". The host
 * checks the scheme and confirms before the system handler sees it. */
export function link(href: string): void {
    send({ kind: "link", href });
}

/* §4: "a collection row opens the sidebar scoped to it." */
export function addFiles(collection: string | null = null): void {
    send({ kind: "addFiles", collection });
}

/* A folder filed whole, into the row's collection when a row asked. */
export function addFolder(collection: string | null = null): void {
    send({ kind: "addFolder", collection });
}

/* The ways out of "no knowledge base here" and "kb can't be found". */
export function initStore(): void {
    send({ kind: "init" });
}

/* Embed what filings left to embed, through the host's command. */
export function finishEmbedding(): void {
    send({ kind: "embed" });
}

export function openCliSetting(): void {
    send({ kind: "settings" });
}

export function scope(collection: string): void {
    send({ kind: "scope", collection });
}

export function setTitle(reference: string, title: string): void {
    send({ kind: "title", reference, title });
}

export function notify(level: "info" | "warning" | "error", message: string): void {
    send({ kind: "notify", level, message });
}

/* The graph's settled positions, kept by the host under a `layoutKey`. */
export function cachedLayout(key: string): Promise<StoredPositions | null> {
    return ask<StoredPositions | null>((id) => ({ kind: "layoutGet", id, key }));
}

export function rememberLayout(key: string, positions: StoredPositions): void {
    send({ kind: "layoutPut", key, positions });
}

/* Every view re-asks its own questions when the store moves. There is nothing
 * to subscribe to — `kb` is a process that starts and exits — so what arrives
 * is only that something changed. */
export function onHostEvent(fn: (r: Response) => void): () => void {
    listeners.add(fn);
    return () => {
        listeners.delete(fn);
    };
}

export function isRefusal(e: unknown): e is StoreRefusal {
    return e instanceof StoreRefusal;
}
