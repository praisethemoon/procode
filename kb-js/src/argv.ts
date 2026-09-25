/* Every command line this package can build, as pure functions of their
 * arguments.
 *
 * ARGUMENTS ARE ARRAY ELEMENTS AND NEVER TEXT IN A COMMAND STRING. This is the
 * whole of why the module exists separately from the one that spawns. A query
 * a reader typed reaches the store exactly as they typed it — `io_uring &&
 * rm -rf ~` is one argument containing spaces and ampersands, not three
 * commands — and the only way that stays true is for no part of this package
 * to ever concatenate a value into a line. `run.ts` passes the array straight
 * to `execFile`, which takes an argv and no shell; `guards.test.ts` refuses
 * `exec(`, `execSync(`, `shell: true` and a backticked command anywhere in
 * `src/`.
 *
 * A pure function of its input is also a thing a test can call, and every flag
 * spelling in this file is a claim about the CLI that a test can check against
 * `kb --help`.
 *
 * `--json` IS APPENDED BY `run.ts` AND NOT HERE, exactly once, so no builder
 * can forget it and none can add a second.
 *
 * NOTHING IN THIS FILE READS THE DISK OR SPAWNS ANYTHING.
 */

import { StoreSelector } from "./types";

/* An empty or whitespace-only value is not a filter and is dropped. `kb ls
 * --collection ""` asks for documents whose collection is the empty string,
 * which is nobody's question — the same rule coboard's filter bar follows. */
function put(argv: string[], flag: string, value: string | null | undefined): void {
    if (typeof value !== "string") {
        return;
    }
    const trimmed = value.trim();
    if (trimmed.length === 0) {
        return;
    }
    argv.push(flag, trimmed);
}

function putNumber(argv: string[], flag: string, value: number | null | undefined): void {
    if (typeof value !== "number" || !Number.isFinite(value)) {
        return;
    }
    argv.push(flag, String(value));
}

function putStore(argv: string[], store: StoreSelector | null | undefined): void {
    if (store === undefined || store === null) {
        return;
    }
    argv.push("--store", store);
}

/* ------------------------------------------------------------ the reads */

export interface LsOptions {
    collection?: string | null;
    source?: string | null;
    mime?: string | null;
    /* An ISO-8601 instant. The store compares these as strings — a fixed
     * layout sorts lexicographically — so anything else silently filters
     * nothing rather than failing. */
    since?: string | null;
    limit?: number | null;
    store?: StoreSelector | null;
}

export function lsArgv(options: LsOptions = {}): string[] {
    const argv = ["ls"];
    put(argv, "--collection", options.collection);
    put(argv, "--source", options.source);
    put(argv, "--mime", options.mime);
    put(argv, "--since", options.since);
    putNumber(argv, "--limit", options.limit);
    putStore(argv, options.store);
    return argv;
}

export interface GetOptions {
    text?: boolean;
    chunks?: boolean;
    /* §2's third: `?include=text,chunks,links`. A document's edges alongside
     * its content, so a reader following §6's layer does not pay a second call
     * for the one question that follows from the first. */
    links?: boolean;
    store?: StoreSelector | null;
}

export function getArgv(id: string, options: GetOptions = {}): string[] {
    const argv = ["get", id];
    /* One `--include` carrying a comma list, which is the route's own spelling
     * (`?include=text,chunks`). Two `--include` flags would leave the CLI's
     * last-wins parsing deciding which of them counted. */
    const include: string[] = [];
    if (options.text === true) {
        include.push("text");
    }
    if (options.chunks === true) {
        include.push("chunks");
    }
    if (options.links === true) {
        include.push("links");
    }
    if (include.length > 0) {
        argv.push("--include", include.join(","));
    }
    putStore(argv, options.store);
    return argv;
}

export function collectionsArgv(store?: StoreSelector | null): string[] {
    const argv = ["collections"];
    putStore(argv, store);
    return argv;
}

export function statusArgv(): string[] {
    return ["status"];
}

/* ------------------------------------------------------------ the search */

/* §4's parameters, each as the flag of the same name.
 *
 * THE QUERY IS POSITIONAL, WHICH IS THE ONE CHOICE HERE THAT IS NOT READ OFF
 * THE ROUTE. `kb get D-241` takes its subject positionally and every filter as
 * a flag, and search has the same shape: one subject, several narrowings. It
 * is also the spelling that cannot be confused with a filter — `kb search
 * --collection x` with no query is visibly missing its subject, where
 * `--q ""` is not.
 *
 * A QUERY THAT BEGINS WITH A DASH IS STILL A QUERY. `--` ends the flags for
 * every command (`cmd_common.c`'s parser honours it), so it is written before
 * the subject whenever the subject could be read as a flag. Not always: an
 * unconditional `--` would be a second thing for a reader of a log line to
 * decode, and the CLI's own help writes neither.
 *
 * `minScore` IS `--min-score`. §4 names the parameter in camel case because it
 * is a query string; every flag the CLI has is lower case with dashes, and a
 * `--minScore` would be the only one that was not. */
export type SearchMode = "hybrid" | "semantic" | "keyword";

export interface SearchOptions {
    /* §4: a comma list. Passed as one flag carrying the list, the way
     * `--include` carries its own. */
    collection?: readonly string[] | string | null;
    mode?: SearchMode | null;
    /* §4: "hits, max 100". Not clamped here — the store owns its own limits
     * and a binding that silently clamped would hide a caller's mistake. */
    k?: number | null;
    /* §4: "also return N neighbouring chunks". */
    expand?: number | null;
    store?: StoreSelector | null;
    source?: string | null;
    mime?: string | null;
    since?: string | null;
    minScore?: number | null;
}

export function searchArgv(query: string, options: SearchOptions = {}): string[] {
    const argv = ["search"];
    const collection = Array.isArray(options.collection)
        ? options.collection.map((c) => c.trim()).filter((c) => c.length > 0).join(",")
        : options.collection;
    put(argv, "--collection", typeof collection === "string" ? collection : null);
    put(argv, "--mode", options.mode);
    putNumber(argv, "--k", options.k);
    putNumber(argv, "--expand", options.expand);
    putStore(argv, options.store);
    put(argv, "--source", options.source);
    put(argv, "--mime", options.mime);
    put(argv, "--since", options.since);
    putNumber(argv, "--min-score", options.minScore);
    /* The subject last, behind `--` when it could be read as a flag. The query
     * is NOT trimmed away when it is blank: an empty search is the caller's
     * mistake to be told about by the store, not one this layer covers up by
     * sending a different command. */
    if (query.startsWith("-")) {
        argv.push("--");
    }
    argv.push(query);
    return argv;
}

export function chunkArgv(id: string, options: { expand?: number | null; store?: StoreSelector | null } = {}): string[] {
    const argv = ["chunk", id];
    putNumber(argv, "--expand", options.expand);
    putStore(argv, options.store);
    return argv;
}

/* ------------------------------------------------------------- the writes */

/* §2's `POST /documents`: the caller already has the text and hands it over
 * rather than causing a second fetch.
 *
 * THE CONTENT DOES NOT TRAVEL IN THE ARGV. It goes down stdin — `--file -` —
 * because an argument list has a hard size limit that a document reaches long
 * before a person notices, and because a page's text in a process listing is
 * not where a page's text belongs. `meta` travels the same way it is spelled:
 * `--meta` takes a JSON object as one argument, and one argument is safe
 * whatever is in it. */
export interface AddOptions {
    title: string;
    collection: string;
    url?: string | null;
    mime?: string | null;
    meta?: Readonly<Record<string, unknown>> | null;
    store?: Exclude<StoreSelector, "all"> | null;
}

export function addArgv(options: AddOptions): string[] {
    const argv = ["add"];
    argv.push("--title", options.title);
    argv.push("--collection", options.collection);
    put(argv, "--url", options.url);
    put(argv, "--mime", options.mime);
    if (options.meta !== undefined && options.meta !== null) {
        argv.push("--meta", JSON.stringify(options.meta));
    }
    putStore(argv, options.store);
    /* stdin, always. A caller handing over a path instead would be asking the
     * store to read a file this process has already read, which is one more
     * thing that can disagree about what was filed. */
    argv.push("--file", "-");
    return argv;
}

/* §5's `GET /stale` and `POST /refresh`, and §7's two collection writes.
 *
 * NONE OF THESE COMMANDS EXIST IN THE CLI YET, and that is recorded here
 * rather than left for whoever calls one to discover. Each is the route from
 * the specification spelled in the CLI's own idiom — a verb, its subject
 * positionally, its narrowings as flags — and a caller that reaches one today
 * gets a `usage` or `unknown_command` refusal, which is what the surfaces
 * above turn into a sentence rather than into a silence. The value of building
 * them now is that the day the commands land there is one file to reconcile,
 * and it is this one.
 *
 * `--older-than` TAKES §5'S OWN SPELLING, `90d`, because that is what the
 * route's query string carries and a duration written two ways is a duration
 * somebody gets wrong. */
export interface StaleOptions {
    olderThan?: string | null;
    collection?: string | null;
    store?: StoreSelector | null;
}

export function staleArgv(options: StaleOptions = {}): string[] {
    const argv = ["stale"];
    put(argv, "--older-than", options.olderThan);
    put(argv, "--collection", options.collection);
    putStore(argv, options.store);
    return argv;
}

/* §5's `POST /refresh?collection=&olderThan=`, AND NOTHING ELSE ON IT.
 *
 * `--document` AND `--source` WERE HERE AND WERE WRONG. They were written
 * against a route that does not exist: §5's refresh takes a collection and a
 * threshold, and `kb refresh` refuses either flag with `usage: unknown
 * option`. The reason they were invented is worth keeping, because it is the
 * mistake rather than the fix — §3.1 of the UI spec wants a per-document
 * refresh action, and a filter on a scope-wide route looked like the cheapest
 * way to have one. It is not the same operation. §2 has the route for it,
 * `POST /sources/{id}/refresh`, which refetches ONE source and compares by
 * hash; a `--source` narrowing on this command would be a third thing that is
 * neither, and would answer a report about one source while looking like it
 * had refetched it.
 *
 * SO IT IS NOT BEING ASKED FOR HERE. If §3.1's action is built, the thing to
 * add is `kb refresh <S-n>` — §2's own route, spelled as its own subject —
 * and this function is not where it goes.
 *
 * `--store` IS REAL AND IS HONOURED (`cmd_stale.c`'s `REFRESH_FLAGS` lists
 * it), and it takes `all`: a report about what has gone stale spans both tiers
 * for the same reason a search does. */
export interface RefreshOptions {
    collection?: string | null;
    olderThan?: string | null;
    store?: StoreSelector | null;
}

export function refreshArgv(options: RefreshOptions = {}): string[] {
    const argv = ["refresh"];
    put(argv, "--collection", options.collection);
    put(argv, "--older-than", options.olderThan);
    putStore(argv, options.store);
    return argv;
}

/* §7's `PATCH /collections/{name}` and `DELETE /collections/{name}`, as
 * subcommands of the command that lists them — `kb collections rename a b`.
 * A top-level `kb rename-collection` would put two words for one noun at the
 * top of `kb --help`, where the noun already has a command. */
export function renameCollectionArgv(
    from: string,
    to: string,
    store?: Exclude<StoreSelector, "all"> | null,
): string[] {
    const argv = ["collections", "rename"];
    putStore(argv, store);
    /* Behind `--`, always: a collection name is a name somebody chose and
     * `-x` is a legal one. Unconditional here and conditional for a search
     * query, because there are two subjects rather than one and a `--` in
     * front of only the first would be the confusing half-measure. */
    argv.push("--", from, to);
    return argv;
}

export function deleteCollectionArgv(
    name: string,
    store?: Exclude<StoreSelector, "all"> | null,
): string[] {
    const argv = ["collections", "delete"];
    putStore(argv, store);
    argv.push("--", name);
    return argv;
}

/* §6's read and its write, as subcommands of the noun they are about —
 * `kb links D-241` and `kb links add D-241 analogue_of D-7`, which is the
 * CLI's own spelling and is checked against `kb --help`.
 *
 * THE WRITE READS AS A SENTENCE — from, type, to — rather than as §6's
 * `{ from, to, type }` field order. Three bare ids and a word in a row are a
 * thing somebody has to get right at a terminal, and `D-241 analogue_of D-7`
 * is the only ordering of the three that can be read back to check.
 *
 * NO `--`, WHICH IS THE ONE PLACE THIS FILE DIFFERS FROM `collections rename`
 * ABOVE AND IS NOT A PREFERENCE. `cmd_links.c` counts its positional arguments
 * rather than routing them through the common parser, so a `--` is a fourth
 * argument to a command that takes three and the refusal says so. The argument
 * for `--` is that a value must not reach a parser as a flag; the answer here
 * is that these values are ids the store itself issued (§1.1: `D-<n>`), and a
 * spelling the binary rejects protects nothing. */
export function linksArgv(document: string, store?: StoreSelector | null): string[] {
    const argv = ["links", document];
    putStore(argv, store);
    return argv;
}

export function linkArgv(
    from: string,
    type: string,
    to: string,
    store?: Exclude<StoreSelector, "all"> | null,
): string[] {
    const argv = ["links", "add", from, type, to];
    putStore(argv, store);
    return argv;
}

/* §7's `GET /stats`: "per-collection document, chunk and byte counts".
 *
 * A SEPARATE COMMAND FROM `collections`, because the CLI made them two. The
 * two rows overlap and neither contains the other: `collections` carries
 * `oldestFetchedAt`, which is §5's freshness question, and `stats` carries
 * `chunks` and a store-wide total. A caller that wants the whole picture asks
 * both, and this is the second half. */
export function statsArgv(store?: StoreSelector | null): string[] {
    const argv = ["stats"];
    putStore(argv, store);
    return argv;
}

export function initArgv(store?: Exclude<StoreSelector, "all"> | null): string[] {
    const argv = ["init"];
    putStore(argv, store);
    return argv;
}
