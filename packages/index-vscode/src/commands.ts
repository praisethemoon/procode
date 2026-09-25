/* index-ui.md §2's three title-bar actions, and §3.1's per-document refresh.
 *
 * THESE ARE THE ONLY WRITES THE SURFACE MAKES, and every one of them goes
 * through the CLI — index-api.md §10 makes the C the only code that writes, and
 * a second writer would be a second thing that decides what a content hash
 * covers. §4's rename and delete are the other two and live in the collections
 * tab, where §4 says they live.
 *
 * "ADD A URL" FETCHES HERE, AND THAT IS index-api.md §12.2'S OWN ALTERNATIVE
 * RATHER THAN A DEPARTURE FROM IT. §12.2 leaves fetching unresolved and states
 * the two options: an HTTP client in the CLI, or "fetch nothing and accept
 * content only through `POST /documents`, keeping the binary free of TLS
 * entirely". The CLI as built takes the second — `kb add` reads its content
 * from a file or from stdin and has no network at all — so a caller that wants
 * a page filed is the one that has to fetch it. §2 of the UI spec asks for the
 * action, so this is the caller.
 *
 * AND IT ASKS FIRST. A title-bar button that silently makes a network request
 * is a surprise in a surface whose whole promise is that it reads a local
 * store. The confirmation names the host, because that is the fact somebody
 * deciding needs.
 */

import * as path from "node:path";
import * as vscode from "vscode";

import { Kb, KbError, isKbError } from "kb-js";

/* VSCode's language identifiers, mapped onto the mimes `kb add` files under.
 *
 * WHY THIS EXISTS AT ALL. `kb add` guesses a mime from the locator's
 * extension, and a locator often has none — a URL ending `/win32/iocp`, a file
 * called `Makefile`. A document filed as `text/plain` when it is markdown
 * renders in §3.2 as a wall of unformatted text with its own hashes in it, and
 * nothing anywhere says why. The editor already knows what the language is, so
 * the caller that knows is the caller that says.
 *
 * Every mime here is one `view/mime.ts` has a rendering for, which
 * `guards.test.ts` checks: a mime this map can produce and that map cannot
 * place would be a source file rendered as prose. */
const MIME_OF_LANGUAGE: Readonly<Record<string, string>> = {
    markdown: "text/markdown",
    html: "text/html",
    plaintext: "text/plain",
    json: "application/json",
    jsonc: "application/json",
    javascript: "text/javascript",
    javascriptreact: "text/javascript",
    typescript: "application/typescript",
    typescriptreact: "application/typescript",
    c: "text/x-c",
    cpp: "text/x-c++",
    csharp: "text/x-csharp",
    java: "text/x-java",
    go: "text/x-go",
    rust: "text/x-rust",
    python: "text/x-python",
    ruby: "text/x-ruby",
    shellscript: "application/x-sh",
    sql: "text/x-sql",
    css: "text/x-css",
    yaml: "text/x-yaml",
    typec: "text/x-typec",
};

export function mimeForLanguage(languageId: string): string | undefined {
    return MIME_OF_LANGUAGE[languageId];
}

/* §11 rendered for a person. The code is shown because a reader looking at
 * `model_mismatch` is looking at a store that needs reindexing, and the word is
 * the thing they can search for. */
function report(e: unknown, what: string): void {
    if (isKbError(e)) {
        const error: KbError = e;
        /* The CLI answers a command it does not have with `usage` or
         * `unknown_command`, and several routes index-ui.md draws affordances
         * for are not built yet (index-api.md §5's refresh, §7's collection
         * writes). Saying "this build of kb cannot do it" is a different
         * sentence from "the store refused", and a reader acts on them
         * differently. */
        if (error.code === "usage" || error.code === "unknown_command") {
            void vscode.window.showWarningMessage(
                `This build of kb cannot ${what} yet: ${error.message}`,
            );
            return;
        }
        void vscode.window.showErrorMessage(`${error.message} (${error.code})`);
        return;
    }
    void vscode.window.showErrorMessage(
        `Knowledge could not ${what}: ${e instanceof Error ? e.message : String(e)}`,
    );
}

/* The collection to file into: one of the ones that exist, or a new name.
 *
 * THE EXISTING VOCABULARY IS OFFERED BEFORE A NEW WORD IS COINED. §1.3 makes a
 * collection implicit on first ingest, so a typo is a new collection rather
 * than an error — `win32-iocp` and `win32-icop` are two topics that will never
 * find each other again. Showing the list is the only thing standing between a
 * reader and that. */
async function pickCollection(kb: Kb): Promise<string | undefined> {
    let existing: string[] = [];
    try {
        existing = (await kb.collections()).map((c) => c.name).sort();
    } catch {
        /* No store yet, or an unreadable one. The free-text path still works,
         * and the write below will report whatever is really wrong. */
    }
    const NEW = "$(add) New collection…";
    const picked =
        existing.length === 0
            ? NEW
            : await vscode.window.showQuickPick([...existing, NEW], {
                  title: "File into which collection?",
                  placeHolder: "A flat named scope: win32-iocp, io-uring, papers",
              });
    if (picked === undefined) {
        return undefined;
    }
    if (picked !== NEW) {
        return picked;
    }
    const typed = await vscode.window.showInputBox({
        title: "New collection",
        prompt: "Collections do not nest and a document belongs to exactly one.",
        validateInput: (value) => {
            if (value.trim().length === 0) {
                return "A collection needs a name.";
            }
            /* The CLI refuses a separator with "collection names do not nest".
             * Saying so here costs a round trip less. */
            return value.includes("/") || value.includes("\\")
                ? "Collections do not nest, so a name cannot carry a separator."
                : null;
        },
    });
    return typed?.trim();
}

/* §2: "add the current file". */
export async function addCurrentFile(kb: Kb, announce: () => void): Promise<void> {
    const editor = vscode.window.activeTextEditor;
    if (editor === undefined) {
        void vscode.window.showWarningMessage("There is no file open to add.");
        return;
    }
    const doc = editor.document;
    const text = doc.getText();
    if (text.trim().length === 0) {
        void vscode.window.showWarningMessage("That file is empty, so there is nothing to index.");
        return;
    }
    const collection = await pickCollection(kb);
    if (collection === undefined || collection.length === 0) {
        return;
    }
    const name = path.basename(doc.uri.path);
    const title = await vscode.window.showInputBox({
        title: "Title",
        value: name,
        prompt: "What this document is called in the store.",
    });
    if (title === undefined || title.trim().length === 0) {
        return;
    }
    try {
        const added = await kb.add(text, {
            title: title.trim(),
            collection,
            /* The locator is the file's own path, which is what §1.2 calls a
             * `file` source: re-filing the same path is the same document and
             * re-indexes nothing when the bytes have not moved. A `file://`
             * URI would be a different string for the same thing. */
            url: doc.uri.scheme === "file" ? doc.uri.fsPath : doc.uri.toString(),
            mime: mimeForLanguage(doc.languageId),
        });
        announce();
        void vscode.window.showInformationMessage(
            `${added.created ? "Filed" : added.reindexed ? "Updated" : "Already filed"} ${added.document} in ${added.collection} (${added.chunkCount} chunk${added.chunkCount === 1 ? "" : "s"}).`,
        );
    } catch (e) {
        report(e, "file that document");
    }
}

/* §2: "add a URL". See the header for why the fetch is here. */
export async function addUrl(kb: Kb, announce: () => void): Promise<void> {
    const typed = await vscode.window.showInputBox({
        title: "Add a URL",
        prompt: "The page is fetched here and its text is filed through kb.",
        placeHolder: "https://learn.microsoft.com/…",
        validateInput: (value) =>
            /^https?:\/\/\S+$/i.test(value.trim())
                ? null
                : "An http or https URL. kb itself has no network (index-api.md §12.2).",
    });
    const url = typed?.trim();
    if (url === undefined || url.length === 0) {
        return;
    }
    let host: string;
    try {
        host = new URL(url).host;
    } catch {
        void vscode.window.showWarningMessage(`Knowledge could not read that URL: ${url}`);
        return;
    }
    const go = await vscode.window.showWarningMessage(
        `Fetch ${host}?`,
        {
            modal: true,
            detail: `Knowledge reads a local store and makes no network requests of its own. Adding a URL is the exception: this downloads the page from ${host} and files its text.\n\n${url}`,
        },
        "Fetch",
    );
    if (go !== "Fetch") {
        return;
    }
    const collection = await pickCollection(kb);
    if (collection === undefined || collection.length === 0) {
        return;
    }
    try {
        const fetched = await fetchPage(url);
        const title = await vscode.window.showInputBox({
            title: "Title",
            value: fetched.title,
            prompt: "What this document is called in the store.",
        });
        if (title === undefined || title.trim().length === 0) {
            return;
        }
        const added = await kb.add(fetched.text, {
            title: title.trim(),
            collection,
            url,
            mime: fetched.mime,
        });
        announce();
        void vscode.window.showInformationMessage(
            `${added.created ? "Filed" : "Updated"} ${added.document} in ${added.collection}.`,
        );
    } catch (e) {
        report(e, "file that page");
    }
}

export interface FetchedPage {
    text: string;
    mime: string;
    title: string;
}

/* The fetch, as small as it can be.
 *
 * THE BYTES ARE FILED AS THEY ARRIVED. A page fetched and then rewritten here
 * — tags stripped, entities decoded, whitespace collapsed — would be a
 * document whose content hash covers this extension's opinion of the page
 * rather than the page. §3.2 renders HTML as prose at READ time, from the
 * stored bytes, which is where an opinion can be changed without invalidating
 * anything.
 *
 * A REDIRECT IS FOLLOWED AND A NON-2xx IS NOT A DOCUMENT. §11 has
 * `fetch_failed` carrying "status and locator" for exactly this, and filing a
 * 404 page under a title somebody chose is how a store comes to contain a
 * confident answer that is an error page. */
export async function fetchPage(url: string): Promise<FetchedPage> {
    const response = await fetch(url, {
        redirect: "follow",
        headers: { accept: "text/html,text/markdown,text/plain;q=0.9,*/*;q=0.5" },
    });
    if (!response.ok) {
        throw new Error(`${url} answered ${response.status} ${response.statusText}.`);
    }
    const contentType = response.headers.get("content-type") ?? "";
    const mime = contentType.split(";")[0].trim().toLowerCase() || "text/plain";
    const text = await response.text();
    return { text, mime, title: titleOf(text, mime, url) };
}

/* A suggested title, which the reader then edits. Read out of the page when it
 * has one, because "Untitled" in a provenance block is a document nobody will
 * find again. */
export function titleOf(text: string, mime: string, url: string): string {
    if (mime === "text/html") {
        const m = /<title[^>]*>([\s\S]{0,300}?)<\/title>/i.exec(text);
        if (m !== null) {
            const title = m[1].replace(/\s+/g, " ").trim();
            if (title.length > 0) {
                return title;
            }
        }
    }
    if (mime === "text/markdown") {
        const m = /^#\s+(.+)$/m.exec(text);
        if (m !== null) {
            return m[1].trim();
        }
    }
    try {
        const parsed = new URL(url);
        const last = parsed.pathname.split("/").filter((p) => p.length > 0).pop();
        return last === undefined ? parsed.host : `${parsed.host} ${last}`;
    } catch {
        return url;
    }
}

/* §2: "refresh stale documents", and §3.1's single-document version. */
export async function refreshStale(
    kb: Kb,
    olderThanDays: number,
    announce: () => void,
): Promise<void> {
    try {
        const done = await kb.refresh({ olderThan: `${olderThanDays}d` });
        announce();
        /* IT SAYS WHAT HAPPENED, WHICH TODAY IS NOTHING. §12.2 resolved
         * against putting an HTTP client and TLS in the binary, so refresh
         * reports what has gone stale and fetches none of it. This used to
         * read "Refreshed 0 documents; 0 had changed" off two keys the store
         * has never emitted — right by accident, and a sentence that told a
         * reader their corpus was current. `note` is the store's own word for
         * it and is shown rather than summarised. */
        void vscode.window.showInformationMessage(
            done.count === 0
                ? `Nothing older than ${done.olderThan}.`
                : `${done.count} source${done.count === 1 ? "" : "s"} older than ${done.olderThan}, covering ${done.staleDocuments} document${done.staleDocuments === 1 ? "" : "s"}. ${done.note}`,
        );
    } catch (e) {
        report(e, "refresh stale documents");
    }
}
