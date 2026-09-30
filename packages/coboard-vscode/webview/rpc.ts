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

export function open(id: string, newTab: boolean): void {
    send({ type: "open", id, newTab });
}

/* The mouse handlers of anything that opens an item: a click shows it in this
 * tab, ⌘-click (Ctrl-click off macOS) and the middle button in a new one.
 * The middle button's press is stopped too, or it starts autoscrolling. */
type Press = { readonly button: number; readonly metaKey: boolean; readonly ctrlKey: boolean; preventDefault(): void };
export function opener(id: string): {
    onClick(e: Press): void;
    onAuxClick(e: Press): void;
    onMouseDown(e: Press): void;
} {
    return {
        onClick: (e) => {
            e.preventDefault();
            open(id, e.metaKey || e.ctrlKey);
        },
        onAuxClick: (e) => {
            if (e.button !== 1) return;
            e.preventDefault();
            open(id, true);
        },
        onMouseDown: (e) => {
            if (e.button === 1) e.preventDefault();
        },
    };
}
