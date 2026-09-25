/* index-ui.md §3.2's "source code with syntax highlighting", as a lexer that
 * answers tokens rather than markup.
 *
 * WHY THERE IS A LEXER HERE AT ALL. §3.2 asks for highlighting and this package
 * may add no third-party runtime dependency beyond what `coboard-vscode`
 * already justifies — react, react-dom, baukasten, and the markdown renderer
 * they bring. coboard highlights nothing, so there is no highlighter in the
 * set to follow, and the honest options were to drop the requirement or to
 * write the small thing that meets it. This is the small thing.
 *
 * IT IS FOUR CATEGORIES AND NOT A GRAMMAR. Comments, strings, numbers and a
 * keyword set are what a reader's eye actually uses to find the shape of a
 * function in a page of code; the rest of a syntax highlighter is colour for
 * its own sake. Four categories is also the amount that can be got right for
 * six language families at once and be TESTED — a partial grammar per language
 * would be six things that are each subtly wrong.
 *
 * IT ANSWERS TOKENS, NEVER MARKUP. Every renderer in this package turns data
 * into ELEMENTS, and a highlighter that emitted `<span class=...>` strings
 * would be the one place that did not — the exact shape that needs
 * `dangerouslySetInnerHTML` on the other side, which `guards.test.ts` refuses
 * by name. A token is `{kind, text}` and React makes a span of it.
 *
 * NO vscode IMPORT, NO DOM: a pure function of a string.
 */

import { Language } from "./mime";

export type TokenKind = "plain" | "comment" | "string" | "number" | "keyword";

export interface Token {
    readonly kind: TokenKind;
    readonly text: string;
}

/* The keyword sets. Small on purpose: the words that carry the CONTROL FLOW
 * and the declarations, which is what a reader is scanning for. A set that
 * tried to be complete would colour half the page and highlight nothing. */
const KEYWORDS: Readonly<Record<Language, readonly string[]>> = {
    "c-family": [
        "if", "else", "for", "while", "do", "switch", "case", "default", "break",
        "continue", "return", "goto", "struct", "union", "enum", "typedef",
        "static", "const", "void", "int", "char", "unsigned", "signed", "sizeof",
        "class", "public", "private", "protected", "virtual", "template",
        "namespace", "new", "delete", "try", "catch", "throw", "func", "package",
        "import", "type", "var", "fn", "let", "mut", "impl", "trait", "match",
        "pub", "use", "self", "where", "unsafe", "extern", "true", "false", "null",
        "nullptr", "NULL", "bool", "size_t",
    ],
    script: [
        "if", "else", "for", "while", "do", "switch", "case", "default", "break",
        "continue", "return", "function", "const", "let", "var", "class",
        "extends", "new", "delete", "typeof", "instanceof", "try", "catch",
        "finally", "throw", "async", "await", "yield", "import", "export",
        "from", "as", "interface", "type", "enum", "implements", "readonly",
        "private", "public", "protected", "static", "of", "in", "this", "super",
        "true", "false", "null", "undefined", "void", "never", "unknown", "any",
    ],
    hash: [
        "if", "elif", "else", "for", "while", "in", "def", "class", "return",
        "import", "from", "as", "try", "except", "finally", "raise", "with",
        "lambda", "yield", "pass", "break", "continue", "global", "nonlocal",
        "and", "or", "not", "is", "None", "True", "False", "then", "fi", "do",
        "done", "case", "esac", "function", "local", "export", "echo",
    ],
    sql: [
        "select", "from", "where", "join", "left", "right", "inner", "outer",
        "on", "group", "order", "by", "having", "limit", "offset", "insert",
        "into", "values", "update", "set", "delete", "create", "table", "index",
        "drop", "alter", "primary", "key", "foreign", "references", "not",
        "null", "and", "or", "as", "distinct", "union", "all", "case", "when",
        "then", "else", "end", "begin", "commit", "rollback",
    ],
    data: [
        "true", "false", "null", "important", "and", "or", "not", "only",
    ],
    none: [],
};

/* Which comment openers a family has. `#` families have no block comment, and
 * giving them one would swallow the rest of a Python file at the first `/*` in
 * a docstring. */
interface Dialect {
    readonly line: readonly string[];
    readonly block: readonly [string, string] | null;
    /* Whether the language has an identifier-boundary keyword match at all.
     * SQL is case-insensitive and everything else is not, which is the one
     * per-language rule worth having. */
    readonly foldCase: boolean;
}

const DIALECTS: Readonly<Record<Language, Dialect>> = {
    "c-family": { line: ["//"], block: ["/*", "*/"], foldCase: false },
    script: { line: ["//"], block: ["/*", "*/"], foldCase: false },
    hash: { line: ["#"], block: null, foldCase: false },
    sql: { line: ["--"], block: ["/*", "*/"], foldCase: true },
    data: { line: ["//"], block: ["/*", "*/"], foldCase: false },
    none: { line: [], block: null, foldCase: false },
};

const IDENT_START = /[A-Za-z_$]/;
const IDENT = /[A-Za-z0-9_$]/;
const DIGIT = /[0-9]/;

/* A string opener. Backtick is included for the script family and is harmless
 * elsewhere: a language without template literals does not have one to open. */
const QUOTES = ['"', "'", "`"];

/* Tokenise a whole document.
 *
 * ONE PASS, NO BACKTRACKING, AND IT ALWAYS TERMINATES. The loop advances at
 * least one character on every iteration, including on an unterminated string
 * or comment — which is the case a highlighter meets on the first truncated
 * file it is given, and the case where a "scan to the closing quote" that
 * assumed one exists spins for ever on a sixty-megabyte blob. The unterminated
 * run is coloured to the end of the input and the pass finishes.
 *
 * ADJACENT TOKENS OF THE SAME KIND ARE MERGED, so a page of plain code is a
 * handful of DOM nodes rather than one per character. */
export function tokenize(source: string, language: Language): Token[] {
    const dialect = DIALECTS[language];
    const keywords = new Set(
        dialect.foldCase
            ? KEYWORDS[language].map((k) => k.toLowerCase())
            : KEYWORDS[language],
    );
    const out: Token[] = [];
    const push = (kind: TokenKind, text: string): void => {
        if (text.length === 0) {
            return;
        }
        const last = out[out.length - 1];
        if (last !== undefined && last.kind === kind) {
            out[out.length - 1] = { kind, text: last.text + text };
            return;
        }
        out.push({ kind, text });
    };

    let i = 0;
    const n = source.length;
    while (i < n) {
        const c = source[i];

        /* A block comment, which is the one construct that can span lines. */
        if (dialect.block !== null && source.startsWith(dialect.block[0], i)) {
            const close = source.indexOf(dialect.block[1], i + dialect.block[0].length);
            const end = close === -1 ? n : close + dialect.block[1].length;
            push("comment", source.slice(i, end));
            i = end;
            continue;
        }

        const lineOpener = dialect.line.find((o) => source.startsWith(o, i));
        if (lineOpener !== undefined) {
            const nl = source.indexOf("\n", i);
            const end = nl === -1 ? n : nl;
            push("comment", source.slice(i, end));
            i = end;
            continue;
        }

        if (QUOTES.includes(c)) {
            let j = i + 1;
            while (j < n) {
                if (source[j] === "\\") {
                    j += 2;
                    continue;
                }
                if (source[j] === c) {
                    j += 1;
                    break;
                }
                /* A single-quoted or double-quoted string does not cross a
                 * newline in any of these languages, and an unterminated one
                 * that did would colour the rest of the file. A backtick does,
                 * which is what a template literal is. */
                if (source[j] === "\n" && c !== "`") {
                    break;
                }
                j += 1;
            }
            push("string", source.slice(i, Math.min(j, n)));
            i = Math.min(Math.max(j, i + 1), n);
            continue;
        }

        if (DIGIT.test(c)) {
            let j = i;
            /* Hex, binary, exponents, separators and suffixes, as one run of
             * "things that belong to a number". Getting the grammar exactly
             * right per language buys nothing a reader can see. */
            while (j < n && /[0-9a-fA-FxXbBoO._']/.test(source[j])) {
                j += 1;
            }
            push("number", source.slice(i, j));
            i = j;
            continue;
        }

        if (IDENT_START.test(c)) {
            let j = i;
            while (j < n && IDENT.test(source[j])) {
                j += 1;
            }
            const word = source.slice(i, j);
            const probe = dialect.foldCase ? word.toLowerCase() : word;
            push(keywords.has(probe) ? "keyword" : "plain", word);
            i = j;
            continue;
        }

        push("plain", c);
        i += 1;
    }
    return out;
}

/* The tokens of a document, split into lines, so the renderer can draw line
 * numbers without re-splitting somebody else's tokens.
 *
 * A TOKEN IS SPLIT AT A NEWLINE RATHER THAN ALLOWED TO SPAN ONE. A block
 * comment crosses lines by nature, and a span that crossed a line boundary in
 * a numbered gutter would put the number in the middle of the comment. */
export function lines(tokens: readonly Token[]): Token[][] {
    const out: Token[][] = [[]];
    for (const token of tokens) {
        const parts = token.text.split("\n");
        parts.forEach((part, index) => {
            if (index > 0) {
                out.push([]);
            }
            if (part.length > 0) {
                out[out.length - 1].push({ kind: token.kind, text: part });
            }
        });
    }
    return out;
}
