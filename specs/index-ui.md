# knowledge base — UI specification

Status: draft. Companion to `index-api.md`.

A reader's surface over the same store the agent queries. Its whole job is:
find a document, read it, see where it came from. It is deliberately smaller
than the board.

Built with baukasten, following the conventions in `UI.md` — read-only custom
editors on a URI scheme, public identifiers only, no dirty state, no save.

---

## 1. Surface

One contribution to the VSCode activity bar:

| view | icon | contents |
|---|---|---|
| **Knowledge** | `media/knowledge.svg`: lap's spiral as a magnifier | documents, searchable |

The icon is drawn like lap's (`packages/lap-vscode/media/lap.svg`): 24×24, one
2px round stroke. It replaced the `book` codicon on 2026-09-26, chosen from the
options in techdocs page A-1.

A webview sidebar, for the same reason the board is one: a native `TreeView`
cannot carry a search field and per-row metadata.

## 2. Sidebar

The search bar at the top — the same bar as the Board's and Lap History's
filter — and below it the collections, or one collection's documents.

**With no query**, the sidebar shows **the collections**, by name, each with
its document count, byte size and oldest fetch date, and a **Manage
collections…** link to `kb:/collections` (§4). Browsing is the default state,
not an empty prompt — the store is worth looking through even when there is
no question. A collection row **opens that collection in the sidebar**: its
documents, newest first, under a back control that returns to the
collections. A document row there carries its title, a description when its
`meta` has one (`description`, then `abstract`, then `summary`: documents have
no description field of their own), its size, its type and its fetched date;
it opens the document.

**Paging.** Both lists end in **Load more** rather than stopping: a
collection's documents come a page at a time from `GET /documents` with its
cursor (index-api §2, `after`, newest first with `reverse`); the collections
come whole from `GET /collections` and are shown a page at a time.

**A collection filed from a folder is drawn as that folder.** When a
document on the collection's first page has a `path` (index-api §1.2, set by
`add --dir`), the collection is a **tree of folders and files**, as VS Code's
Explorer draws one:

- folders first, then files, each in natural order ignoring case;
- a folder row has a chevron, its name and the number of files under it, and
  opens and closes on click; top-level folders start open, deeper ones
  closed, and what the reader opens or closes is kept while the sidebar stays
  open;
- a chain of folders that each hold only one folder is one row
  (`tests/fixtures/syntax`), and the folder every path shares is not a row:
  it is said once above the tree (`in kb-cli/`);
- a file row shows the file name, not the path, with its size, type and
  fetched date; the full path is on hover, and it opens the document;
- documents without a `path` (web pages, papers) follow the tree under
  **Documents without a path**, newest first, a page at a time.

A tree cannot page newest first — files would land in half-built folders — so
such a collection is listed whole (`GET /documents` without `limit`) before it
is drawn, and **Load more** is left to the documents without a path. A search
inside it stays a flat list of results, each with its path.

**With a query**, the list is search results (`GET /search`, hybrid) — across
the store from the collections, within the open collection from inside it.
Search runs debounced as the reader types; a local hybrid search is fast
enough that submitting is unnecessary ceremony.

A search row carries:

- the document title
- the collection
- the fetched date
- a one-line snippet, clamped, with the full text on hover
- a `stale` badge when the document is older than the staleness threshold
- which retrieval paths matched, when the row came from a search

Rows are not highlighted and matches are not marked up. The row says which
document it is; reading it is the next step, and the reader does that in the
document.

**Empty**: "Nothing indexed yet. Research lands here when an agent files what
it read."

**Title-bar actions**: add the current file, add a URL, refresh stale documents.

## 3. Document

A document opens in an editor tab at `kb:/D-241`.

### 3.1 Header

The provenance, always visible, because a passage whose age and origin are
unknown is a passage that will eventually be trusted when it should not be:

- title
- the source locator, as a link that opens the original (below)
- collection
- fetched date, and indexed date when they differ
- `stale` badge with a refresh action
- size, mime, chunk count

**Where the locator link goes.** A document filed from a local file or
folder shows and opens **its own file**: a file source's locator, or a
folder source's locator joined with the document's `path` under it
(`index-api.md` §2.1) — `…/cli/kb-cli` and `src/simd.h` show as
`…/cli/kb-cli/src/simd.h`. A locator written as a bare absolute path or as a
`file://` URL is local either way. The file opens in a VS Code editor without
asking: it is opened for reading and runs nothing. When it is no longer there,
the view says so — the original is gone, and the copy kb stored is what the
reader has. A web locator (`http`, `https`, `mailto`) opens externally behind
a confirmation that names the host, because the system handler is what
receives it; any other scheme, and a relative path, is refused.

Anything else the document carries in `meta` — a paper's authors and year, a
page's section path, a file's language — renders as a plain key/value list
below. It is free-form per document (`index-api.md` §1.2), so the header shows
what is there and asserts nothing about what should be.

### 3.2 Body

The document text, rendered for reading: Markdown as Markdown, HTML as
sanitized prose, source code with syntax highlighting, everything else as
plain text.

No match highlighting. Opening a search result scrolls to the matching chunk's
heading and stops there — the chunk already carries its heading and span
(`index-api.md` §1.2), so the navigation is free, while marking up the text is
a feature with a maintenance cost and no reader asking for it.

**Links are followed inside the store only.** A link in a Markdown or HTML
document is resolved against the document's own address: its source's URL
for a page fetched from the web, or its folder's locator joined with its
`path` as a `file:` URL (`index-api.md` §2.1). Then:

- a fragment of this page (`#setup`, or a link back to the page) scrolls to
  the heading it names, matched the way pages spell anchors
  (`#getting-started` finds "Getting Started");
- a link to another filed document opens that document, at the fragment's
  heading when there is one. Addresses match whatever their spelling: a
  trailing `/` or `index.html` does not stop a match, the query does;
- anything else is **not followed**: it reads as its text, with a dotted
  underline and a tooltip naming where it pointed. Nothing opens, not even
  behind a confirmation. A `kb:` reference in Markdown opens its tab.

The address table comes from one `kb ls`, asked again when the store changes.
The header's link to the original (§3.1) is not a link in the document and is
unchanged.

Read-only. Editing an indexed copy of someone else's documentation would make
the content hash meaningless and the provenance a lie.

## 4. Collections

`kb:/collections` lists every collection with its document count, byte size and
oldest fetch date. Rename and delete live here and nowhere else: the
sidebar's collection rows open a collection and do nothing else, one
mis-click from losing a topic being one too many. The sidebar's list links
here (**Manage collections…**).

A collection row opens that collection in the sidebar (§2).

## 5. Quick open

A command, `Knowledge: Search`, opens a `QuickPick` over the same search.
Picking a result opens the document. This is the path for a reader who already
knows roughly what they are looking for and does not want to leave the
keyboard.

## 6. URIs

```
kb:/D-241          a document
kb:/S-3            a source, with its documents
kb:/collections    the collection list
kb:/graph          the links between documents, drawn
```

`kb:/graph` draws index-api §6's links: a document is a node coloured by its
collection, a link an arrow in its type's style. It is read-only like
everything else here — a collection and "documents with no links" narrow what
is drawn, and clicking a document opens it.

Same scheme discipline as `UI.md` §4 and §6: one string is both the reference
and the editor URI, every identifier shown is the public one, and opening a
document that already has a tab focuses it.

## 7. Not included

- **Editing.** The store holds fetched copies; the originals are elsewhere.
- **Match highlighting.** §3.2.
- **A graph view.** `index-api.md` §6 keeps links minimal and §12 leaves entity
  nodes unresolved; a visualisation of a schema that has not settled would
  settle it by accident.
- **Ingest configuration.** Chunk sizes, model choice and store paths are CLI
  and config concerns, not reader-facing ones.
