# ask — specification

Status: draft. Experimental: not in the published build (E-23).

An agent with several questions asks them as a **form** in the person's
editor instead of a block of text in chat. Each question is a **step** on its
own page. The person answers the steps in any order, skips some, or sends one
back asking for more, then submits. The agent gets every step back with its
state.

| binding | consumer | notes |
|---|---|---|
| `ask` (TypeScript) | the extension | form checks, the window's forms, the MCP server |
| MCP tools, over HTTP | agents | §4 |
| `ask-vscode` | people | runs the server; the editor tab, §5 |

---

## 1. Lifecycle

The MCP server runs **inside the VS Code extension**, so the form an agent
opens and the tab that answers it are in one process. Nothing is written to
disk.

1. The agent calls `ask` over HTTP. The extension opens an editor tab at once.
2. The person submits, or closes the tab.
3. The answer goes back as the call's result.

If the call is cancelled, or its client hangs up, the form is cancelled and
the tab says so. The first answer stands: a form already answered is never
answered again.

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

## 3. Forms in memory

A window holds its forms (`Forms`): a request `{id, title, createdAt, steps,
previous?}` and, once there is one, its answer `{status: submitted|cancelled,
answeredAt, steps: {<step id>: step answer}}`. Ids count up from F-1 per
window. `previous` holds the answers filled in from an earlier form (§4,
`from`). Forms last as long as the window: a reload drops them, and any call
still waiting fails as its connection closes.

An earlier design wrote forms to a `.ask/` folder for a stdio server to
pick up. It was dropped (T-277): the server and the tab disagreed about
which folder to use when the workspace had no `.git` of its own, and the
files showed up in lap.

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

**Transport.** Streamable HTTP at `http://127.0.0.1:<port>/procode/ask/mcp`.
The port comes from the workspace folder's path (40000–48999), so the
address in its `.mcp.json` stays right across restarts. While it is taken
(another window), the next ones are tried. A client POSTs one message at a
time. A tool call is answered with an event stream (its progress, then its
result), any other request with JSON, and a notification with 202. GET is
405. Requests are refused when their Host is not this address, when they
carry an Origin (browsers do, agents do not), or when the body is not
`application/json`. `notifications/cancelled`, or the client hanging up,
cancels the call's form.

**Waiting.** Claude Code drops an HTTP tool call that sends nothing for 5
minutes. Only progress notifications reset that clock: SSE comments and
silence were both cut at 300 s, while progress kept a call alive (T-277,
Claude Code 2.1.287). Claude Code sends a `progressToken` with every call,
and the server sends progress every 15 seconds. The `.mcp.json` entry also
sets `timeout: 1800000`, which floors the idle limit for that server, and
the call gives up at 25 minutes with the form still open.

## 5. The editor tab

`ask-vscode` starts the MCP server (§4) when the window opens, while the
procode › MCP › ask setting is on (the default), and stops and restarts it
as the setting changes; stopping cancels the forms its calls waited on. It
opens a tab the moment an agent opens a form. **ask: Open Pending Questions** brings
back the tab of a form still waiting. In the combined extension, **procode:
Set Up MCP for Claude Code** writes the server's address into `.mcp.json`
as an `http` entry, and VS Code's own agent is given it too.

**Layout.** On the left, the steps, each marked answered, open, skipped or
needs more, with **Review and submit** last. On the right, one step: its
title, its Markdown body, the answer, and two fields that open on demand,
**Add a note** and **I need more on this**. A footer has Back, Skip and
Next. The review page lists every step with its answer, and any step can be
opened from there.

**States.** A step is open until it is touched. It is answered once it holds
an answer (a choice, a filled-in Other, or text). It is skipped when the
person skips it: an answer already in it is set aside, not sent, but kept,
and touching the step brings it back. It needs more when the "I need more"
field has text. That wins over an answer, and both are sent. When the form is
submitted, open steps are sent as skipped. Answers carried over with `from`
start filled in. A carried-over label that the step no longer offers is
dropped.

**Keys.** Outside a text field, `1`–`9` pick the options in order, and the
number after the last option picks Other. Enter goes to the next step and
⇧Enter goes back. In a text field, ⌘/Ctrl+Enter goes to the next step and
Escape leaves the field. On the review page, ⌘/Ctrl+Enter submits. Plain
Enter never submits.

**Rendering.** Markdown goes through react-markdown with GFM and no raw-HTML
plugin. An option's preview shows beside the options, for the option under
the mouse or keyboard focus, else the one picked. It is a `srcdoc` frame
sandboxed with nothing allowed: no scripts, opaque origin, no network. It is
styled with baukasten's tokens, the theme's `--vscode-*` values and techdocs'
`page.css`, and is rebuilt when the theme changes.

**Closing.** Submit hands the answer to the window's forms (`Forms.submit`,
which keeps only the steps asked, and only the first answer). Closing the tab without submitting
cancels the form. An answer that appears from elsewhere, such as the agent's
call being cancelled, turns the tab into a notice that nothing more will
reach the agent.
