/* The wire between a Knowledge webview and the extension host.
 *
 * ONE `call` OVER A NAMED OPERATION, and the name is the whole vocabulary this
 * surface has. `OPERATIONS` is that list; `host.ts` has one table keyed on it,
 * and `guards.test.ts` checks the two agree — so an operation added to the UI
 * and not to the host is a test failure rather than a screen that renders
 * nothing and logs nothing.
 *
 * THE HOST IS THE ONLY SIDE THAT SPAWNS. `kb-js` starts a process, which a
 * browser document cannot do; the webview asks and the host answers. That is
 * also why the answers cross as plain JSON rather than as `kb-js` types with
 * methods on them: everything on this wire is `postMessage`d and survives only
 * as data.
 *
 * A REFUSAL AND A FAULT ARRIVE AS DIFFERENT MESSAGES, which is index-api.md
 * §10's three exit codes kept apart all the way to the screen. A `KbError` is
 * something the reader asked for that the store will not do and is shown with
 * its code; a `KbCrash` is a fault in kb or in this extension and is shown as
 * one. A UI that folded them together would tell a reader to fix input that
 * was never the problem.
 *
 * NO vscode IMPORT AND NO kb-js IMPORT. Both ends of the wire include this
 * file — the webview bundle and the extension host — so it has to be loadable
 * in a browser, and `kb-js` reaches for `node:child_process`.
 */

/* Every question the surface can ask. Read against index-ui.md: §2's two list
 * states, §3's read, §4's list and its two writes, and §3.1's per-document
 * refresh. */
export const OPERATIONS = [
    "status",
    "ls",
    "search",
    "get",
    "collections",
    "refresh",
    "refreshDocument",
    "source",
    "refreshSource",
    "renameCollection",
    "deleteCollection",
] as const;

export type Operation = (typeof OPERATIONS)[number];

export function isOperation(v: unknown): v is Operation {
    return typeof v === "string" && (OPERATIONS as readonly string[]).includes(v);
}

/* index-api.md §11's envelope as it crosses, plus the two things `kb-js`
 * computes from it: whether the code is one of §11's, and whether it is a code
 * nothing has heard of. Both are shown — a reader looking at an unrecognised
 * code is looking at a kb that has moved on without this extension. */
export interface WireError {
    code: string;
    message: string;
    spec: string | null;
    unrecognised: boolean;
    /* §11's details as the store sent them; plain JSON, so it crosses as is. */
    details: Readonly<Record<string, unknown>> | null;
}

/* ----------------------------------------------------- webview -> host */

export type Request =
    | { kind: "call"; id: number; op: Operation; input: unknown }
    /* Open a reference as a tab (§6). `chunk` is §3.2's "opening a search
     * result scrolls to the matching chunk's heading" — it is NOT part of the
     * URI, because `kb:/D-241?chunk=C-1` and `kb:/D-241` would be two
     * resources and therefore two tabs for one document, which is the failure
     * §6 names by hand. */
    | { kind: "open"; reference: string; chunk: string | null; preview: boolean }
    /* §3.1's source locator, "as a link that opens the original externally". */
    | { kind: "link"; href: string }
    /* §4: "a collection row opens the sidebar scoped to it." */
    | { kind: "scope"; collection: string }
    /* What the tab is called. A title read from the store rather than stored
     * on the tab is what keeps a renamed document from carrying a stale name
     * until it is closed — and the view is what has just read it, so it says
     * so instead of the host making a second call for one string. */
    | { kind: "title"; reference: string; title: string }
    | { kind: "notify"; level: "info" | "warning" | "error"; message: string }
    /* Pick files from disk and file them, into `collection` when a row asked. */
    | { kind: "addFiles"; collection: string | null }
    /* §4's delete, whose text has to tell the truth about what it does. */
    | { kind: "confirm"; id: number; title: string; detail: string; confirm: string };

/* ----------------------------------------------------- host -> webview */

export type Response =
    | { kind: "result"; id: number; value: unknown }
    | { kind: "failed"; id: number; error: WireError }
    | { kind: "crash"; id: number; message: string }
    | { kind: "confirmed"; id: number; confirmed: boolean }
    /* The store moved, or a write this extension made landed. There is no
     * subscription to a local store — `kb` is a process that starts and exits
     * — so this says only that something changed and every view re-asks. */
    | { kind: "changed" }
    /* §4's scoping, arriving at the sidebar. */
    | { kind: "scope"; collection: string }
    /* §3.2's scroll target, arriving at a document tab that is already open. */
    | { kind: "reveal"; chunk: string };

/* ------------------------------------------------------------- readers */

/* Both ends read a message off a `postMessage` event, where the payload is
 * whatever the other side sent and the type is a claim rather than a fact. */

export function isRequest(data: unknown): data is Request {
    if (typeof data !== "object" || data === null) {
        return false;
    }
    const kind = (data as Record<string, unknown>)["kind"];
    return (
        kind === "call" ||
        kind === "open" ||
        kind === "link" ||
        kind === "scope" ||
        kind === "title" ||
        kind === "notify" ||
        kind === "addFiles" ||
        kind === "confirm"
    );
}

export function isResponse(data: unknown): data is Response {
    if (typeof data !== "object" || data === null) {
        return false;
    }
    const kind = (data as Record<string, unknown>)["kind"];
    return (
        kind === "result" ||
        kind === "failed" ||
        kind === "crash" ||
        kind === "confirmed" ||
        kind === "changed" ||
        kind === "scope" ||
        kind === "reveal"
    );
}

/* --------------------------------------------------------- the view tag */

/* What a webview document is for. Written into the page by the host rather
 * than asked for over the wire, so a view never has a moment where it is
 * mounted and does not yet know what it is. */
export interface ViewTag {
    view: "sidebar" | "entity";
    /* `entity` only: the reference this tab is a place for. */
    reference?: string;
    /* Stylesheet and font URLs, already resolved through `asWebviewUri`. */
    styles: string[];
    codiconFont: string;
    /* §5's threshold, from the user's settings. A hit carries the store's own
     * verdict and this never overrides it (`kb-js`'s `staleOf`); it is for the
     * browse list, where the store has said nothing. */
    staleAfterDays: number;
}
