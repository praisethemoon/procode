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

import { Kb, KbDocument, KbError, fetchPage, isKbError, titleOf } from "kb-js";
import type { FetchedPage } from "kb-js";

import { extractPdf, isPdf } from "./pdf";
import { RefreshOutcome, folderMessage, refreshPlan } from "./refresh";
import { saveName } from "./view/savename";

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

/* "N chunks", for a count the reader is told. */
function chunks(n: number): string {
    return `${n} chunk${n === 1 ? "" : "s"}`;
}

export const FINISH_EMBEDDING = "Finish embedding";

/* What a filing left to embed, as a sentence to append to the message that
 * reports it: kb files at once and embeds within a budget (index-api §2), so
 * a large document is searchable by keyword now and semantically once the
 * rest is embedded. Empty when nothing is pending. */
export function pendingNote(pending: number): string {
    return pending > 0
        ? ` ${chunks(pending)} left to embed: searchable by keyword now, and by meaning once embedded.`
        : "";
}

/* The message that closes a filing. When the filing left embeddings pending
 * it offers to finish them, which runs the same pass as the command. */
function tell(kb: Kb, announce: () => void, message: string, pending: number): void {
    if (pending <= 0) {
        void vscode.window.showInformationMessage(message);
        return;
    }
    void Promise.resolve(vscode.window.showInformationMessage(`${message}${pendingNote(pending)}`, FINISH_EMBEDDING)).then(
        (chosen) => {
            if (chosen === FINISH_EMBEDDING) {
                return finishEmbedding(kb, announce);
            }
            return undefined;
        },
    );
}

/* `kb embed`: every chunk a budgeted filing left without a vector, embedded
 * now, with progress shown because it can take minutes. */
export async function finishEmbedding(kb: Kb, announce: () => void): Promise<void> {
    try {
        const done = await vscode.window.withProgress(
            {
                location: vscode.ProgressLocation.Notification,
                title: "Embedding the chunks left to embed…",
            },
            () => kb.embed(),
        );
        announce();
        void vscode.window.showInformationMessage(
            done.embedded === 0 ? "Nothing was left to embed." : `Embedded ${chunks(done.embedded)}.`,
        );
    } catch (e) {
        report(e, "finish embedding");
    }
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
        tell(
            kb,
            announce,
            `${added.created ? "Filed" : added.reindexed ? "Updated" : "Already filed"} ${added.document} in ${added.collection} (${added.chunkCount} chunk${added.chunkCount === 1 ? "" : "s"}).`,
            added.pending,
        );
    } catch (e) {
        report(e, "file that document");
    }
}

/* Files picked from disk with the system dialog, several at once. Each is
 * filed under its own name, with its path as the locator — exactly what
 * "add the current file" does for one open file — so re-adding an unchanged
 * file re-indexes nothing. `collection` is preselected when the dialog was
 * opened from a collection's row. Empty and binary files are skipped and
 * named, rather than filed as noise. */
export async function addFiles(kb: Kb, announce: () => void, collection?: string): Promise<void> {
    const picked = await vscode.window.showOpenDialog({
        canSelectMany: true,
        canSelectFiles: true,
        canSelectFolders: false,
        openLabel: "Add to Knowledge",
        title: collection ? `Add files to ${collection}` : "Add files to Knowledge",
        filters: {
            "Text, code and PDF": ["md", "markdown", "txt", "rst", "adoc", "html", "htm", "pdf", "c", "h", "cc", "cpp", "hpp", "rs", "go", "py", "ts", "tsx", "js", "java", "json", "yaml", "yml", "toml"],
            "All files": ["*"],
        },
    });
    if (picked === undefined || picked.length === 0) {
        return;
    }
    const into = collection ?? (await pickCollection(kb));
    if (into === undefined || into.length === 0) {
        return;
    }
    let title: string | undefined;
    if (picked.length === 1) {
        title = await vscode.window.showInputBox({
            title: "Title",
            value: path.basename(picked[0].path),
            prompt: "What this document is called in the store.",
        });
        if (title === undefined || title.trim().length === 0) {
            return;
        }
    }
    const filed: string[] = [];
    const skipped: string[] = [];
    /* Each filing embeds what the one before it left, within its own budget,
     * so the last answer is what the store as a whole has left to embed. */
    let pending = 0;
    for (const uri of picked) {
        const name = path.basename(uri.path);
        const bytes = await vscode.workspace.fs.readFile(uri);
        if (isPdf(name) && bytes.length > 0) {
            /* A paper: its text extracted here and filed as Markdown, one
             * section per page, with the PDF's path as the locator. */
            try {
                const paper = await extractPdf(bytes, name.replace(/\.pdf$/i, ""));
                const added = await kb.add(paper.markdown, {
                    title: (title ?? paper.title).trim(),
                    collection: into,
                    url: uri.fsPath,
                    mime: "text/markdown",
                    meta: paper.meta,
                });
                filed.push(added.document);
                pending = added.pending;
            } catch (e) {
                report(e, `file ${name}`);
            }
            continue;
        }
        if (bytes.length === 0 || bytes.includes(0)) {
            skipped.push(name);
            continue;
        }
        try {
            const added = await kb.add(Buffer.from(bytes).toString("utf8"), {
                title: (title ?? name).trim(),
                collection: into,
                /* The path is the locator, and kb infers the mime (and so the
                 * splitter: headings for Markdown, declarations for code)
                 * from its extension. */
                url: uri.fsPath,
            });
            filed.push(added.document);
            pending = added.pending;
        } catch (e) {
            report(e, `file ${name}`);
        }
    }
    if (filed.length > 0) {
        announce();
    }
    const parts = [];
    if (filed.length > 0) parts.push(`Filed ${filed.join(", ")} in ${into}.`);
    if (skipped.length > 0) parts.push(`Skipped ${skipped.join(", ")}: empty or not text.`);
    if (parts.length > 0) {
        tell(kb, announce, parts.join(" "), pending);
    }
}

/* A folder picked from disk, filed whole as one source with a document per
 * file (index-api §2.1). kb walks it itself — the ignore rules, the per-file
 * types and the skipping are the CLI's — so nothing is read here but the
 * folder's name. Filing the same folder again re-indexes only what changed.
 *
 * FORGETTING IS ON. A file gone from the folder since it was last filed is
 * forgotten, which is the CLI's default and is right here: this is the
 * reader's own action on their own folder, and §9 withholds forgetting from
 * agents, not from the reader. The message names every forgotten count so it
 * is never silent. */
export async function addFolder(kb: Kb, announce: () => void, collection?: string): Promise<void> {
    const picked = await vscode.window.showOpenDialog({
        canSelectMany: false,
        canSelectFiles: false,
        canSelectFolders: true,
        openLabel: "Add to Knowledge",
        title: collection ? `Add a folder to ${collection}` : "Add a folder to Knowledge",
    });
    if (picked === undefined || picked.length === 0) {
        return;
    }
    const folder = picked[0];
    const into = collection ?? (await pickCollection(kb));
    if (into === undefined || into.length === 0) {
        return;
    }
    await fileFolder(kb, announce, folder.fsPath, into);
}

/* One folder filed with progress shown, and the totals told once it is done. */
async function fileFolder(kb: Kb, announce: () => void, folder: string, into: string): Promise<void> {
    const name = path.basename(folder);
    try {
        const filed = await vscode.window.withProgress(
            {
                location: vscode.ProgressLocation.Notification,
                title: `Filing ${name} into ${into}…`,
            },
            () => kb.addDir(folder, { collection: into }),
        );
        announce();
        tell(kb, announce, folderMessage(filed), filed.pending);
    } catch (e) {
        report(e, `file ${name}`);
    }
}

/* A document's stored text written to a file the reader picks, as kb holds
 * it; the document in the store is not touched. The dialog starts in the
 * first workspace folder with the name the document is known by. Returns the
 * path written, or null when nothing was. */
export async function saveDocumentAs(kb: Kb, id: string): Promise<string | null> {
    let read;
    try {
        read = await kb.get(id, { text: true });
    } catch (e) {
        report(e, `read ${id}`);
        return null;
    }
    if (typeof read.text !== "string") {
        void vscode.window.showWarningMessage(`${id} has no stored text to save: its blob is missing, which kb rebuild repairs.`);
        return null;
    }
    const name = saveName(read.document);
    const folder = vscode.workspace.workspaceFolders?.find((f) => f.uri.scheme === "file")?.uri;
    const target = await vscode.window.showSaveDialog({
        defaultUri: folder ? vscode.Uri.joinPath(folder, name) : vscode.Uri.file(name),
        saveLabel: "Save",
        title: `Save ${id} as`,
    });
    if (target === undefined) {
        return null;
    }
    await vscode.workspace.fs.writeFile(target, new TextEncoder().encode(read.text));
    return target.fsPath;
}

/* The open workspace folders filed as Add Folder files one, with nothing to
 * pick: each into a collection named after the folder, so a multi-root
 * workspace keeps its folders apart. Running it again is how the index is kept
 * current, since kb files only what changed and forgets what is gone. */
export async function indexWorkspace(kb: Kb, announce: () => void): Promise<void> {
    const folders = (vscode.workspace.workspaceFolders ?? []).filter((f) => f.uri.scheme === "file");
    if (folders.length === 0) {
        void vscode.window.showWarningMessage("Open a folder first: Index This Workspace files the folders open in this window.");
        return;
    }
    for (const f of folders) {
        await fileFolder(kb, announce, f.uri.fsPath, path.basename(f.uri.fsPath));
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
        if (fetched.notModified) {
            return; // not asked for conditionally, so not reachable; keeps the type honest
        }
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
            etag: fetched.etag,
        });
        announce();
        tell(kb, announce, `${added.created ? "Filed" : "Updated"} ${added.document} in ${added.collection}.`, added.pending);
    } catch (e) {
        report(e, "file that page");
    }
}

/* index-ui §3.1's refresh action for one document: read its source again and
 * re-file the text through kb under everything the store already knows about
 * it, so kb's hash comparison decides whether anything changed. A URL is the
 * one network request Knowledge makes, so it is asked for first, exactly as
 * adding a URL is. */
export async function refreshDocument(kb: Kb, id: string): Promise<RefreshOutcome> {
    const d = (await kb.get(id)).document;
    if (d.path !== "") {
        return refreshFolderDocument(kb, d);
    }
    const plan = refreshPlan(d.locator);
    if (plan.kind === "none") {
        return { outcome: "cannot", why: plan.why };
    }
    let text: string;
    let mime = d.mime;
    let etag: string | null = null;
    if (plan.kind === "url") {
        const go = await vscode.window.showWarningMessage(
            `Fetch ${plan.host} again?`,
            { modal: true, detail: `Refreshing ${d.id} downloads its page again and files the text if it changed.\n\n${plan.url}` },
            "Fetch",
        );
        if (go !== "Fetch") {
            return { outcome: "declined" };
        }
        /* Asked conditionally when the last fetch recorded an ETag: a 304 is
         * the server saying the page is the one already filed, so the stored
         * text is filed again — which only moves its fetch date (§2). */
        const known = (await kb.source(d.source)).source.etag;
        const fetched = await fetchPage(plan.url, known);
        if (fetched.notModified) {
            text = (await kb.get(id, { text: true })).text ?? "";
            etag = known;
        } else {
            text = fetched.text;
            mime = fetched.mime;
            etag = fetched.etag;
        }
    } else {
        let bytes: Uint8Array;
        try {
            bytes = await vscode.workspace.fs.readFile(vscode.Uri.file(plan.path));
        } catch {
            return { outcome: "cannot", why: `its file is no longer at ${plan.path}.` };
        }
        if (isPdf(plan.path)) {
            text = (await extractPdf(bytes, d.title)).markdown;
            mime = "text/markdown";
        } else {
            text = Buffer.from(bytes).toString("utf8");
        }
    }
    const added = await kb.add(text, {
        title: d.title,
        collection: d.collection,
        url: d.locator,
        mime,
        meta: d.meta,
        etag,
    });
    return {
        outcome: added.created || added.reindexed ? "updated" : "unchanged",
        document: added.document,
        fetchedAt: added.fetchedAt,
    };
}

/* A file of a folder (index-api §2.1). Its locator is the folder, not the file,
 * and kb reads the file itself by walking the folder again — there is no route
 * that re-files one of its documents alone. WITHOUT FORGETTING: refreshing one
 * document is not where its neighbours should disappear, so a file gone from
 * the folder is reported rather than acted on. Whether this one changed is read
 * off its content hash, since the walk counts the folder rather than naming
 * each document. */
async function refreshFolderDocument(kb: Kb, d: KbDocument): Promise<RefreshOutcome> {
    const walked = await kb.addDir(d.locator, { collection: d.collection, forget: false });
    if (walked.missing.includes(d.path)) {
        return { outcome: "cannot", why: `its file is no longer at ${path.join(d.locator, d.path)}.` };
    }
    const now = (await kb.get(d.id)).document;
    return {
        outcome: now.contentHash === d.contentHash ? "unchanged" : "updated",
        document: now.id,
        fetchedAt: now.fetchedAt,
    };
}

/* The fetch that Add URL and a URL's refresh use lives in kb-js, shared with
 * the kb MCP server's kb_add; re-exported for the tests that drive it here. */
export { fetchPage, titleOf };
export type { FetchedPage };

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
