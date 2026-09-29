/* The pure view layer: §2's rows, §3.1's facts, §3.2's dispatch and its two
 * renderers, and §4's collection rows.
 *
 * All of them are functions of their input, which is why they are in
 * `src/view/` and not inside a component: the webview bundle and these tests
 * compile the same sources, so the behaviour pinned here is the behaviour that
 * ships.
 */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import { KbDocument, KbHit, KbStatus } from "kb-js/pure";

import {
    BROWSE_LIMIT,
    browseCut,
    LINE_LIMIT,
    browseQuery,
    browseRows,
    isSearching,
    leftToEmbed,
    oneLine,
    pendingLine,
    searchQuery,
    searchRows,
    unembeddedNote,
} from "../view/rows";
import {
    collectionRows,
    documentFacts,
    formatBytes,
    formatDate,
    formatMetaValue,
    metaEntries,
    oldestFetch,
} from "../view/facts";
import { headingId, headingText } from "../view/headings";
import { anchorSlug, followLink, headingMatches } from "../view/links";
import { markTerms, scoreLabel, searchPageQuery, sectionOf, toggleCollection } from "../view/search";
import { languageFor, mimeLabel, renderingFor } from "../view/mime";
import { lines, tokenize } from "../view/code";
import { RENDERABLE_TAGS, decodeEntities, parseHtml, safeHref } from "../view/html";

/* ------------------------------------------------------------ fixtures */

function doc(over: Partial<KbDocument> = {}): KbDocument {
    return {
        id: "D-1",
        source: "S-1",
        collection: "win32-iocp",
        path: "",
        title: "I/O Completion Ports",
        mime: "text/markdown",
        locator: "https://learn.microsoft.test/win32/iocp",
        contentHash: "a".repeat(64),
        bytes: 4096,
        fetchedAt: "2026-06-01T09:15:00Z",
        indexedAt: "2026-06-01T09:15:00Z",
        chunkCount: 3,
        chunkBase: 99810,
        meta: {},
        ...over,
    };
}

function hit(over: Partial<KbHit> = {}): KbHit {
    return {
        chunk: "C-99812",
        document: "D-1",
        source: "S-1",
        title: "I/O Completion Ports",
        heading: "Creating a completion port",
        snippet: "CreateIoCompletionPort associates an open file handle with a port.",
        collection: "win32-iocp",
        matched: ["keyword", "semantic"],
        scores: { bm25: 11.25, vector: 0.82, fused: 0.031 },
        fetchedAt: "2026-06-01T09:15:00Z",
        stale: false,
        ...over,
    };
}

const NOW = Date.parse("2026-09-25T00:00:00Z");

/* ----------------------------------------------------- §2: the two lists */

test("with no query the list is every document in scope", () => {
    /* §2: "browsing is the default state, not an empty prompt". This is the
     * property that regresses into a "type to search" placeholder the first
     * time somebody simplifies the loading path. */
    assert.equal(isSearching(""), false);
    assert.equal(isSearching("   "), false, "whitespace is not a query");
    assert.equal(isSearching(" io_uring "), true);

    const rows = browseRows([doc({ id: "D-1" }), doc({ id: "D-2" })], NOW, 90);
    assert.equal(rows.length, 2, "the browse list dropped documents");
});

test("the browse list is newest first, and an undated row sorts last", () => {
    const rows = browseRows(
        [
            doc({ id: "D-1", fetchedAt: "2024-01-01T00:00:00Z" }),
            doc({ id: "D-2", fetchedAt: "" }),
            doc({ id: "D-3", fetchedAt: "2026-06-01T00:00:00Z" }),
        ],
        NOW,
        90,
    );
    assert.deepEqual(rows.map((r) => r.reference), ["D-3", "D-1", "D-2"]);
});

test("the browse query never sends a blank collection", () => {
    /* A blank `collection=` filters for the empty-string collection, which is
     * nobody's question. */
    assert.deepEqual(browseQuery(""), { limit: BROWSE_LIMIT + 1 });
    assert.deepEqual(browseQuery("   "), { limit: BROWSE_LIMIT + 1 });
    assert.equal(browseQuery(" papers ").collection, "papers");
    assert.equal("collection" in searchQuery(""), false);
});

test("a browse list that was cut knows it, and asking for all sends no limit", () => {
    const many = Array.from({ length: BROWSE_LIMIT + 1 }, (_, i) => i);
    assert.deepEqual(browseCut(many), { shown: many.slice(0, BROWSE_LIMIT), more: true });
    assert.deepEqual(browseCut(many.slice(0, BROWSE_LIMIT)).more, false);
    assert.deepEqual(browseQuery("papers", true), { collection: "papers" });
});

test("a search row keeps the store's order, its verdict and its matched paths", () => {
    /* §4 fuses the two retrieval paths by reciprocal rank; re-sorting here
     * would throw away the only thing the search computed. And the store's
     * `stale` wins, so the two paths cannot drift apart. */
    const rows = searchRows(
        [
            hit({ document: "D-9", fetchedAt: "2020-01-01T00:00:00Z", stale: false }),
            hit({ document: "D-1", matched: ["semantic"], stale: true }),
        ],
        NOW,
        90,
    );
    assert.deepEqual(rows.map((r) => r.reference), ["D-9", "D-1"]);
    assert.equal(rows[0].stale, false, "the store said fresh and this overruled it");
    assert.deepEqual([...rows[1].matched], ["semantic"]);
    assert.equal(rows[0].chunk, "C-99812", "a search row carries the chunk §3.2 scrolls to");
});

test("a browsed row has no matched paths and no chunk", () => {
    /* §2: "which retrieval paths matched, WHEN the row came from a search".
     * The "when" is only checkable because a browsed row is empty here. */
    const [row] = browseRows([doc()], NOW, 90);
    assert.deepEqual([...row.matched], []);
    assert.equal(row.chunk, null);
});

test("a browsed row computes staleness and a hit does not", () => {
    const [old] = browseRows([doc({ fetchedAt: "2020-01-01T00:00:00Z" })], NOW, 90);
    assert.equal(old.stale, true);
    const [fresh] = browseRows([doc({ fetchedAt: "2026-09-01T00:00:00Z" })], NOW, 90);
    assert.equal(fresh.stale, false);
});

/* --------------------------------------------------- §2: the clamped line */

test("the line is one line, whatever the snippet contained", () => {
    /* §2 asks for ONE line. A snippet spanning a heading boundary genuinely
     * contains newlines; left alone they make the row three lines tall, or —
     * with the CSS clamp on — hide everything after the first. */
    assert.equal(oneLine("a\nb\tc\r\nd"), "a b c d");
    assert.equal(oneLine("  spaced   out  "), "spaced out");
    assert.equal(oneLine("\u0000bell\u0007here"), "bell here");
});

test("the line is clamped and the full text is what the hover gets", () => {
    const long = "word ".repeat(200);
    const [row] = searchRows([hit({ snippet: long })], NOW, 90);
    assert.ok(row.line.length <= LINE_LIMIT + 1, `the line is ${row.line.length} characters`);
    assert.ok(row.line.endsWith("…"), "a clamped line does not say it was clamped");
    assert.equal(row.lineFull, long, "the hover lost the full text");
    assert.notEqual(row.line, row.lineFull);
});

test("a short line is not clamped and is identical to its hover", () => {
    /* The surface only sets a title attribute when the two differ — a tooltip
     * repeating what is on screen teaches a reader to ignore tooltips. */
    const [row] = searchRows([hit({ snippet: "short" })], NOW, 90);
    assert.equal(row.line, "short");
    assert.equal(row.line, row.lineFull);
});

test("the line never carries markup, because it is never marked up", () => {
    /* §2: "rows are not highlighted and matches are not marked up". `oneLine`
     * answers a STRING — there is no shape here for a `<mark>` to be inserted
     * into — and whatever the document contained comes out as characters. */
    const [row] = searchRows([hit({ snippet: "<script>alert(1)</script> and <b>bold</b>" })], NOW, 90);
    assert.equal(row.line, "<script>alert(1)</script> and <b>bold</b>");
    assert.equal(typeof row.line, "string");
    assert.ok(!row.line.includes("<mark"), "something marked up a match");
});

test("a browsed row's line is the locator and says so", () => {
    /* The gap between the two specifications, made explicit rather than left
     * as an empty line: §2 asks every row for a snippet and `GET /documents`
     * carries none. */
    const [row] = browseRows([doc()], NOW, 90);
    assert.equal(row.lineKind, "locator");
    assert.equal(row.line, "https://learn.microsoft.test/win32/iocp");
    const [searched] = searchRows([hit()], NOW, 90);
    assert.equal(searched.lineKind, "snippet");
});

/* ------------------------------------------------------- §3.1: the facts */

test("the indexed date is shown only when it differs from the fetched one", () => {
    /* §3.1: "fetched date, and indexed date when they differ". They are the
     * same instant for almost every document, and a strip repeating one date
     * twice teaches a reader to stop reading it. Compared as DAYS, because
     * `kb add` writes them a second apart. */
    const same = documentFacts(
        doc({ fetchedAt: "2026-06-01T09:15:00Z", indexedAt: "2026-06-01T09:15:01Z" }),
    );
    assert.equal(same.some((f) => f.label === "Indexed"), false);

    const differs = documentFacts(
        doc({ fetchedAt: "2026-06-01T09:15:00Z", indexedAt: "2026-06-02T09:15:00Z" }),
    );
    const indexed = differs.find((f) => f.label === "Indexed");
    assert.ok(indexed !== undefined, "a re-index on another day is not shown");
    assert.equal(indexed.value, "2026-06-02");
});

test("every fact §3.1 names is in the strip", () => {
    const facts = documentFacts(doc());
    const labels = facts.map((f) => f.label);
    for (const needed of ["Collection", "Fetched", "Size", "Type", "Chunks"]) {
        assert.ok(labels.includes(needed), `§3.1 asks for ${needed} and the strip has no row for it`);
    }
});

test("the chunk count names the range, because that is what a citation carries", () => {
    const chunks = documentFacts(doc({ chunkCount: 3, chunkBase: 99810 })).find(
        (f) => f.label === "Chunks",
    );
    assert.equal(chunks?.value, "3");
    assert.equal(chunks?.title, "C-99810..C-99812");
    /* A document with no chunks has no range to name. */
    assert.equal(
        documentFacts(doc({ chunkCount: 0 })).find((f) => f.label === "Chunks")?.title,
        undefined,
    );
});

test("a date is a fixed ISO day and an unreadable one is shown rather than blanked", () => {
    /* A provenance block exists to be compared, and `01/02/26` is a different
     * day in two countries. */
    assert.equal(formatDate("2026-06-01T09:15:00Z"), "2026-06-01");
    assert.equal(formatDate("whenever"), "whenever");
    assert.equal(formatDate(""), "—");
});

test("bytes are binary units with their real names", () => {
    assert.equal(formatBytes(0), "0 B");
    assert.equal(formatBytes(1023), "1023 B");
    assert.equal(formatBytes(1024), "1.0 KiB");
    assert.equal(formatBytes(1024 * 1024 * 3.5), "3.5 MiB");
    assert.equal(formatBytes(-1), "—");
    assert.equal(formatBytes(Number.NaN), "—");
});

/* ------------------------------------------------- §3.1: free-form meta */

test("meta is shown as it is, and nothing is asserted about what should be", () => {
    /* index-api.md §1.2 makes it free-form per document. There is no
     * known-key list here and no key treated specially. */
    const entries = metaEntries({
        year: 2026,
        authors: ["Ada", "Grace"],
        section: { path: ["Win32", "IOCP"] },
        draft: false,
        note: null,
    });
    assert.deepEqual(
        entries,
        [
            { key: "authors", value: "Ada, Grace" },
            { key: "draft", value: "false" },
            { key: "note", value: "—" },
            { key: "section", value: '{"path":["Win32","IOCP"]}' },
            { key: "year", value: "2026" },
        ],
        "meta was reordered, filtered, or a shape was flattened to [object Object]",
    );
});

test("an object in meta is its JSON and never [object Object]", () => {
    assert.equal(formatMetaValue({ a: 1 }), '{"a":1}');
    assert.notEqual(formatMetaValue({ a: 1 }), "[object Object]");
    assert.equal(formatMetaValue([{ a: 1 }, "x"]), '{"a":1}, x');
});

test("an empty meta is an empty list rather than a heading with nothing under it", () => {
    assert.deepEqual(metaEntries({}), []);
});

/* ---------------------------------------------------- §4: the collections */

test("the oldest fetch date is derived per collection", () => {
    const oldest = oldestFetch([
        doc({ collection: "a", fetchedAt: "2026-01-01T00:00:00Z" }),
        doc({ collection: "a", fetchedAt: "2024-01-01T00:00:00Z" }),
        doc({ collection: "c", fetchedAt: "2025-01-01T00:00:00Z" }),
        doc({ collection: "b", fetchedAt: "" }),
    ]);
    assert.equal(oldest.get("a"), "2024-01-01T00:00:00Z");
    assert.equal(oldest.get("c"), "2025-01-01T00:00:00Z");
    assert.equal(oldest.get("b"), undefined, "an undated document invented a date");
});

test("a collection row carries the three facts §4 asks for", () => {
    const rows = collectionRows(
        [
            { name: "io-uring", sources: 1, documents: 3, chunks: 9, bytes: 4096 },
            { name: "win32-iocp", sources: 2, documents: 7, chunks: 41, bytes: 90210 },
        ],
        [doc({ collection: "win32-iocp", fetchedAt: "2024-05-05T00:00:00Z" })],
    );
    assert.deepEqual(rows.map((r) => r.name), ["io-uring", "win32-iocp"]);
    const iocp = rows[1];
    assert.equal(iocp.documents, 7);
    assert.equal(iocp.bytes, 90210);
    assert.equal(iocp.oldestFetch, "2024-05-05T00:00:00Z");
    /* A collection whose documents were not read has no date rather than a
     * wrong one. */
    assert.equal(rows[0].oldestFetch, "");
});

/* ------------------------------------------------------ §3.2: the dispatch */

test("every mime gets one of §3.2's four renderings, and the default is never nothing", () => {
    assert.equal(renderingFor("text/markdown"), "markdown");
    assert.equal(renderingFor("text/html"), "html");
    assert.equal(renderingFor("text/x-c"), "code");
    assert.equal(renderingFor("application/typescript"), "code");
    assert.equal(renderingFor("text/plain"), "text");
    for (const unknown of ["", "application/octet-stream", "video/mp4", "nonsense"]) {
        assert.equal(
            renderingFor(unknown),
            "text",
            `${JSON.stringify(unknown)} has no rendering, so the document would be invisible`,
        );
    }
    /* Case and whitespace are what a header carries, not what a table has. */
    assert.equal(renderingFor("  TEXT/MARKDOWN  "), "markdown");
});

test("a code mime names a language and everything else names none", () => {
    assert.equal(languageFor("text/x-c"), "c-family");
    assert.equal(languageFor("text/x-python"), "hash");
    assert.equal(languageFor("text/x-sql"), "sql");
    assert.equal(languageFor("text/plain"), "none");
    assert.equal(mimeLabel("application/typescript"), "TypeScript");
    assert.equal(mimeLabel("application/octet-stream"), null);
});

/* --------------------------------------------------- §3.2: the highlighter */

function kinds(source: string, language: Parameters<typeof tokenize>[1]): string[] {
    return tokenize(source, language).map((t) => `${t.kind}:${t.text}`);
}

test("the four categories, over a line of C", () => {
    /* ADJACENT TOKENS OF ONE KIND ARE ONE TOKEN — ` x = ` is a single run of
     * plain rather than five. A page of ordinary code is then a handful of DOM
     * nodes instead of one per character, which is the difference between a
     * source document that scrolls and one that does not. */
    assert.deepEqual(kinds('int x = 42; /* note */ "s"', "c-family"), [
        "keyword:int",
        "plain: x = ",
        "number:42",
        "plain:; ",
        "comment:/* note */",
        "plain: ",
        'string:"s"',
    ]);
});

test("a run of plain text is one token and not one per character", () => {
    /* Stated on its own because the assertion above would still pass if the
     * merge only worked for spaces: a hundred characters of prose in a comment
     * is one node, and the count is what a renderer pays. */
    const tokens = tokenize("abcdefghij".repeat(10), "none");
    assert.equal(tokens.length, 1);
});

test("a hash language has no block comment, so a docstring does not swallow the file", () => {
    /* Giving `#` languages a slash-star comment pair would colour the rest of
     * a Python file from the first slash-star in a docstring. */
    const tokens = tokenize("x = 1  # note\ny = /* not a comment */ 2", "hash");
    assert.equal(tokens.filter((t) => t.kind === "comment").length, 1);
});

test("an unterminated string or comment ends at the input and the pass finishes", () => {
    /* The case a highlighter meets on the first truncated file it is given,
     * and the case where "scan to the closing quote" spins for ever. */
    assert.deepEqual(kinds('"never closed', "script"), ["string:\"never closed"]);
    assert.deepEqual(kinds("/* never closed", "script"), ["comment:/* never closed"]);
    /* And a quote that a newline ends, which is what these languages do. */
    const tokens = tokenize('"unterminated\nnext', "script");
    assert.equal(tokens[0].kind, "string");
    assert.equal(tokens[0].text, '"unterminated');
});

test("an escaped quote does not end the string", () => {
    assert.deepEqual(kinds('"a\\"b"', "script"), ['string:"a\\"b"']);
});

test("SQL folds case and the other languages do not", () => {
    assert.equal(tokenize("SELECT", "sql")[0].kind, "keyword");
    assert.equal(tokenize("select", "sql")[0].kind, "keyword");
    assert.equal(tokenize("IF", "c-family")[0].kind, "plain", "C keywords are not case-folded");
});

test("a language with no keywords colours nothing but still tokenises", () => {
    const tokens = tokenize("anything at all", "none");
    assert.deepEqual(new Set(tokens.map((t) => t.kind)), new Set(["plain"]));
    assert.equal(tokens.map((t) => t.text).join(""), "anything at all");
});

test("tokenising never loses or invents a character", () => {
    /* The property that matters most: this is the document's own text, and a
     * lexer that dropped a byte would be showing somebody a passage that is
     * not the passage. */
    const sources = [
        'int main(void) { /* hi */ return 0; }\n',
        '# python\ndef f(x):\n    return "\\n".join([str(x)])\n',
        "SELECT * FROM t WHERE a = 'b' -- note\n",
        "",
        "\n\n\n",
        "€ ünïcödé ✓",
        "`template ${x}` // trailing",
    ];
    for (const language of ["c-family", "script", "hash", "sql", "data", "none"] as const) {
        for (const source of sources) {
            assert.equal(
                tokenize(source, language).map((t) => t.text).join(""),
                source,
                `${language} altered ${JSON.stringify(source)}`,
            );
        }
    }
});

test("lines split at the newline and keep the count", () => {
    /* A block comment crosses lines by nature; a span that crossed a boundary
     * in a numbered gutter would put the number inside the comment. */
    const rows = lines(tokenize("/* a\nb */\nx", "c-family"));
    assert.equal(rows.length, 3);
    assert.deepEqual(rows[0].map((t) => t.text), ["/* a"]);
    assert.deepEqual(rows[1].map((t) => t.text), ["b */"]);
    assert.deepEqual(rows[2].map((t) => t.text), ["x"]);
    /* A blank line is a row with no tokens, not a missing row: the gutter's
     * numbers have to stay in step with the file. */
    assert.deepEqual(lines(tokenize("a\n\nb", "none")).length, 3);
    assert.deepEqual(lines(tokenize("a\n\nb", "none"))[1], []);
});

/* ---------------------------------------------------------- §3.2: the HTML */

function textOf(nodes: ReturnType<typeof parseHtml>): string {
    return nodes
        .map((n) => (n.kind === "text" ? n.text : textOf([...n.children])))
        .join("");
}

function tags(nodes: ReturnType<typeof parseHtml>): string[] {
    const out: string[] = [];
    const walk = (list: ReturnType<typeof parseHtml>): void => {
        for (const n of list) {
            if (n.kind === "element") {
                out.push(n.tag);
                walk([...n.children]);
            }
        }
    };
    walk(nodes);
    return out;
}

test("prose survives and structure is kept", () => {
    const nodes = parseHtml("<h2>Title</h2><p>Some <strong>bold</strong> text.</p><ul><li>one</li></ul>");
    assert.deepEqual(tags(nodes), ["h2", "p", "strong", "ul", "li"]);
    assert.equal(textOf(nodes), "TitleSome bold text.one");
});

test("a script's content is dropped with the element", () => {
    /* A `<script>` body is not prose, and keeping the text would put
     * JavaScript in the middle of a paragraph. */
    const nodes = parseHtml("<p>before</p><script>alert('x')</script><p>after</p>");
    assert.deepEqual(tags(nodes), ["p", "p"]);
    assert.equal(textOf(nodes), "beforeafter");
    assert.ok(!textOf(nodes).includes("alert"));
    assert.equal(textOf(parseHtml("<style>p{color:red}</style>")), "");
});

test("an unknown element keeps its children and is not itself an element", () => {
    /* The refusal is BY CONSTRUCTION: a tag that is not in the three sets
     * produces no element, so nothing this parser has never heard of can
     * become one. */
    const nodes = parseHtml("<div><custom-thing><p>kept</p></custom-thing></div>");
    assert.deepEqual(tags(nodes), ["p"]);
    assert.equal(textOf(nodes), "kept");
});

test("no attribute survives except an anchor's href", () => {
    const nodes = parseHtml('<p style="color:red" onclick="steal()" class="x">t</p>');
    const [p] = nodes;
    assert.equal(p.kind, "element");
    assert.deepEqual(Object.keys(p), ["kind", "tag", "children"], "an attribute survived");
});

test("javascript: is refused because it is not on the allow list", () => {
    /* A deny list has to get `JaVaScRiPt:` and a leading NUL right; an allow
     * list does not. */
    for (const bad of [
        "javascript:alert(1)",
        "JaVaScRiPt:alert(1)",
        "\u0000javascript:alert(1)",
        "  javascript:alert(1)",
        "data:text/html,<script>",
        "vbscript:x",
        "file:///etc/passwd",
        "command:workbench.action.terminal.new",
    ]) {
        assert.equal(safeHref(bad), undefined, `${JSON.stringify(bad)} was permitted`);
    }
    for (const good of ["https://x.test/", "http://x.test/", "mailto:a@b.test", "/relative", "#frag"]) {
        assert.ok(safeHref(good) !== undefined, `${good} was refused`);
    }
    assert.equal(safeHref(undefined), undefined);
    assert.equal(safeHref("   "), undefined);
});

test("an anchor with an unusable href keeps only its text", () => {
    /* A control that looks like a link and goes nowhere is worse than the
     * words on their own. */
    const nodes = parseHtml('<a href="javascript:alert(1)">click me</a>');
    assert.deepEqual(tags(nodes), []);
    assert.equal(textOf(nodes), "click me");
    const ok = parseHtml('<a href="https://x.test/">go</a>');
    assert.deepEqual(tags(ok), ["a"]);
    assert.equal(ok[0].kind === "element" ? ok[0].href : null, "https://x.test/");
});

test("an unclosed element is closed at the end and a stray close is ignored", () => {
    /* Both are what a browser does and both are what real pages contain. The
     * alternative is a parser that throws away a document over one `</p>`. */
    assert.deepEqual(tags(parseHtml("<p>unclosed")), ["p"]);
    assert.equal(textOf(parseHtml("<p>unclosed")), "unclosed");
    assert.equal(textOf(parseHtml("</p>stray")), "stray");
    assert.deepEqual(tags(parseHtml("<b><i>x</b></i>")), ["b", "i"]);
});

test("a bare angle bracket in prose is a character", () => {
    assert.equal(textOf(parseHtml("a < b and c > d")), "a < b and c > d");
});

test("comments and doctypes are dropped whole", () => {
    assert.equal(textOf(parseHtml("<!doctype html><!-- hidden -->shown")), "shown");
    assert.equal(textOf(parseHtml("<!-- unterminated")), "");
});

test("entities are decoded, and an unknown one is left as characters", () => {
    assert.equal(decodeEntities("a &amp; b &lt;c&gt; &#65; &#x42; &nbsp;"), "a & b <c> A B \u00a0");
    assert.equal(decodeEntities("&thinsp;"), "&thinsp;", "an unknown entity was guessed at");
    assert.equal(decodeEntities("&#x110000;"), "&#x110000;", "a code point outside Unicode");
    assert.equal(decodeEntities("&#xD800;"), "&#xD800;", "a lone surrogate");
});

test("a doubly-encoded script tag stays text", () => {
    /* `&lt;script&gt;` decodes to `<script>` and must not then be parsed — the
     * decode happens on TEXT that has already been split out of the markup, so
     * there is no second pass for it to become an element in. */
    const nodes = parseHtml("<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>");
    assert.deepEqual(tags(nodes), ["p"]);
    assert.equal(textOf(nodes), "<script>alert(1)</script>");
});

test("every tag the parser can emit is one the renderer knows", () => {
    /* A tag the parser emits and the renderer cannot draw would be an element
     * silently missing from the page. */
    const emitted = new Set(
        tags(
            parseHtml(
                RENDERABLE_TAGS.map((t) => {
                    if (t === "br" || t === "hr") {
                        return `<${t}>`;
                    }
                    /* An anchor with no usable href is kept only for its text,
                     * which is the rule one test up — so the one that checks
                     * the renderer knows every tag has to give it one. */
                    return t === "a"
                        ? '<a href="https://x.test/">x</a>'
                        : `<${t}>x</${t}>`;
                }).join(""),
            ),
        ),
    );
    for (const tag of RENDERABLE_TAGS) {
        assert.ok(emitted.has(tag), `${tag} is renderable and the parser will not emit it`);
    }
});

test("deep nesting degrades to flat text rather than to a stack overflow", () => {
    const deep = "<p>".repeat(500) + "bottom" + "</p>".repeat(500);
    const nodes = parseHtml(deep);
    assert.equal(textOf(nodes).includes("bottom"), true);
    assert.ok(tags(nodes).length <= 64, "the depth cap did not hold");
});

/* -------------------------------------------------- §3.2: the scroll target */

test("one heading makes one id, from either side", () => {
    /* The renderer puts an id on each heading and the tab looks up the id made
     * from the chunk's `heading` string. Two slug functions that differed by a
     * hyphen would produce a scroll that silently did nothing. */
    assert.equal(headingId("Creating a completion port"), headingId("  Creating  a completion port "));
    assert.notEqual(
        headingId("io_uring_prep_recv"),
        headingId("io-uring-prep-recv"),
        "two identifiers one hyphen apart collided, which in this corpus is a scroll to the wrong section",
    );
    assert.equal(headingId(""), "");
    assert.equal(headingId("   "), "");
    assert.ok(headingId("a b").startsWith("kb-h-"), "an id can collide with the mount point");
});

test("a heading's text is followed through the elements inside it", () => {
    /* A markdown heading with a symbol name in backticks arrives as an element
     * whose text is one level down — and those are exactly the headings that
     * matter in this corpus. */
    assert.equal(headingText("plain"), "plain");
    assert.equal(headingText(["a", 1, "b"]), "a1b");
    assert.equal(
        headingText({ props: { children: ["Use ", { props: { children: "malloc" } }] } }),
        "Use malloc",
    );
    assert.equal(headingText(null), "");
    assert.equal(headingText(undefined), "");
});

/* ------------------------------------------------------- left to embed */

function status(vectors: KbStatus["vectors"], available: boolean, current: boolean | null): KbStatus {
    return {
        path: "/w/.kb",
        present: true,
        readable: true,
        olderThan: "90d",
        vectors,
        model: {
            recorded: null,
            available: available ? ({ path: "/m", bytes: 1 } as never) : null,
            current,
        },
    };
}

test("chunks are left to embed only when there is a model to embed them with", () => {
    assert.equal(leftToEmbed(status({ count: 88, missing: 812, current: false }, true, true)), 812);
    assert.equal(leftToEmbed(status({ count: 88, missing: 812, current: false }, true, null)), 812);
    // A keyword-only store: every chunk is missing and none is owed.
    assert.equal(leftToEmbed(status({ count: 0, missing: 900, current: false }, false, null)), 0);
    // Another model than the one recorded is a reindex, not an embed pass.
    assert.equal(leftToEmbed(status({ count: 88, missing: 812, current: false }, true, false)), 0);
    assert.equal(leftToEmbed(status({ count: 900, missing: 0, current: true }, true, true)), 0);
    // A kb that does not report vectors owes nothing it can be seen to owe.
    assert.equal(leftToEmbed(status(undefined, true, true)), 0);
});

test("the rail and a search say how many chunks are not embedded, in words that count", () => {
    assert.equal(pendingLine(1), "1 chunk is not embedded yet: searchable by keyword, not yet by meaning.");
    assert.match(pendingLine(812), /^812 chunks are not embedded yet/);
    assert.equal(unembeddedNote(undefined), null);
    assert.equal(unembeddedNote(0), null);
    assert.equal(unembeddedNote(812), "812 chunks are not embedded yet — semantic results may be missing them.");
    assert.equal(unembeddedNote(1), "1 chunk is not embedded yet — semantic results may be missing it.");
});

/* index-ui.md §3.2: a link in a filed document is followed only inside the store. */
const LINK_INDEX = new Map<string, string>([
    ["https://docs.example.com/guide", "D-1"],
    ["https://docs.example.com/api/files", "D-2"],
    ["file:///home/me/proj/src/util.h", "D-3"],
]);

test("a bare fragment, or a link back to this page, scrolls here", () => {
    const base = "https://docs.example.com/guide/";
    assert.deepEqual(followLink("#Set%20up", base, LINK_INDEX, "D-1"), { kind: "here", fragment: "Set up" });
    assert.deepEqual(followLink("index.html#faq", base, LINK_INDEX, "D-1"), { kind: "here", fragment: "faq" });
});

test("a relative or absolute link to a filed document opens it, at its fragment", () => {
    const base = "https://docs.example.com/guide/";
    assert.deepEqual(followLink("../api/files#open", base, LINK_INDEX, "D-1"), { kind: "document", id: "D-2", fragment: "open" });
    assert.deepEqual(followLink("https://docs.example.com/api/files/", base, LINK_INDEX, "D-1"), { kind: "document", id: "D-2", fragment: "" });
    assert.deepEqual(followLink("util.h", "file:///home/me/proj/src/main.c", LINK_INDEX, "D-9"), { kind: "document", id: "D-3", fragment: "" });
});

test("anything outside the store is not followed, whatever its scheme", () => {
    const base = "https://docs.example.com/guide/";
    assert.deepEqual(followLink("https://elsewhere.example.org/", base, LINK_INDEX, "D-1"), { kind: "outside", url: "https://elsewhere.example.org/" });
    assert.equal(followLink("javascript:alert(1)", base, LINK_INDEX, "D-1").kind, "outside");
    assert.equal(followLink("mailto:someone@example.com", base, LINK_INDEX, "D-1").kind, "outside");
    assert.equal(followLink("../other", null, LINK_INDEX, "D-1").kind, "outside");
});

test("a fragment finds its heading the way pages spell anchors", () => {
    assert.equal(anchorSlug("Getting Started!"), "getting-started");
    assert.equal(headingMatches("Getting Started", "getting-started"), true);
    assert.equal(headingMatches("open_files()", "open-files"), true);
    assert.equal(headingMatches("Getting Started", "install"), false);
    assert.equal(headingMatches("Anything", ""), false);
});

/* The search page (kb:/search). */
test("the search page asks every collection by default, or the chosen ones as one comma list", () => {
    assert.deepEqual(searchPageQuery("  io_uring submit ", []), { k: 50, q: "io_uring submit" });
    assert.deepEqual(searchPageQuery("submit", ["io-uring", "papers"]), { k: 50, q: "submit", collection: "io-uring,papers" });
});

test("a collection is picked and unpicked in the order it was picked", () => {
    assert.deepEqual(toggleCollection([], "a"), ["a"]);
    assert.deepEqual(toggleCollection(["a", "b"], "a"), ["b"]);
    assert.deepEqual(toggleCollection(["b"], "a"), ["b", "a"]);
});

test("the search page marks the query's words in a snippet, whatever their case", () => {
    assert.deepEqual(markTerms("The Submission Queue is polled", "queue poll"), [
        { text: "The Submission ", hit: false },
        { text: "Queue", hit: true },
        { text: " is ", hit: false },
        { text: "poll", hit: true },
        { text: "ed", hit: false },
    ]);
    assert.deepEqual(markTerms("a b c", "a"), [{ text: "a b c", hit: false }]);
    assert.deepEqual(markTerms("x (here) y", "(here"), [
        { text: "x ", hit: false },
        { text: "(here", hit: true },
        { text: ") y", hit: false },
    ]);
    assert.deepEqual(markTerms("", "x"), []);
});

test("a hit's section is its chunk's heading on one line, or nothing", () => {
    assert.equal(sectionOf("  Setup \n and   install "), "Setup and install");
    assert.equal(sectionOf(null), null);
    assert.equal(sectionOf("   "), null);
    assert.equal(sectionOf("x".repeat(200))?.length, 160);
});

test("a hit's score is the one it was ranked by, with every score on hover", () => {
    assert.deepEqual(scoreLabel({ bm25: 12.44, vector: 0.712, fused: 0.0328 }), {
        text: "score 0.03",
        detail: "keyword 12.4 · semantic 0.71 · fused 0.03",
    });
    assert.deepEqual(scoreLabel({ fused: 0.5, rerank: 1.84 }), { text: "rerank 1.84", detail: "fused 0.50 · rerank 1.84" });
});
