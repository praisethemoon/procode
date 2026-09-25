/* The wire to the extension host. */

import type { ToHost, ToView } from "../src/protocol";

declare function acquireVsCodeApi(): { postMessage(m: unknown): void };

const vscode = acquireVsCodeApi();

export function send(m: ToHost): void {
    vscode.postMessage(m);
}

export function listen(fn: (m: ToView) => void): () => void {
    const handler = (e: MessageEvent) => fn(e.data as ToView);
    window.addEventListener("message", handler);
    return () => window.removeEventListener("message", handler);
}

export function open(id: string): void {
    send({ type: "open", id });
}
