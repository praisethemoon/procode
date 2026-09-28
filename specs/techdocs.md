# techdocs — specification

Status: draft.

A techdocs **page** is a finished piece of work an agent hands to a person: a report, a comparison, a design note, a table of findings, a chart. It
is HTML, written against baukasten's design tokens, published into the
workspace, and read in VS Code, where it looks like part of the editor in
whatever theme the person uses.

Bindings, in the pattern lap, kb and coboard share:

| binding | consumer | notes |
|---|---|---|
| `techdocs` (TypeScript) | the MCP server, the extension | the only code that writes |
| MCP tools | agents | §4 |
| `techdocs-vscode` | people | the list and the viewer, §5 |

---

## 1. Where pages live

`.techdocs/` at the workspace root, found by walking up from the working
directory the way `.git`, `.kb/` and `.coboard/` are. `techdocs_publish`
creates it at the enclosing git repository's root when there is none (the
working directory outside a repository). Nothing is written
anywhere else.

```
.techdocs/
  A-1/
    index.html       the page
    page.json        its metadata
  A-2/
    …
```

`.techdocs/` belongs in version control: a page is a result, and a result
worth reading is worth keeping with the work it came from.

## 2. Identity and metadata

```
Page { id, title, description, keywords, createdAt, updatedAt, bytes }
```

- **`id`** is `A-<n>`: public, prefixed, monotonic, never reused. The next id
  is past both `.techdocs/next` (a counter, so an id stays spent after its
  page is deleted) and every `A-<n>` directory present, and is claimed by
  creating its directory, which only one writer can do.
- **`title`** — required, one line, at most 200 characters.
- **`description`** — optional, what the page is and why it exists, at most
  2000 characters. It is what the list shows under the title.
- **`keywords`** — optional, a list of short words or phrases the page is
  about, so pages on one topic can be found together. Each is trimmed,
  lowercased, its inner spaces collapsed; blanks are dropped and repeats kept
  once, in the order given. At most 10, each at most 40 characters; anything
  else is refused (`invalid`). A page published without them has none, and
  a page written before keywords existed reads as none: no migration.
  Republishing with keywords replaces the list; without, it keeps it.
- **`createdAt`**, **`updatedAt`** — ISO-8601 UTC. Republishing a page
  moves `updatedAt` and keeps `createdAt`.
- **`bytes`** — the size of `index.html`, reported and never stored.

`page.json` holds `{ id, title, description, keywords, createdAt, updatedAt }`.
The directory name and `id` must agree; a directory that is not `A-<n>`, or
whose `page.json` is missing or unreadable, is not a page and is
skipped.

Both files are written atomically (temporary file, then rename), the page
first, so a reader never sees metadata for a page that is not there.

## 3. The page

`index.html` is either a whole document or a fragment; the viewer wraps a
fragment in a document. It may assume:

- **baukasten's tokens**, as CSS custom properties on `:root`, bound to the
  current VS Code theme: colours (`--bk-color-foreground`,
  `--bk-color-foreground-muted`, `--bk-color-background`,
  `--bk-color-background-secondary`, `--bk-color-border`, `--bk-color-link`,
  `--bk-color-primary`, `--bk-color-success`, `--bk-color-warning`,
  `--bk-color-danger`, `--bk-color-info`, `--bk-color-code-background`, …),
  spacing (`--bk-spacing-1` … `--bk-spacing-24`, `--bk-gap-{xs..xl}`), type
  (`--bk-font-family-sans`, `--bk-font-family-mono`, `--bk-font-size-{xs..5xl}`,
  `--bk-font-weight-{normal,medium,semibold,bold}`,
  `--bk-line-height-{tight,normal,relaxed}`), shape (`--bk-radius-{sm..3xl}`,
  `--bk-border-width-{1,2,4}`, `--bk-shadow-{sm..2xl}`);
- **a default stylesheet** for plain elements — body, headings, paragraphs,
  lists, links, tables, `code` and `pre`, `blockquote`, `hr`, `details` —
  written with those tokens, so a page with no styles of its own already
  looks native;
- **components by class name**, so a report is markup and no CSS:

  | class | markup |
  |---|---|
  | `eyebrow`, `lede` | the line above the title; the paragraph under it |
  | `meta` + `chip` | `<div class="meta"><span class="chip">Status <b>open</b></span></div>` |
  | `kpis` + `kpi` (`warn`, `danger`) | `<div class="kpi"><b>13</b><span>tickets closed</span><small>3 epics</small></div>` |
  | `callout ok · info · warn · danger` | `<div class="callout ok"><strong>Finding.</strong><p>Why.</p></div>` |
  | `cols`, `panel` | side by side (stacking when narrow); a bordered box |
  | `tag` (`ok`, `warn`, `danger`) | a small label in a table or a line |
  | `tabs` | filter buttons; the pressed one has `aria-pressed="true"` |
  | `td.id`, `td.num` | identifiers in the editor font; right-aligned numbers |
  | `toc` | a two-column list of section links |
  | `svg.chart` | inline charts: `.bar` (`ok`, `warn`, `danger`, `muted`), `.grid`, `.node` (`accent`), `.edge` (`accent`, `dashed`), `.arrowhead`, `text.label` |

  All of them are in the stylesheet's layer, so a page's own CSS overrides
  any of them.

A page that hard-codes colours will look wrong in some theme; the tokens are
the point.

**What a page cannot do.** It renders in a sandboxed frame with no network:
no fetch, no remote script, image, font or stylesheet (`data:` URIs work).
Inline scripts run, inside the sandbox, with no access to VS Code, the
extension, the workspace or the viewer. Everything a page shows travels in
`index.html`.

## 4. MCP tools

| tool | does |
|---|---|
| `techdocs_publish` | `{ title, html, description?, keywords?, id? }`. Without `id`, creates the next `A-<n>`. With `id`, replaces that page and its metadata, keeping `createdAt`, and keeping its keywords unless `keywords` is given. Returns the page's metadata and the path of its `index.html`. The server's instructions ask for two to five keywords on every page. |
| `techdocs_template` | `{ name? }`: a starting page with every component in place (§3), or without `name` the list of templates. Today there is `report`. |
| `techdocs_list` | `{ keyword? }`: every page, newest `updatedAt` first, with its keywords and without its HTML; with `keyword`, only the pages carrying it (compared as keywords are stored, so case does not matter) |
| `techdocs_get` | `{ id }`: the metadata (keywords included) and the page |

Refusals are tool results with `isError`, never transport errors:

| code | when |
|---|---|
| `invalid` | a title that is empty, multi-line or too long; a description that is too long; keywords that are not a list of text, or over the limits of §2; an empty page; a page over 5 MB |
| `not_found` | an `id` that is not a page |
| `bad_id` | an `id` that is not `A-<n>` — including anything that could name a path |

There is no delete tool. Removing a page is a person's decision, made in
the editor.

## 5. The viewer

`techdocs-vscode`:

- an **techdocs** view: every page, newest first, with its id, title,
  when it was last updated, its description and its keywords; refreshed when
  `.techdocs/` changes. It is a webview with the same filter bar as the Board
  and Lap History: the text keeps the pages whose id, title, description or
  keywords hold every word typed, in any case; the chevron opens the
  keywords, each with how many pages carry it, and choosing some keeps the
  pages carrying any of them — as does clicking a keyword on a row. What is
  typed and chosen survives the view being hidden. A row opens its page;
  right-click offers Open, Open HTML Source and Delete;
- its activity-bar icon is a sheet falling onto a pile of papers, the same
  whether or not the workspace has a page; the view is `techdocs.list`, in
  one container;
- opening one shows it in an editor tab titled with the page's title. The
  tab builds the frame's document from the page with, in order: a content
  security policy (`default-src 'none'; img-src data:; font-src data:;
  style-src 'unsafe-inline'; script-src 'unsafe-inline'`), baukasten's
  VS Code stylesheet, the current values of every `--vscode-*` variable (the
  frame does not inherit them), and the default stylesheet — all before the
  page's own styles, so a page can override any of it;
- a theme change re-injects the variables, and republishing reloads the tab;
- **Open HTML Source** opens `index.html` as text; **Delete** removes the
  page's directory after the person confirms.
