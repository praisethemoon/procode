---
name: eggzibit
description: Write and publish an eggzibit page — an HTML page the person reads in VS Code — with the eggzibit MCP server (local pages, not to be confused with claude.ai artifacts). Use when a result is worth reading as a document rather than a chat reply (a report, a comparison, findings, a design note, anything with a table or a chart), or when the user asks for an eggzibit page, a report or a page.
compatibility: Requires the eggzibit MCP server (packages/eggzibit).
metadata:
  version: "2"
---

# eggzibit — pages for people

An eggzibit page is finished work handed over as a page — procode's own,
not a claude.ai artifact. It lives in the workspace's `.eggzibit/`, the
person opens it from the eggzibit view — where
they find it by filtering on its id, title, description and keywords, so
give it good ones — and it renders in their editor theme. The format is in `specs/eggzibit.md`.

## When to make one

Make one when the answer would be long in chat and better read than
scrolled: results with numbers, a comparison, anything a table or chart says
better than prose, a report someone will come back to or share. Do not make
one for a short answer, a question, or work still in progress — say it in
chat.

## How

1. `eggzibit_template {name: "report"}` — start from it for anything longer
   than a few paragraphs. Every component is in place and the comments say
   what goes where. Delete the comments and every section you do not need.
2. Write the page. Replace every UPPERCASE placeholder; never leave one.
3. `eggzibit_publish {title, description, keywords, html}`. The title is the
   finding, one line; the description is one or two sentences for the list;
   `keywords` are two to five short words or phrases the page is about —
   the topic, the component, the kind of page — so pages on one topic can
   be found together, e.g. `keywords: ["lap", "merge", "design note"]`.
   `eggzibit_list {keyword: "merge"}` finds the pages carrying one.
4. To revise it, publish again **with its `id`** — never a second page
   for the same work. `createdAt` is kept, and the keywords too unless you
   pass new ones.

## The shape that works

Taken from the first report, which the user liked enough to make the model:

- **Header**: `eyebrow` (kind · date), an `h1` that states the finding, a
  `lede` that answers the question in two sentences, `chip`s for scope,
  source and status.
- **Headline numbers**: three or four `kpi`s. Each says what it counts and
  why it matters. Only measured numbers.
- **Summary** as `callout`s: a verdict in `<strong>`, the reason in `<p>`.
  `ok` for what holds, `info` for context, `warn` for caveats, `danger` for
  what is broken.
- **Evidence**: a chart in a `panel` beside the table or text that explains
  it, in `cols`.
- **Detail**: a table; `td.id` for identifiers, `td.num` for numbers, `tag`
  for states; filter `tabs` when there are groups.
- **Open questions** as `details`, the most important one `open`. Say what
  was not verified. It is what makes the rest believable.
- **Next**: actions, as a list.
- A last `muted` line saying where the facts came from.

## Rules

- **No colours of your own.** No `<style>` for colour, no hex, no `rgb()`,
  no `fill=`/`stroke=` attributes. Components and plain elements are styled
  by the viewer from baukasten's tokens; if you truly need CSS, use
  `var(--bk-…)` tokens only (the server instructions list them). A fixed
  colour is wrong in some theme.
- **Charts are inline SVG** with `class="chart"` and the chart classes:
  `.bar` (`ok`, `warn`, `danger`, `muted`), `.grid`, `.node` (`accent`),
  `.edge` (`accent`, `dashed`), `.arrowhead`, `text.label`. Compute bar
  lengths from the numbers, start the scale at 0, and say the unit and scale
  in the `figcaption`. Leave room for labels at the right edge.
- **Diagrams**: route lines through the gaps between boxes; a line crossing
  a box reads as a connection that is not there.
- **Every fact from a real run**: tests, benchmarks, files, commits, board
  items. No invented numbers, no rounding that changes a claim. Check that
  totals add up.
- **No network.** Everything inline: SVG, `data:` images. Scripts run
  sandboxed; use them for filtering or drawing from a table, not to fetch.
- **Say what is not verified.** A page that sounds certain about
  something untested is worse than none.

## Checking it

Read the page back with `eggzibit_get` and look for leftover placeholders,
numbers that do not match their source, and sections that say nothing. When
you can render it (headless Chrome over the viewer's frame document, as in
`packages/eggzibit-vscode`), look at it in a light and a dark theme.
