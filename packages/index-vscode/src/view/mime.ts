/* index-ui.md §3.2: "Markdown as Markdown, HTML as sanitized prose, source code
 * with syntax highlighting, everything else as plain text."
 *
 * FOUR ANSWERS AND A DEFAULT THAT IS NEVER "NOTHING". A mime nobody recognises
 * is plain text, because a document the store holds and the reader cannot see
 * is worse than one rendered without its formatting — and a fifth branch
 * returning null is indistinguishable from a bug.
 *
 * THE LANGUAGE IS DECIDED HERE AND NOT IN THE HIGHLIGHTER. `code.ts` colours a
 * language; this says which one a mime is, so the mapping is in one place and a
 * mime the store starts emitting is one line rather than a search.
 */

export type Rendering = "markdown" | "html" | "code" | "text";

/* The language families `code.ts` can colour. They are FAMILIES rather than
 * languages: the highlighter is a lexer over comments, strings, numbers and a
 * keyword set, and what actually differs between C and Rust at that level is
 * the keyword set and whether `#` starts a comment. */
export type Language = "c-family" | "script" | "hash" | "sql" | "data" | "none";

/* `text/x-c` and friends are what `kb add`'s own table emits (`cmd_add.c`), so
 * the two lists have to agree — `guards.test.ts` reads that table and checks
 * every mime it can produce is one this file has an answer for. A mime the
 * store can file and this cannot place would be a source document rendered as
 * prose, which is the quiet half of getting it wrong. */
const CODE: Readonly<Record<string, Language>> = {
    "text/x-c": "c-family",
    "text/x-c++": "c-family",
    "text/x-java": "c-family",
    "text/x-go": "c-family",
    "text/x-rust": "c-family",
    "text/x-csharp": "c-family",
    "text/javascript": "script",
    "application/javascript": "script",
    "application/typescript": "script",
    "text/x-typescript": "script",
    "text/x-typec": "script",
    "text/x-python": "hash",
    "text/x-ruby": "hash",
    "application/x-sh": "hash",
    "text/x-shellscript": "hash",
    "text/x-yaml": "hash",
    "application/yaml": "hash",
    "text/x-sql": "sql",
    "application/json": "data",
    "text/x-css": "data",
    "text/css": "data",
};

export function renderingFor(mime: string): Rendering {
    const m = mime.trim().toLowerCase();
    if (m === "text/markdown" || m === "text/x-markdown") {
        return "markdown";
    }
    if (m === "text/html" || m === "application/xhtml+xml") {
        return "html";
    }
    if (m in CODE) {
        return "code";
    }
    return "text";
}

export function languageFor(mime: string): Language {
    return CODE[mime.trim().toLowerCase()] ?? "none";
}

/* A short human name for the mime, for the header's facts strip. The full mime
 * is still shown — §3.1 asks for it by name — and this is the word beside it,
 * because `application/typescript` tells a reader less about what they are
 * about to read than `TypeScript` does. */
const LABELS: Readonly<Record<string, string>> = {
    "text/markdown": "Markdown",
    "text/html": "HTML",
    "text/plain": "Plain text",
    "application/json": "JSON",
    "application/typescript": "TypeScript",
    "text/javascript": "JavaScript",
    "text/x-c": "C",
    "text/x-c++": "C++",
    "text/x-python": "Python",
    "text/x-rust": "Rust",
    "text/x-go": "Go",
    "text/x-java": "Java",
    "text/x-sql": "SQL",
    "application/x-sh": "Shell",
    "text/x-css": "CSS",
    "text/x-typec": "Type-C",
};

export function mimeLabel(mime: string): string | null {
    return LABELS[mime.trim().toLowerCase()] ?? null;
}
