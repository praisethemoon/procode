# artifacts — specification

Status: draft.

An **artifact** is a finished piece of work an agent hands to a person as a
page: a report, a comparison, a design note, a table of findings, a chart. It
is HTML, written against baukasten's design tokens, published into the
workspace, and read in VS Code, where it looks like part of the editor in
whatever theme the person uses.

Bindings, in the pattern lap, kb and coboard share:

| binding | consumer | notes |
|---|---|---|
| `artifacts` (TypeScript) | the MCP server, the extension | the only code that writes |
| MCP tools | agents | §4 |
| `artifacts-vscode` | people | the list and the viewer, §5 |

---

## 1. Where artifacts live

`.artifact/` at the workspace root, found by walking up from the working
directory the way `.git`, `.kb/` and `.coboard/` are. `artifact_publish`
creates it at the enclosing git repository's root when there is none (the
working directory outside a repository). Nothing is written
anywhere else.

```
.artifact/
  A-1/
    index.html       the page
    artifact.json    its metadata
  A-2/
    …
```

`.artifact/` belongs in version control: an artifact is a result, and a result
worth reading is worth keeping with the work it came from.

## 2. Identity and metadata

```
Artifact { id, title, description, createdAt, updatedAt, bytes }
```

- **`id`** is `A-<n>`: public, prefixed, monotonic, never reused. The next id
  is past both `.artifact/next` (a counter, so an id stays spent after its
  artifact is deleted) and every `A-<n>` directory present, and is claimed by
  creating its directory, which only one writer can do.
- **`title`** — required, one line, at most 200 characters.
- **`description`** — optional, what the page is and why it exists, at most
  2000 characters. It is what the list shows under the title.
- **`createdAt`**, **`updatedAt`** — ISO-8601 UTC. Republishing an artifact
  moves `updatedAt` and keeps `createdAt`.
- **`bytes`** — the size of `index.html`, reported and never stored.

`artifact.json` holds `{ id, title, description, createdAt, updatedAt }`.
The directory name and `id` must agree; a directory that is not `A-<n>`, or
whose `artifact.json` is missing or unreadable, is not an artifact and is
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
| `artifact_publish` | `{ title, html, description?, id? }`. Without `id`, creates the next `A-<n>`. With `id`, replaces that artifact's page and metadata, keeping `createdAt`. Returns the artifact and the path of its page. |
| `artifact_template` | `{ name? }`: a starting page with every component in place (§3), or without `name` the list of templates. Today there is `report`. |
| `artifact_list` | every artifact, newest `updatedAt` first, without pages |
| `artifact_get` | `{ id }`: the metadata and the page |

Refusals are tool results with `isError`, never transport errors:

| code | when |
|---|---|
| `invalid` | a title that is empty, multi-line or too long; a description that is too long; an empty page; a page over 5 MB |
| `not_found` | an `id` that is not an artifact |
| `bad_id` | an `id` that is not `A-<n>` — including anything that could name a path |

There is no delete tool. Removing an artifact is a person's decision, made in
the editor.

## 5. The viewer

`artifacts-vscode`:

- an **Artifacts** view: every artifact, newest first, with its title, its
  description and when it was last updated; refreshed when `.artifact/`
  changes;
- opening one shows it in an editor tab titled with the artifact's title. The
  tab builds the frame's document from the page with, in order: a content
  security policy (`default-src 'none'; img-src data:; font-src data:;
  style-src 'unsafe-inline'; script-src 'unsafe-inline'`), baukasten's
  VS Code stylesheet, the current values of every `--vscode-*` variable (the
  frame does not inherit them), and the default stylesheet — all before the
  page's own styles, so a page can override any of it;
- a theme change re-injects the variables, and republishing reloads the tab;
- **Open HTML Source** opens `index.html` as text; **Delete** removes the
  artifact's directory after the person confirms.
