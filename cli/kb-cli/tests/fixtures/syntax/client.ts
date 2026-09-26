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
    AddDirOptions,
    AddOptions,
    BatchDocument,
    addBatchArgv,
    batchLines,
    GetOptions,
    LsOptions,
    RefreshOptions,
    SearchOptions,
    StaleOptions,
    addArgv,
    addDirArgv,
    chunkArgv,
    collectionsArgv,
    deleteCollectionArgv,
    forgetArgv,
    getArgv,
    refreshSourceArgv,
    sourceArgv,
    sourcesArgv,
    SourcesOptions,
    initArgv,
    linkArgv,
    linksArgv,
    allLinksArgv,
    lsArgv,
    refreshArgv,
    renameCollectionArgv,
    searchArgv,
    staleArgv,
    statsArgv,
    statusArgv,
} from "./argv";
import { DEFAULT_DIR_TIMEOUT_MS, KbOptions, run } from "./run";
import {
    arr,
    num,
    obj,
    readAdded,
    readChunkRead,
    readCollection,
    readDirAdded,
    readDocument,
    readDocumentRead,
    readEdges,
    readForgotten,
    readSource,
    readSourceRead,
    readSourceRefreshed,
    readHit,
    readLinkWritten,
    readLinks,
    readRefresh,
    readStaleList,
    readStats,
    readStatus,
    str,
} from "./shape";
import {
    KbAdded,
    KbChunkRead,
    KbCollection,
    KbDocument,
    KbDirAdded,
    KbDocumentRead,
    KbEdge,
    KbForgotten,
    KbHit,
    KbSource,
    KbSourceRead,
    KbSourceRefreshed,
    KbLinkWritten,
    KbLinks,
    KbRefresh,
    KbStaleList,
    KbStats,
    KbStatus,
} from "./types";

export interface KbSearchResult {
    readonly hits: readonly KbHit[];
    /* Which retrieval path the store actually ran. §4 makes hybrid the default
     * and §8 makes it refuse without a model, so the mode that came back is
     * not always the mode that was asked for — and a caller told only the hits
     * cannot know whether the exact identifier it searched for was resolved by
     * keyword or approximated by vectors. */
    readonly mode: string;
    /* The staleness threshold every hit's `stale` flag was computed against.
     * FOUND BY THE KEY-COVERAGE TEST IN `cli.test.ts`, NOT BY READING THE
     * COMMAND: the store started printing it and this reader went on not
     * answering it, which is the same defect as `refresh` one route along. §4
     * puts `stale` on a hit and §5 says why it matters; a caller shown a hit
     * flagged stale and not the threshold cannot say older than what. */
    readonly olderThan: string;
    /* How many hits came back, as the store counted them. Read rather than
     * taken from `hits.length`: a store that pages would report a total the
     * array does not have, and a binding that answered the array's length
     * would quietly redefine the field. */
    readonly count: number;
}

export class Kb {
    constructor(private readonly options: KbOptions = {}) {}

    /* A second client pointed at another directory — which may be another
     * store, because §1.4 finds one by walking up from the working directory.
     * Cheaper and clearer than a `cwd` argument on every call, and it keeps
     * `cwd` out of the per-call shape where a caller could forget it. */
    at(cwd: string): Kb {
        return new Kb({ ...this.options, cwd });
    }

    /* ------------------------------------------------------------- reads */

    /* §2's `GET /documents`. Rows in the store's own order, which is log
     * order; §2 of the UI spec wants them
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
    async collections(): Promise<readonly KbCollection[]> {
        const payload = await run(collectionsArgv(), this.options);
        return arr(payload["collections"]).map(readCollection);
    }

    /* §7's `GET /status`: the store this client's directory finds, or
     * `path: null` when there is none. */
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
        return {
            hits,
            mode: str(payload["mode"]),
            olderThan: str(payload["olderThan"]),
            count: "count" in payload ? num(payload["count"]) : hits.length,
        };
    }

    /* §4's `GET /chunks/{id}`: "the full chunk text and its neighbours". */
    async chunk(id: string, options: { expand?: number | null } = {}): Promise<KbChunkRead> {
        return readChunkRead(await run(chunkArgv(id, options), this.options));
    }

    /* ------------------------------------------------------------ writes */

    /* §2's `POST /documents`, the load-bearing route: the caller already has
     * the text and hands it over instead of causing a second fetch.
     *
     * The content goes down stdin (`argv.ts` says why) and never through the
     * argument list. */
    async add(content: string, options: AddOptions): Promise<KbAdded> {
        return readAdded(await run(addArgv(options), this.options, content));
    }

    /* §2's `POST /documents/batch`: every document filed under one lock with
     * one index rebuild, or — when any of them is refused — none at all. The
     * answers come back in the order the documents were given. */
    async addBatch(documents: readonly BatchDocument[]): Promise<readonly KbAdded[]> {
        const payload = await run(addBatchArgv(), this.options, batchLines(documents));
        return arr(payload["added"]).map((row) => readAdded(obj(row)));
    }

    /* §2.1's folder, filed as one `dir` source with a document per file. The
     * store walks it, so this hands over a path and not content; a relative
     * path is resolved against this client's directory, as the CLI's own
     * working directory. Forgetting files gone from the folder is on unless
     * `forget: false` asks for them to be reported as `missing` instead. */
    async addDir(dir: string, options: AddDirOptions): Promise<KbDirAdded> {
        const patient: KbOptions = { ...this.options, timeoutMs: this.options.timeoutMs ?? DEFAULT_DIR_TIMEOUT_MS };
        return readDirAdded(await run(addDirArgv(dir, options), patient));
    }

    /* §5's `GET /stale`: "documents whose age exceeds a threshold, newest
     * sources first".
     *
     * THE THRESHOLD COMES BACK WITH THE ROWS. "18 documents are stale" means
     * nothing without "older than what", and a caller that did not send an
     * `olderThan` cannot say which default it got. */
    async stale(options: StaleOptions = {}): Promise<KbStaleList> {
        return readStaleList(await run(staleArgv(options), this.options));
    }

    /* §6's `GET /documents/{id}/links`: "outgoing and incoming, resolved to
     * rows". */
    async links(document: string): Promise<KbLinks> {
        return readLinks(await run(linksArgv(document), this.options));
    }

    /* Every link in the store, for a graph of it. */
    async allLinks(): Promise<KbEdge[]> {
        return readEdges(await run(allLinksArgv(), this.options));
    }

    /* §6's `POST /links`. `analogue_of` is the one that motivated the layer:
     * IOCP and io_uring and kqueue solve the same problem three ways, and no
     * amount of semantic similarity will state that relationship. */
    async link(from: string, type: string, to: string): Promise<KbLinkWritten> {
        return readLinkWritten(await run(linkArgv(from, type, to), this.options));
    }

    /* §7's `GET /stats`. `collections` above answers the other half of the
     * same question; `argv.ts` records why they are two commands. */
    async stats(): Promise<KbStats> {
        return readStats(await run(statsArgv(), this.options));
    }

    /* §5's `POST /refresh`, which today REPORTS and does not act.
     *
     * §12.2 resolved against putting an HTTP client and TLS in the binary, so
     * the command names the sources that have gone stale and the route that
     * would bring each up to date, and fetches nothing. `KbRefresh.note` is
     * the store saying so in a sentence, and a surface that announces a
     * refresh without showing it is announcing something that did not happen.
     *
     * The two keys this used to read — `refreshed` and `changed` — were never
     * in the answer and defaulted to zero, which is the number a report of no
     * action has. It was right by accident, which is worse than wrong: it
     * would have stayed right in appearance and become wrong in fact on the
     * day refresh started refetching. */
    async refresh(options: RefreshOptions = {}): Promise<KbRefresh> {
        return readRefresh(await run(refreshArgv(options), this.options));
    }

    /* §7's `PATCH /collections/{name}`. */
    async renameCollection(from: string, to: string): Promise<void> {
        await run(renameCollectionArgv(from, to), this.options);
    }

    /* §7's `DELETE /collections/{name}`. §11 has `collection_in_use` for the
     * case this refuses, carrying the document count — which is exactly what a
     * confirmation dialog needs to be able to say. */
    async deleteCollection(name: string, options: { withDocuments?: boolean } = {}): Promise<void> {
        await run(deleteCollectionArgv(name, options.withDocuments === true), this.options);
    }

    /* §2's GET /sources: one row per source, narrowed by collection or kind. */
    async sources(options: SourcesOptions = {}): Promise<readonly KbSource[]> {
        const payload = await run(sourcesArgv(options), this.options);
        return arr(payload["sources"]).map(readSource);
    }

    /* §2's GET /sources/{id}: the source, its documents and their fetch history. */
    async source(id: string): Promise<KbSourceRead> {
        return readSourceRead(await run(sourceArgv(id), this.options));
    }

    /* §2's POST /sources/{id}/refresh, for a file source: read it again and
     * re-index only if its text changed. A url or inline source is refused. A
     * `dir` source is walked again by the CLI and answers in `addDir`'s shape,
     * not this one, so it is refreshed through `addDir` with its locator and
     * collection instead. */
    async refreshSource(id: string): Promise<KbSourceRefreshed> {
        return readSourceRefreshed(await run(refreshSourceArgv(id), this.options));
    }

    /* §2's DELETE /documents/{id} and DELETE /sources/{id}. Not offered to
     * agents (§9): forgetting is the reader's decision. */
    async forget(id: string): Promise<KbForgotten> {
        return readForgotten(await run(forgetArgv(id), this.options));
    }

    /* §1.4's store, created in this client's directory. `init` does not adopt
     * a store that exists above it — it creates one here — which is the CLI's
     * rule and is why this takes no path. */
    async init(): Promise<{ path: string }> {
        const payload = await run(initArgv(), this.options);
        return { path: str(payload["path"]) };
    }
}
