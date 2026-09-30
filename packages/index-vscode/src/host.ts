/* The extension host's half of the wire: what a Knowledge webview document is,
 * and what happens to every message that comes out of one.
 *
 * ONE PLUMBING FOR TWO SURFACES. The sidebar and a `kb:` tab are the same
 * document with a different `ViewTag`, so they share their HTML, their resource
 * roots and their message handler. Two copies of this would be two places for
 * the CSP to drift.
 *
 * ONE TABLE FOR THE OPERATIONS, AND IT IS THE WHOLE VOCABULARY. `protocol.ts`
 * lists what the surface may ask; this answers each of them with one `kb-js`
 * call and nothing else. There is no place here to filter a list the store
 * already filtered, or to compute a field the store emits —
 * index-api.md §10 makes the C the only implementation, and a convenience in
 * this file is the second one arriving a line at a time.
 *
 * THE WEBVIEW IS TRUSTED AND THE DOCUMENTS ARE NOT. Everything in this file
 * talks to this package's own React app, which it built. The pages in the store
 * are somebody else's, and they never become markup anywhere: `view/html.ts`
 * turns HTML into a node tree and React makes elements of it, so there is no
 * `srcdoc`, no frame and no `innerHTML` — which is why this file has no airlock
 * and no second CSP, and why it can afford a nonce on `script-src`.
 */

import * as fs from "node:fs";

import * as vscode from "vscode";

import { Kb, KbCrash, KbDirAdded, KbError, KbSourceRefreshed, isKbCrash, isKbError } from "kb-js";

import { refreshDocument } from "./commands";
import { KNOWLEDGE_STYLESHEETS, knowledgePolicy } from "./policy";
import { Operation, Request, Response, StoredPositions, ViewTag, isRequest } from "./protocol";
import { Settings } from "./session";
import { linkTarget } from "./view/locator";

/* A CSP nonce, which is the whole of why this package's own script may run and
 * an injected one may not. 128 bits of randomness spelled in hex. */
function nonce(): string {
    const bytes = new Uint8Array(16);
    /* `crypto` is global in every Node this extension can run on; the import
     * is avoided so this file has one fewer thing to mock. */
    globalThis.crypto.getRandomValues(bytes);
    return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

/* JSON that is safe inside a `<script>` element. `</script` ends the element
 * wherever it appears — inside a string literal included — and `<!--` opens a
 * comment that swallows the rest. Both are escaped at the character level, so
 * the value the page parses is identical. */
function embeddedJson(value: unknown): string {
    return JSON.stringify(value).replace(
        /[<>\u2028\u2029]/g,
        (c) => `\\u${c.charCodeAt(0).toString(16).padStart(4, "0")}`,
    );
}

export interface HostContext {
    extensionUri: vscode.Uri;
    client(): Kb;
    settings(): Settings;
    /* Something this extension wrote landed, or the reader asked for a
     * refresh. Every open view re-asks its own questions. */
    announce(): void;
    /* §6: opening a reference, with §3.2's scroll target when there is one. */
    open(reference: string, chunk: string | null, preview: boolean): void;
    /* §4: "a collection row opens the sidebar scoped to it." */
    scope(collection: string): void;
    /* The sidebar's search text, as it settles; the search page starts from it. */
    typed(q: string): void;
    /* A tab's name, from the view that has just read the document. */
    retitle(reference: string, title: string): void;
    /* Settled graph layouts, by `layoutKey`. */
    layouts: LayoutShelf;
}

export interface LayoutShelf {
    get(key: string): StoredPositions | null;
    put(key: string, positions: StoredPositions): void;
}

/* How many layouts are kept: the whole store, a collection or two, and the
 * same with unlinked documents shown. Two thousand positions are some 60 KB
 * of JSON, so the shelf stays small. */
const LAYOUTS_KEPT = 6;
const LAYOUTS_STATE = "knowledge.graphLayouts";

/* The graph's settled positions in the workspace's state, most recent first,
 * so they outlive the tab — and the window — while the store they were
 * computed from is unchanged. A changed store has a different key and simply
 * misses. */
export function layoutShelf(state: vscode.Memento): LayoutShelf {
    type Kept = { key: string; positions: StoredPositions };
    const all = (): Kept[] => {
        const v = state.get<unknown>(LAYOUTS_STATE);
        return Array.isArray(v) ? (v as Kept[]) : [];
    };
    return {
        get: (key) => all().find((l) => l.key === key)?.positions ?? null,
        put: (key, positions) => {
            const rest = all().filter((l) => l.key !== key);
            void state.update(LAYOUTS_STATE, [{ key, positions }, ...rest].slice(0, LAYOUTS_KEPT));
        },
    };
}

export function mediaUri(ctx: HostContext): vscode.Uri {
    return vscode.Uri.joinPath(ctx.extensionUri, "out", "media");
}

/* Everything a webview is allowed to load from disk: `out/media`, which is
 * this package's stylesheets, its bundle and the codicon font. The store is
 * NOT a resource root — nothing in a document is loaded by URL, because every
 * byte of it comes back through the wire as data the React app renders. */
export function resourceRoots(ctx: HostContext): vscode.Uri[] {
    return [mediaUri(ctx)];
}

export function webviewOptions(ctx: HostContext): vscode.WebviewOptions {
    return { enableScripts: true, localResourceRoots: resourceRoots(ctx) };
}

export function webviewHtml(
    ctx: HostContext,
    webview: vscode.Webview,
    tag: Omit<ViewTag, "styles" | "codiconFont" | "staleAfterDays">,
): string {
    const media = mediaUri(ctx);
    const uri = (name: string): string =>
        webview.asWebviewUri(vscode.Uri.joinPath(media, name)).toString();
    const full: ViewTag = {
        ...tag,
        styles: KNOWLEDGE_STYLESHEETS.map(uri),
        codiconFont: uri("codicon.ttf"),
        staleAfterDays: ctx.settings().staleAfterDays,
    };
    const n = nonce();
    return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="${knowledgePolicy(webview.cspSource, n)}">
<meta name="viewport" content="width=device-width, initial-scale=1">
${full.styles.map((s) => `<link rel="stylesheet" href="${s}">`).join("\n")}
</head>
<body>
<div id="root"></div>
<script nonce="${n}">window.__KNOWLEDGE__ = ${embeddedJson(full)};</script>
<script nonce="${n}" src="${uri("knowledge-webview.js")}"></script>
</body>
</html>`;
}

/* ------------------------------------------------------- the message pump */

/* One `kb-js` call per operation, and that is the entire table.
 *
 * `ls` AND `search` ARE SEPARATE OPERATIONS AND NOT ONE WITH AN OPTIONAL
 * QUERY. §2's two list states are two different routes over two different
 * shapes — documents and hits — and a single operation that branched on
 * whether the query was empty would put §2's "browsing is the default state"
 * inside an `if` in this file, where nothing could check it. It lives in the
 * view instead, in a function with a test.
 *
 * THE INPUT IS READ FIELD BY FIELD RATHER THAN SPREAD. It arrives from a
 * webview through `postMessage` and is a claim rather than a fact; spreading it
 * into a `kb-js` options object would let a field this table has never heard of
 * reach the argv builder. */
async function perform(kb: Kb, op: Operation, raw: unknown, settings: Settings): Promise<unknown> {
    const input = typeof raw === "object" && raw !== null ? (raw as Record<string, unknown>) : {};
    const text = (key: string): string | undefined =>
        typeof input[key] === "string" ? (input[key] as string) : undefined;
    const count = (key: string): number | undefined =>
        typeof input[key] === "number" ? (input[key] as number) : undefined;

    switch (op) {
        case "status":
            return kb.status();
        case "ls":
            return kb.ls({
                collection: text("collection"),
                source: text("source"),
                mime: text("mime"),
                since: text("since"),
                limit: count("limit"),
                after: text("after"),
                reverse: input["reverse"] === true,
            });
        case "search":
            /* `rerank` is the reader's setting and not the webview's to ask
             * for: the same search from the sidebar and the picker. */
            return kb.search(text("q") ?? "", {
                collection: text("collection"),
                k: count("k"),
                rerank: settings.rerank === true,
            });
        case "get":
            return kb.get(text("id") ?? "", { text: true, chunks: true });
        case "collections":
            return kb.collections();
        case "graph": {
            /* Every document once and every link once: the graph is drawn
             * from the two, rather than one `links` call per document. */
            const [documents, edges] = await Promise.all([kb.ls(), kb.allLinks()]);
            return { documents, edges };
        }
        case "refresh":
            /* §5's route takes a scope and a threshold and nothing else: a
             * report over the store. One document's refresh is its own
             * operation, below, because it reads that document's source. */
            return kb.refresh({
                collection: text("collection"),
                olderThan: text("olderThan"),
            });
        case "refreshDocument":
            return refreshDocument(kb, text("id") ?? "");
        case "source":
            return kb.source(text("id") ?? "");
        case "refreshSource":
            return refreshSourceHere(kb, text("id") ?? "");
        case "renameCollection":
            return kb.renameCollection(text("from") ?? "", text("to") ?? "");
        default:
            return kb.deleteCollection(text("name") ?? "", { withDocuments: input["withDocuments"] === true });
    }
}

export interface Surface {
    webview: vscode.Webview;
}

export function handleRequest(ctx: HostContext, surface: Surface, raw: unknown): void {
    if (!isRequest(raw)) {
        return;
    }
    const request: Request = raw;
    switch (request.kind) {
        case "call":
            void answer(ctx, surface.webview, request.id, request.op, request.input);
            return;
        case "open":
            ctx.open(request.reference, request.chunk, request.preview);
            return;
        case "link":
            openLink(request.href);
            return;
        case "scope":
            ctx.scope(request.collection);
            return;
        case "typed":
            ctx.typed(request.q);
            return;
        case "title":
            ctx.retitle(request.reference, request.title);
            return;
        case "notify":
            notify(request.level, request.message);
            return;
        case "addFiles":
            void vscode.commands.executeCommand("knowledge.addFiles", request.collection ?? undefined);
            return;
        case "addFolder":
            void vscode.commands.executeCommand("knowledge.addFolder", request.collection ?? undefined);
            return;
        case "init":
            void vscode.commands.executeCommand("knowledge.init");
            return;
        case "settings":
            void vscode.commands.executeCommand("workbench.action.openSettings", "knowledge.cliPath");
            return;
        case "embed":
            void vscode.commands.executeCommand("knowledge.embed");
            return;
        case "layoutGet":
            reply(surface.webview, { kind: "result", id: request.id, value: ctx.layouts.get(String(request.key)) });
            return;
        case "layoutPut":
            if (typeof request.key === "string" && Array.isArray(request.positions)) {
                ctx.layouts.put(request.key, request.positions);
            }
            return;
        case "saveAs":
            void vscode.commands.executeCommand("knowledge.saveDocumentAs", request.reference);
            return;
        case "find":
            /* VS Code's own find widget, which the editor enables. */
            void vscode.commands.executeCommand("editor.action.webvieweditor.showFind");
            return;
        default:
            void vscode.window
                .showWarningMessage(
                    request.title,
                    { modal: true, detail: request.detail },
                    request.confirm,
                )
                .then((chosen) =>
                    reply(surface.webview, {
                        kind: "confirmed",
                        id: request.id,
                        confirmed: chosen === request.confirm,
                    }),
                );
    }
}

function reply(webview: vscode.Webview, response: Response): void {
    void webview.postMessage(response);
}

/* index-api.md §10's three exit codes, kept apart all the way to the screen. */
async function answer(
    ctx: HostContext,
    webview: vscode.Webview,
    id: number,
    op: Operation,
    input: unknown,
): Promise<void> {
    try {
        reply(webview, { kind: "result", id, value: await perform(ctx.client(), op, input, ctx.settings()) });
        /* A write is the only thing that can have changed the store under the
         * other views, and it is the only thing that announces. A read that
         * announced would put every view into a loop of re-asking. */
        if (
            op === "refresh" ||
            op === "refreshDocument" ||
            op === "refreshSource" ||
            op === "renameCollection" ||
            op === "deleteCollection"
        ) {
            ctx.announce();
        }
    } catch (e) {
        const out = failure(id, e);
        if (out.kind === "failed" && out.error.code === "not_found" && (await noStore(ctx))) {
            reply(webview, { ...out, error: { ...out.error, noStore: true } });
        } else if (out.kind === "crash" && out.cannotStart === "binary") {
            reply(webview, { ...out, command: ctx.settings().cliPath });
        } else {
            reply(webview, out);
        }
    }
}

/* Whether the store itself is what is missing: `not_found` is also the
 * answer for a document that is not in one, and that is a plain refusal. */
async function noStore(ctx: HostContext): Promise<boolean> {
    try {
        return !(await ctx.client().status()).present;
    } catch {
        return false; // no folder, or kb cannot say: leave the refusal as it is
    }
}

export function failure(id: number, e: unknown): Response {
    if (isKbError(e)) {
        const error: KbError = e;
        return {
            kind: "failed",
            id,
            error: {
                code: error.code,
                message: error.message,
                spec: error.spec,
                unrecognised: error.unrecognised,
                details: error.details,
            },
        };
    }
    if (isKbCrash(e)) {
        const crash: KbCrash = e;
        /* stderr is the only place left to look when the payload was
         * unreadable, so it travels with the message rather than being
         * dropped for tidiness. */
        return {
            kind: "crash",
            id,
            message: crash.stderr.length > 0 ? `${crash.message}\n${crash.stderr}` : crash.message,
            ...(crash.cannotStart !== null ? { cannotStart: crash.cannotStart } : {}),
        };
    }
    return { kind: "crash", id, message: e instanceof Error ? `${e.name}: ${e.message}` : String(e) };
}

/* §3.1's source locator, as a link that opens the original.
 *
 * A LOCAL ORIGINAL OPENS IN VS CODE, without asking: it opens a file for
 * reading and runs nothing. One that is gone is said to be gone.
 *
 * A WEB ONE OPENS BEHIND A CONFIRMATION, AND THE CONFIRMATION NAMES THE HOST.
 * The locator was written by whatever filed the document, which may have been
 * an agent, and `vscode.env.openExternal` hands a URL to the system handler —
 * where a scheme that is not `http` can be an application launch. So the
 * scheme is checked (`view/locator.ts`) and the reader is shown where they
 * are about to go. */
function openLink(href: string): void {
    const target = linkTarget(href, (p) => fs.existsSync(p));
    if (target.kind === "file") {
        void vscode.commands.executeCommand("vscode.open", vscode.Uri.file(target.path));
        return;
    }
    if (target.kind === "missing") {
        void vscode.window.showWarningMessage(
            `The original is no longer at ${target.path}; the copy kb stored is what you are reading.`,
        );
        return;
    }
    if (target.kind === "refused") {
        void vscode.window.showWarningMessage(
            `Knowledge did not open that link. Only local files and http, https and mailto locators open, and this one is ${describeScheme(target.href)}.`,
        );
        return;
    }
    const url = target.url;
    let parsed: vscode.Uri;
    try {
        parsed = vscode.Uri.parse(url, true);
    } catch {
        void vscode.window.showWarningMessage(`Knowledge could not read that locator: ${url}`);
        return;
    }
    void vscode.window
        .showWarningMessage(
            `Open ${parsed.authority.length > 0 ? parsed.authority : parsed.scheme} in your browser?`,
            { modal: true, detail: url },
            "Open",
        )
        .then((chosen) => {
            if (chosen === "Open") {
                void vscode.env.openExternal(parsed);
            }
        });
}

function describeScheme(url: string): string {
    const m = /^([a-zA-Z][a-zA-Z0-9+.-]*):/.exec(url);
    return m === null ? "not a URL at all" : `a ${m[1]}: URL`;
}

function notify(level: "info" | "warning" | "error", message: string): void {
    if (level === "error") {
        void vscode.window.showErrorMessage(message);
    } else if (level === "warning") {
        void vscode.window.showWarningMessage(message);
    } else {
        void vscode.window.showInformationMessage(message);
    }
}

export function broadcast(webviews: Iterable<vscode.Webview>, response: Response): void {
    for (const w of webviews) {
        void w.postMessage(response);
    }
}

/* A source's Refresh. A file is read again by kb itself; a url is fetched
 * here, because kb has no network (index-api §12.2) — through the same
 * document refresh, conditional on the ETag, answered in the source's shape.
 * A folder is walked again (§2.1), with forgetting on as `kb refresh` has it,
 * and answers in the folder's own shape: there is no one document to name. */
async function refreshSourceHere(kb: Kb, id: string): Promise<KbSourceRefreshed | KbDirAdded> {
    const read = await kb.source(id);
    if (read.source.kind === "dir") {
        return kb.addDir(read.source.locator, { collection: read.source.collection });
    }
    if (read.source.kind !== "url") {
        return kb.refreshSource(id);
    }
    const doc = read.documents[0];
    if (doc === undefined) {
        throw new Error(`${id} holds no document to refresh.`);
    }
    const r = await refreshDocument(kb, doc.id);
    if (r.outcome === "declined") {
        throw new Error(`${id} was not refreshed.`);
    }
    if (r.outcome === "cannot") {
        throw new Error(`${id} cannot be refreshed: ${r.why}`);
    }
    const filed = (await kb.get(r.document)).document;
    return {
        action: "refresh",
        source: id,
        document: r.document,
        changed: r.outcome === "updated",
        contentHash: filed.contentHash,
        fetchedAt: r.fetchedAt,
    };
}
