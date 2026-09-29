/* The half of `kb-js` that is a pure function of the store's answers.
 *
 * WHY THERE IS A SECOND ENTRY POINT. The main one reaches `run.ts`, which
 * imports `node:child_process` — a browser document cannot spawn anything, so
 * a webview that imported `kb-js` would either fail to bundle or bundle a
 * shim that fails at run time with a message about a module rather than about
 * the mistake. `index-vscode`'s bundler refuses `node:*` by name and the
 * refusal names the file that reached for it, which is how this entry point
 * came to exist rather than being planned.
 *
 * WHAT IS HERE IS EXACTLY WHAT A RENDERER NEEDS: the shapes, so a row can be
 * typed; the readers, so an answer off a `postMessage` wire can be turned back
 * into one; and §5's staleness, so §2's browse list can badge a row the store
 * has said nothing about. None of it touches a process, a file or a socket.
 *
 * THE TWO ENTRY POINTS ARE NOT TWO IMPLEMENTATIONS. Everything below is
 * re-exported from the main one as well, from the same source files. A caller
 * that has both may use either, and a test that checks one has checked the
 * other.
 */

export {
    DEFAULT_STALE_DAYS,
    NO_DATE,
    fetchedAtKey,
    isStale,
    newestFirst,
    staleOf,
} from "./stale";

export { addressIndex, addressKey, documentAddress } from "./address";

export {
    arr,
    bool,
    num,
    obj,
    readChunk,
    readCollection,
    readBatchAdded,
    readDirAdded,
    readEmbedded,
    readFiled,
    readDocument,
    readHit,
    readLink,
    readLinkWritten,
    readLinks,
    readRefresh,
    readSource,
    readStaleList,
    readStats,
    str,
    strOrNull,
} from "./shape";

export {
    KbAdded,
    KbBatchAdded,
    KbEmbedded,
    KbFiled,
    KbChunk,
    KbChunkRead,
    KbCollection,
    KbDirAdded,
    KbDirSkipped,
    KbDocument,
    KbDocumentRead,
    KbForgotten,
    KbFetch,
    KbSourceRead,
    KbSourceRefreshed,
    KbCollectionStats,
    KbHit,
    KbEdge,
    KbLink,
    KbLinkWritten,
    KbLinks,
    KbRefresh,
    KbRefreshSource,
    KbScores,
    KbSource,
    KbStaleList,
    KbStats,
    KbModelConfig,
    KbModelStatus,
    KbStatus,
    LINK_TYPES,
    LinkType,
    RETRIEVAL_PATHS,
    RetrievalPath,
    SourceKind,
    isLinkType,
} from "./types";

/* §11's vocabulary travels too: a surface showing a refusal shows its code, and
 * `isSpecErrorCode` is how it tells one of §11's from one of the CLI's. The
 * error CLASSES are deliberately not here — an `instanceof` across a
 * `postMessage` boundary is meaningless, so the wire carries the envelope and
 * the renderer reads it as data. */
export {
    CLI_ERROR_CODES,
    CliErrorCode,
    KbErrorCode,
    KbErrorDetails,
    SPEC_ERROR_CODES,
    SpecErrorCode,
    isKnownCode,
    isSpecErrorCode,
} from "./errors";
