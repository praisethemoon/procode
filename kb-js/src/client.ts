/* `kb-js` (index-api.md §10): one object holding where the store is, and one
 * method per command.
 *
 * IT SHELLS OUT AND PARSES `--json`. IT DOES NOT REIMPLEMENT RETRIEVAL, and
 * §10 states the reason in a way worth repeating here because it is the whole
 * shape of this package: reading a lap log is parsing, so `lap-js` folds it
 * in-process; searching here requires embedding the query, which requires the
 * model. A second implementation would be a second inference path, and two
 * inference paths that disagree produce a store that returns different answers
 * depending on which door the caller came through. One implementation, in C.
 *
 * SO THERE IS NOTHING CLEVER IN THIS FILE, AND THAT IS THE POINT. Each method
 * builds an argv (`argv.ts`), runs it (`run.ts`) and reads the answer
 * (`shape.ts`). No method merges tiers, ranks anything, filters a list the
 * store already filtered, or computes a field the store emits — a helper that
 * did would be the second implementation arriving one convenience at a time.
 *
 * THE SEARCH AND CHUNK PATHS ARE BUILT TO §4 AND THE COMMANDS THAT SERVE THEM
 * ARE BEING BUILT IN PARALLEL. Everything here is shaped by the specification
 * rather than by a binary that exists: `search` reads §4's hit field for field
 * and `chunk` reads "the full chunk text and its neighbours". Where §4 is a
 * query string and the CLI is a command line, `argv.ts` records each choice of
 * spelling and is the one file to edit when the two are reconciled.
 */

import {
    AddOptions,
    GetOptions,
    LsOptions,
    RefreshOptions,
    SearchOptions,
    StaleOptions,
    addArgv,
    chunkArgv,
    collectionsArgv,
    deleteCollectionArgv,
    getArgv,
    initArgv,
    linkArgv,
    linksArgv,
    lsArgv,
    refreshArgv,
    renameCollectionArgv,
    searchArgv,
    staleArgv,
    statsArgv,
    statusArgv,
} from "./argv";
import { KbOptions, run } from "./run";
import {
    arr,
    num,
    readChunkRead,
    readCollection,
    readDocument,
    readDocumentRead,
    readHit,
    readLinkWritten,
    readLinks,
    readStats,
    readStatus,
    str,
} from "./shape";
import {
    KbAdded,
    KbChunkRead,
    KbCollection,
    KbDocument,
    KbDocumentRead,
    KbHit,
    KbLinkWritten,
    KbLinks,
    KbStats,
    KbStatus,
    StoreSelector,
} from "./types";

export interface KbSearchResult {
    readonly hits: readonly KbHit[];
    /* How many hits came back, as the store counted them. Read rather than
     * taken from `hits.length`: a store that pages would report a total the
     * array does not have, and a binding that answered the array's length
     * would quietly redefine the field. */
    readonly count: number;
}

export class Kb {
    constructor(private readonly options: KbOptions = {}) {}

    /* A second client pointed at another directory — which is another project
     * store, because §1.4 finds one by walking up from the working directory.
     * Cheaper and clearer than a `cwd` argument on every call, and it keeps
     * `cwd` out of the per-call shape where a caller could forget it. */
    at(cwd: string): Kb {
        return new Kb({ ...this.options, cwd });
    }

    /* ------------------------------------------------------------- reads */

    /* §2's `GET /documents`. Rows in the store's own order, which is log order
     * within each tier and project before global; §2 of the UI spec wants them
     * newest first and does that where it draws them, because the order a list
     * is READ in is the store's and the order it is SHOWN in is the surface's. */
    async ls(options: LsOptions = {}): Promise<readonly KbDocument[]> {
        const payload = await run(lsArgv(options), this.options);
        return arr(payload["documents"]).map(readDocument);
    }

    /* §2's `GET /documents/{id}` with `?include=text,chunks`. */
    async get(id: string, options: GetOptions = {}): Promise<KbDocumentRead> {
        return readDocumentRead(await run(getArgv(id, options), this.options));
    }

    /* §7's `GET /collections` and `GET /stats`, which the CLI answers at once. */
    async collections(store?: StoreSelector | null): Promise<readonly KbCollection[]> {
        const payload = await run(collectionsArgv(store), this.options);
        return arr(payload["collections"]).map(readCollection);
    }

    /* §7's `GET /status`: both tiers, and which one a write would land in. */
    async status(): Promise<KbStatus> {
        return readStatus(await run(statusArgv(), this.options));
    }

    /* ------------------------------------------------------------ search */

    /* §4. Hybrid is the default and §4 says it is not an optimisation — this
     * corpus is dense with exact identifiers that embeddings place on top of
     * their opposites, and keyword retrieval is what resolves them. The default
     * is left to the store rather than sent from here: a binding that spelled
     * `mode=hybrid` on every call would silently pin the default the day the
     * store changed it. */
    async search(query: string, options: SearchOptions = {}): Promise<KbSearchResult> {
        const payload = await run(searchArgv(query, options), this.options);
        const hits = arr(payload["hits"]).map(readHit);
        return { hits, count: "count" in payload ? num(payload["count"]) : hits.length };
    }

    /* §4's `GET /chunks/{id}`: "the full chunk text and its neighbours". */
    async chunk(
        id: string,
        options: { expand?: number | null; store?: StoreSelector | null } = {},
    ): Promise<KbChunkRead> {
        return readChunkRead(await run(chunkArgv(id, options), this.options));
    }

    /* ------------------------------------------------------------ writes */

    /* §2's `POST /documents`, the load-bearing route: the caller already has
     * the text and hands it over instead of causing a second fetch.
     *
     * The content goes down stdin (`argv.ts` says why) and never through the
     * argument list. */
    async add(content: string, options: AddOptions): Promise<KbAdded> {
        const payload = await run(addArgv(options), this.options, content);
        return {
            store: payload["store"] === "global" ? "global" : "project",
            document: str(payload["document"]),
            source: str(payload["source"]),
            contentHash: str(payload["contentHash"]),
            bytes: num(payload["bytes"]),
            collection: str(payload["collection"]),
            mime: str(payload["mime"]),
            splitter: str(payload["splitter"]),
            chunkCount: num(payload["chunkCount"]),
            chunkBase: num(payload["chunkBase"]),
            created: payload["created"] === true,
            reindexed: payload["reindexed"] === true,
            blobWritten: payload["blobWritten"] === true,
            fetchedAt: str(payload["fetchedAt"]),
        };
    }

    /* §5's `GET /stale`: "documents whose age exceeds a threshold, newest
     * sources first". */
    async stale(options: StaleOptions = {}): Promise<readonly KbDocument[]> {
        const payload = await run(staleArgv(options), this.options);
        return arr(payload["documents"]).map(readDocument);
    }

    /* §6's `GET /documents/{id}/links`: "outgoing and incoming, resolved to
     * rows". */
    async links(document: string, store?: StoreSelector | null): Promise<KbLinks> {
        return readLinks(await run(linksArgv(document, store), this.options));
    }

    /* §6's `POST /links`. `analogue_of` is the one that motivated the layer:
     * IOCP and io_uring and kqueue solve the same problem three ways, and no
     * amount of semantic similarity will state that relationship. */
    async link(
        from: string,
        type: string,
        to: string,
        store?: "project" | "global" | null,
    ): Promise<KbLinkWritten> {
        return readLinkWritten(await run(linkArgv(from, type, to, store), this.options));
    }

    /* §7's `GET /stats`. `collections` above answers the other half of the
     * same question; `argv.ts` records why they are two commands. */
    async stats(store?: StoreSelector | null): Promise<KbStats> {
        return readStats(await run(statsArgv(store), this.options));
    }

    /* §5's `POST /refresh`: "refetch a whole scope, re-embedding only changed
     * content". The command does not exist in the CLI yet (`argv.ts` says so),
     * so today this answers a `usage` refusal — which is a sentence a reader
     * can act on, and is what a surface offering the action owes them. */
    async refresh(options: RefreshOptions = {}): Promise<{ refreshed: number; changed: number }> {
        const payload = await run(refreshArgv(options), this.options);
        return { refreshed: num(payload["refreshed"]), changed: num(payload["changed"]) };
    }

    /* §7's `PATCH /collections/{name}`. */
    async renameCollection(
        from: string,
        to: string,
        store?: "project" | "global" | null,
    ): Promise<void> {
        await run(renameCollectionArgv(from, to, store), this.options);
    }

    /* §7's `DELETE /collections/{name}`. §11 has `collection_in_use` for the
     * case this refuses, carrying the document count — which is exactly what a
     * confirmation dialog needs to be able to say. */
    async deleteCollection(name: string, store?: "project" | "global" | null): Promise<void> {
        await run(deleteCollectionArgv(name, store), this.options);
    }

    /* §1.4's two tiers, created. `init` does not adopt a store that exists
     * above the current directory — it creates one here — which is the CLI's
     * rule and is why this takes no path. */
    async init(store?: "project" | "global" | null): Promise<{ store: string; path: string }> {
        const payload = await run(initArgv(store), this.options);
        return { store: str(payload["store"]), path: str(payload["path"]) };
    }
}
