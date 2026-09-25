/* index-api.md §11's vocabulary, and the line between an error and a fault.
 *
 * THE EXIT CODE DECIDES WHICH KIND IT IS, NOT THE PAYLOAD. §10's contract is
 * three codes — `0` success, `1` user or store error, `2` internal failure —
 * and they mean different things to a caller: a `1` is something the reader
 * asked for that the store will not do, and a `2` is a bug in kb. A binding
 * that folded them together would make a crash look like a refusal, and a UI
 * would then tell a reader to fix input that was never the problem. `coboard`
 * draws the same line between an §11 error and a crash and for the same
 * reason.
 *
 * §11'S TABLE IS NOT THE WHOLE SET AND THIS FILE SAYS SO OUT LOUD. The table
 * is the API's vocabulary — what a store refuses — and the CLI is also a
 * command line, so it emits `usage` for an argument it cannot read and
 * `corrupt_log` for a log it cannot fold. Both are real exit-1 refusals and
 * neither is in §11. They are listed here, separately, rather than being
 * quietly folded into the table: a caller switching on §11's codes must not
 * have to guess which of them the CLI actually reaches, and a code that
 * appears in neither list is a code this binding has not been told about —
 * which is worth surfacing rather than mapping to something plausible.
 *
 * NO `vscode` AND NO THIRD-PARTY IMPORT. This module is loadable anywhere.
 */

/* §11, verbatim and in its order. */
export const SPEC_ERROR_CODES = [
    "not_found",
    "model_mismatch",
    "model_missing",
    "index_stale",
    "unsupported_mime",
    "fetch_failed",
    "store_locked",
    "collection_in_use",
] as const;

export type SpecErrorCode = (typeof SPEC_ERROR_CODES)[number];

/* Refusals the CLI makes that §11 has no row for, because they are about the
 * command line and the store's files rather than about the API's model. Kept
 * apart from the table above so that neither list can quietly absorb the
 * other. */
export const CLI_ERROR_CODES = [
    "usage",
    "unknown_command",
    "init_failed",
    "corrupt_log",
    /* The CLI spells a fault `internal` in its payload and also exits 2 for
     * it. It is named here so `isKnownCode` does not report it as a code this
     * binding has never heard of; it still arrives as a `KbCrash`, because the
     * exit code is what decides. */
    "internal",
] as const;

export type CliErrorCode = (typeof CLI_ERROR_CODES)[number];

export type KbErrorCode = SpecErrorCode | CliErrorCode;

export function isSpecErrorCode(code: string): code is SpecErrorCode {
    return (SPEC_ERROR_CODES as readonly string[]).includes(code);
}

export function isKnownCode(code: string): code is KbErrorCode {
    return isSpecErrorCode(code) || (CLI_ERROR_CODES as readonly string[]).includes(code);
}

/* A refusal: exit 1, with the store's own reason.
 *
 * `code` IS WHATEVER THE STORE SAID AND IS NEVER TRANSLATED. A binding that
 * mapped an unrecognised code onto `not_found` would answer a question nobody
 * asked and hide the one fact a caller needed. `spec` is the narrowing, and it
 * is null exactly when the code is not one of §11's — so a caller that
 * switches on §11 can, and one that wants the raw word still has it. */
export class KbError extends Error {
    readonly code: string;
    readonly spec: SpecErrorCode | null;
    /* True when the code is in neither list: a vocabulary this binding has not
     * been told about, which is a thing to report rather than to guess at. */
    readonly unrecognised: boolean;
    /* The command that produced it, for a message a reader can act on. */
    readonly argv: readonly string[];

    constructor(code: string, message: string, argv: readonly string[]) {
        super(message);
        this.name = "KbError";
        this.code = code;
        this.spec = isSpecErrorCode(code) ? code : null;
        this.unrecognised = !isKnownCode(code);
        this.argv = argv;
    }
}

/* A fault: exit 2, a binary that would not start, or an answer this layer
 * could not read.
 *
 * NOT AN §11 ERROR, AND DELIBERATELY NOT DRESSED AS ONE. §11's table is the
 * vocabulary a caller can act on, and a bug in kb is not in it. A surface
 * showing this should say that kb failed, not that the reader's question was
 * wrong. */
export class KbCrash extends Error {
    readonly argv: readonly string[];
    /* The process's exit status, or null when it never ran — a missing binary,
     * a permission denied, a spawn that failed before exec. */
    readonly exitCode: number | null;
    /* Whatever the process wrote to stderr, trimmed. The one place a reader
     * can look when the payload was unreadable. */
    readonly stderr: string;

    constructor(message: string, argv: readonly string[], exitCode: number | null, stderr: string) {
        super(message);
        this.name = "KbCrash";
        this.argv = argv;
        this.exitCode = exitCode;
        this.stderr = stderr;
    }
}

export function isKbError(e: unknown): e is KbError {
    return e instanceof KbError;
}

export function isKbCrash(e: unknown): e is KbCrash {
    return e instanceof KbCrash;
}
