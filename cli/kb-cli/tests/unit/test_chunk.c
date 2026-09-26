/* §3: chunks follow document structure, not a fixed window.
 *
 * Rewritten after the working tree was lost. These assert the properties §3
 * states rather than the exact boundaries this splitter happens to pick, so
 * they discriminate against a broken splitter without freezing an
 * implementation detail the spec does not fix.
 *
 * The span invariant is the one that matters most: §3 says every chunk keeps
 * its span back into the document so a hit can be shown in place. A span that
 * does not land on the text it claims makes every search result a lie, and it
 * is exactly the kind of error that looks fine in a snippet. */
#include <string.h>

#include "test.h"
#include "../../src/chunk.h"

static Chunks split(Arena *a, const char *text, Lang lang) {
    return chunk_split(a, text, strlen(text), lang, SYNTAX_NONE, 400, 60);
}

static void test_lang(void) {
    t_begin("chunk: the mime decides the splitter when it says anything");
    ASSERT_EQ_I(chunk_lang("text/markdown", ""), LANG_MARKDOWN);
    ASSERT_EQ_I(chunk_lang("text/html", ""), LANG_HTML);
    ASSERT_EQ_I(chunk_lang("text/plain", ""), LANG_TEXT);

    t_begin("chunk: the path decides when the mime does not");
    ASSERT_EQ_I(chunk_lang(NULL, "notes.md"), LANG_MARKDOWN);
    ASSERT_EQ_I(chunk_lang(NULL, "page.html"), LANG_HTML);
    ASSERT_EQ_I(chunk_lang(NULL, "main.c"), LANG_CODE);

    t_begin("chunk: anything unrecognised is plain text, which always splits");
    /* §3's fallback. A splitter that refused an unknown type would make the
     * store unable to hold something it was handed, and ingest is the one
     * place that must not be fussy. */
    ASSERT_EQ_I(chunk_lang(NULL, "archive.tar.zst"), LANG_TEXT);
    ASSERT_EQ_I(chunk_lang(NULL, NULL), LANG_TEXT);
}

/* The property §3 promises: a chunk's span addresses the document it came
 * from. Checked for every chunk of every splitter rather than spot-checked. */
static void spans_are_sane(const Chunks *c, size_t len, const char *what) {
    ASSERT_TRUE(c->n > 0);
    for (size_t i = 0; i < c->n; i++) {
        ASSERT_TRUE(c->v[i].start <= c->v[i].end);
        ASSERT_TRUE(c->v[i].end <= len);
    }
    /* Structural splitters cover the document from the first byte to the
     * last: a gap is text nobody can retrieve. */
    ASSERT_EQ_I((int64_t)c->v[0].start, 0);
    ASSERT_EQ_I((int64_t)c->v[c->n - 1].end, (int64_t)len);
    (void)what;
}

static void test_markdown(Arena *a) {
    const char *doc = "# First\n\nalpha beta\n\n# Second\n\ngamma delta\n";
    t_begin("chunk: markdown splits on headings and keeps them");
    Chunks c = split(a, doc, LANG_MARKDOWN);
    ASSERT_EQ_I((int64_t)c.n, 2);
    spans_are_sane(&c, strlen(doc), "markdown");
    ASSERT_EQ_S(c.v[0].heading, "First");
    ASSERT_EQ_S(c.v[1].heading, "Second");
    /* The second chunk must actually contain its own text, which is what the
     * span is for. */
    ASSERT_TRUE(memmem(doc + c.v[1].start, c.v[1].end - c.v[1].start, "gamma",
                       5) != NULL);
    ASSERT_TRUE(memmem(doc + c.v[0].start, c.v[0].end - c.v[0].start, "gamma",
                       5) == NULL);

    t_begin("chunk: a hash inside a fence is a comment, not a heading");
    /* The case that makes a naive line-scanner wrong. A shell comment in a
     * fenced block would otherwise start a new chunk and take the heading of
     * a section that does not exist. */
    const char *fenced = "# Real\n\n```sh\n# not a heading\necho hi\n```\n\nx\n";
    Chunks f = split(a, fenced, LANG_MARKDOWN);
    ASSERT_EQ_I((int64_t)f.n, 1);
    ASSERT_EQ_S(f.v[0].heading, "Real");
    spans_are_sane(&f, strlen(fenced), "fenced");
}

static void test_html(Arena *a) {
    t_begin("chunk: html splits on h1-h6 and strips tags from the label");
    const char *doc =
        "<h1>Alpha</h1><p>one</p><h2>Beta <em>two</em></h2><p>three</p>";
    Chunks c = split(a, doc, LANG_HTML);
    ASSERT_EQ_I((int64_t)c.n, 2);
    spans_are_sane(&c, strlen(doc), "html");
    ASSERT_EQ_S(c.v[0].heading, "Alpha");
    /* The label is for a reader, so the markup comes out — but the span still
     * indexes the raw bytes, which is what the blob holds. */
    ASSERT_TRUE(strstr(c.v[1].heading, "Beta") != NULL);
    ASSERT_TRUE(strchr(c.v[1].heading, '<') == NULL);
}

static void test_text(Arena *a) {
    t_begin("chunk: plain text windows with overlap, and still covers itself");
    /* Long enough to force more than one window at the 400-byte target. */
    char big[2048];
    for (size_t i = 0; i < sizeof big - 1; i++)
        big[i] = (char)('a' + (i % 26));
    big[sizeof big - 1] = '\0';
    Chunks c = chunk_split(a, big, strlen(big), LANG_TEXT, SYNTAX_NONE, 400, 60);
    ASSERT_TRUE(c.n > 1);
    spans_are_sane(&c, strlen(big), "text");
    /* §3 says the fallback overlaps. Without it a match that straddles a
     * window boundary is retrievable from neither side. */
    ASSERT_TRUE(c.v[1].start < c.v[0].end);

    t_begin("chunk: a document smaller than the window is one chunk");
    Chunks one = split(a, "short", LANG_TEXT);
    ASSERT_EQ_I((int64_t)one.n, 1);
    ASSERT_EQ_I((int64_t)one.v[0].start, 0);
    ASSERT_EQ_I((int64_t)one.v[0].end, 5);

    t_begin("chunk: an empty document yields nothing to retrieve");
    Chunks none = chunk_split(a, "", 0, LANG_TEXT, SYNTAX_NONE, 400, 60);
    ASSERT_EQ_I((int64_t)none.n, 0);
}

static void test_tokens(void) {
    t_begin("chunk: the token estimate rises with size and never reports zero");
    /* A stand-in until §8's tokenizer exists. Zero would make a chunk look
     * free to a budget that is counting. */
    ASSERT_TRUE(chunk_tokens_of(1) >= 1);
    ASSERT_TRUE(chunk_tokens_of(4000) > chunk_tokens_of(400));
}

static void test_heading_paths(void) {
    Arena *a = arena_new(1 << 16);
    t_begin("chunk: a section knows the headings above it");
    const char *md = "# Guide\n\nintro\n\n## Install\n\nsteps\n\n### On macOS\n\nbrew\n\n"
                     "## Use\n\nrun it\n\n#### Deep\n\nskipped a level\n";
    Chunks c = chunk_split(a, md, strlen(md), LANG_MARKDOWN, SYNTAX_NONE, 400, 60);
    const Chunk *mac = NULL, *use = NULL, *deep = NULL;
    for (size_t i = 0; i < c.n; i++) {
        if (c.v[i].heading && strcmp(c.v[i].heading, "On macOS") == 0) mac = &c.v[i];
        if (c.v[i].heading && strcmp(c.v[i].heading, "Use") == 0) use = &c.v[i];
        if (c.v[i].heading && strcmp(c.v[i].heading, "Deep") == 0) deep = &c.v[i];
    }
    ASSERT_TRUE(mac && mac->context && strcmp(mac->context, "Guide > Install") == 0);
    ASSERT_TRUE(use && use->context && strcmp(use->context, "Guide") == 0);
    ASSERT_TRUE(deep && deep->context && strcmp(deep->context, "Guide > Use") == 0);

    t_begin("chunk: the header line names the document and the section path, once");
    ASSERT_TRUE(strcmp(chunk_header(a, LANG_MARKDOWN, "Guide", mac), "Guide > Install > On macOS") == 0);
    ASSERT_TRUE(strcmp(chunk_header(a, LANG_MARKDOWN, "setup.md", mac),
                       "setup.md > Guide > Install > On macOS") == 0);
    Chunk top = {"Guide", NULL, 0, 1, 1};
    ASSERT_TRUE(strcmp(chunk_header(a, LANG_MARKDOWN, "Guide", &top), "Guide") == 0);
    Chunk bare = {NULL, NULL, 0, 1, 1};
    ASSERT_TRUE(strcmp(chunk_header(a, LANG_TEXT, "notes", &bare), "notes") == 0);
    ASSERT_TRUE(chunk_header(a, LANG_TEXT, NULL, &bare) == NULL);

    t_begin("chunk: html headings have paths too");
    const char *html = "<h1>API</h1><p>a</p><h2>Search</h2><p>b</p><h3>Fusion</h3><p>c</p>";
    c = chunk_split(a, html, strlen(html), LANG_HTML, SYNTAX_NONE, 400, 60);
    bool found = false;
    for (size_t i = 0; i < c.n; i++)
        if (c.v[i].heading && strcmp(c.v[i].heading, "Fusion") == 0)
            found = c.v[i].context && strcmp(c.v[i].context, "API > Search") == 0;
    ASSERT_TRUE(found);
    arena_free(a);
}

void test_chunk(void) {
    test_heading_paths();
    Arena *a = arena_new(1 << 16);
    test_lang();
    test_markdown(a);
    test_html(a);
    test_text(a);
    test_tokens();
    arena_free(a);
}
