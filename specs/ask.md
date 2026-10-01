# ask — specification

Status: draft. Experimental: not in the published build (E-23).

An agent with several questions asks them as a **form** in the person's
editor instead of a block of text in chat. Each question is a **step** on its
own page. The person answers the steps in any order, skips some, or sends one
back asking for more, then submits. The agent gets every step back with its
state.

| binding | consumer | notes |
|---|---|---|
| `ask` (TypeScript) | the MCP server, the extension | form checks and the store |
| MCP tools | agents | §4 |
| `ask-vscode` | people | the editor tab, §5 |

---

## 1. Lifecycle

1. The agent calls `ask`. The server writes `.ask/F-<n>/request.json` and waits.
2. The extension sees the request and opens an editor tab for it.
3. The person submits, or closes the tab. The tab writes `answer.json`.
4. The server reads the answer and returns it as the call's result.

If the call is cancelled, or its client goes away, the server writes a
cancelled answer and the tab closes. A form already answered is never
overwritten.

## 2. The form

**Step**: `id` (defaults to its position, `"1"`, `"2"`, …), `title`, `body`
(Markdown), `kind`, `options`, `other`.

- `kind` is `single` (pick one), `multi` (pick any) or `text` (write).
  Without a kind, a step with options is `single` and one without is `text`.
- An **option** is a label, or `{label, description, preview}`. The
  description is Markdown. The preview is HTML, shown sandboxed and styled
  with baukasten's `--bk-*` variables, the same rules as a techdocs page.
- Choice steps offer a free-text **Other** unless `other` is `false`. A
  choice step needs one option, or two with Other off.
- Limits: 30 steps, 12 options per step, 200 characters per title and
  label, 20,000 characters of Markdown, 200 kB of HTML per preview.

**Step answer**: `state` is `answered`, `skipped` or `needs_more`. It also
holds `choices` (the labels picked), `other`, `text`, `note` (anything the
person adds beside the answer) and `question` (what they want to know, when
the state is `needs_more`). Empty fields are left out.

## 3. Files

```
.ask/
  .gitignore       "*": a form is part of a conversation, not a record
  F-1/
    request.json   {id, title, createdAt, steps, previous?}
    answer.json    {status: submitted|cancelled, answeredAt, steps: {<step id>: step answer}}
```

`.ask/` is found by walking up from the working directory. With none, the
first form creates it at the enclosing git repository's root. Ids count up
from F-1, and `mkdir` claims the number, so two servers never share one. Both
files are written atomically (a temporary file, then a rename). `previous`
holds the answers to fill in from an earlier form (§4, `from`).

## 4. MCP tools

**`ask {title, steps, from?}`** opens a form and waits for it. The result is
`{id, status, steps?, next?}`:

- `submitted`: every step asked, in order, each with its state. A step the
  person never touched reads as `skipped`. When any step is `needs_more`,
  `next` tells the agent to answer the person's question in that step's body
  and ask those steps again with `from`.
- `cancelled`: the person closed the form.
- `waiting`: the call reached its limit (25 minutes) with the form still
  open, and `next` says to call `ask_wait`.

`from: F-<n>` carries over that form's **answered** steps, matched by step
id. Skipped and `needs_more` steps open fresh.

**`ask_wait {id}`** waits again for a form that came back `waiting`, and
returns the same result.

**Waiting.** Claude Code lets a tool call run for about 28 hours. It does cut
off a stdio call that sends nothing for 30 minutes (the idle timeout).
Progress notifications reset that clock. The server sends one every 15
seconds when the call carries a `progressToken`. The 25-minute limit keeps a
client that sends no token under the idle timeout too. (Claude Code docs, MCP
page, 2026-10-02. One known exception: the desktop app cancelled stdio calls
at about 60 seconds, per anthropics/claude-code#63379.)

## 5. The editor tab

To be specified with T-272.
