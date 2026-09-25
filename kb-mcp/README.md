# kb-mcp

The `kb` knowledge base as MCP tools for agents: the six of `index-api.md` §9,
spoken as JSON-RPC 2.0 over stdio.

No SDK. The protocol an agent needs from this server is a handshake, a list and
a call; a dependency tree to speak it would be a larger thing to audit than the
surface it exposes. The whole of it is `framing.ts` (bytes), `jsonrpc.ts`
(meaning) and `server.ts` (methods).

It does not spawn the CLI. `kb-js` does, because §10 gives retrieval exactly one
implementation: searching requires embedding the query, which requires the
model, and two inference paths that disagree produce a store that answers
differently depending on which door the caller came through. An agent and a
reader must get the same answer to the same question.

## The six

| tool | routes |
|---|---|
| `kb_search` | `GET /search` with every filter |
| `kb_get` | `GET /chunks/{id}`, `GET /documents/{id}` |
| `kb_add` | `POST /documents` |
| `kb_collections` | `GET /collections`, `GET /stats` |
| `kb_links` | `GET /documents/{id}/links`, `POST /links` |
| `kb_stale` | `GET /stale` |

## And the seventh, which does not exist

`rebuild`, `reindex`, `compact`, `promote`, `demote` and every `DELETE` are not
exposed. §9 is explicit about why: an agent files knowledge into the project
store and reads from both, while forgetting — and deciding that something is
general enough to outlive the project — are the reader's decisions.

A tool that let an agent drop a collection would be a defect against that
section rather than a feature, and especially so when it is useful: the useful
ones are the ones somebody adds without reading §9. `tools.test.ts` asserts the
list whole, by name and in order, and `guards.test.ts` refuses a call into the
half of `kb-js` that would serve one.

Two rules follow the same line:

- **Snippets only.** `kb_search` returns snippets, and a search costs exactly
  one process — a layer that enriched each hit with its document's text would be
  the context flood §4 forbids, arriving as a convenience. `kb_get` is how a
  caller asks for the whole thing, deliberately, one item at a time.
- **No tool answers a question.** The store returns passages. Summarising is the
  caller's work, and doing it here would bury the provenance that makes the
  store worth having.

## Where a write lands

`kb_add` takes no tier. §1.4 sends an ingest to the project store when one
exists and to global otherwise, because the intent at the moment of filing is
almost always project-scoped; §9 then keeps `promote` and `demote` away from
agents entirely. A `store: "global"` argument would hand the caller that
decision at the one moment it cannot be made well, in the direction that is hard
to undo — a document filed globally is easy never to notice again. So the flag
is not sent, and `kb_links`'s write refuses a tier out loud rather than
accepting one and dropping it.

Which project store that is depends on the working directory the server was
started in, because §1.4 finds one by walking up like `.git`. A host that starts
this somewhere other than the workspace gets a server that searches and files
into the global tier, and every answer it gives is plausible.

## Running it

```
kb-mcp
```

Reads JSON-RPC messages on stdin, one per line, and writes them on stdout, one
per line. Diagnostics go to stderr — stdout carries the protocol and nothing
else, and `guards.test.ts` refuses `console.log` and `process.stdout.write`
across `src/` by name.

| variable | meaning |
|---|---|
| `KB_BIN` | the `kb` executable. A bare name is resolved through PATH. |
| `KB_STORE` | read by the CLI, not here: it moves the global tier. |

## Framing

Newline-delimited JSON, which is MCP's stdio transport — not `Content-Length`
headers, which is LSP's. The two look alike from a distance and a server that
picks the wrong one does not fail loudly: it reads nothing, answers nothing, and
the client waits with nothing in any log to read.

`data` on a pipe is not a message boundary. The reader buffers across chunks and
decodes across a character split, and `framing.test.ts` cuts a message at every
byte offset rather than at a plausible one — a reader that handles one boundary
passes a test that only cuts it in half, and a split between the two bytes of an
`é` is silent corruption of the thing being filed.

A protocol error is an `error` response; a tool failure is a `result` with
`isError`. "There is no tool called kb_serach" is a fault in the call and the
transport says so. "kb refused: not_found" is the tool working correctly and
reporting what the store said — the model has to read that, and a client shown a
failed call instead would show it a broken tool and it would ask again.

## Building and testing

```
node scripts/link.mjs     # node_modules/kb-js -> ../../kb-js
npm test                  # compile, then node --test out/test/*.test.js
```

`cli.test.ts` drives the real binary when `../kb-cli/bin/kb` is built and skips
when it is not. `stdio.test.ts` spawns `bin/kb-mcp` as a process and talks to it
over a real pipe, which is the only place a failure in the wiring — an entry
point that never starts, a banner on stdout, a process that exits while a call
is in flight — can be found at all.
