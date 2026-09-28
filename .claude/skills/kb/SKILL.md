---
name: kb
description: Search and file research in the project's local knowledge base (kb) instead of fetching it again. Use whenever a .kb store exists in the project and you are about to look something up (documentation, an API, a paper, how some code works), after you have read something worth keeping, or when the user asks about kb. Teaches search-before-fetch, filing as you go, reading passages whole, and citing them.
compatibility: Requires the kb MCP server, or the kb CLI (cli/kb-cli) at ~/.procode/bin/kb when procode's VS Code extension is installed, else on PATH. A store (.kb/) must exist; creating one is the user's call.
metadata:
  version: "1"
---

# kb — the project's knowledge base

kb is a local, offline store of documentation, source and papers, one per
workspace in `.kb/` (found by walking up, like `.git`). Research filed once
is answered from disk the next time instead of fetched again, and every
passage keeps where it came from and when. kb finds and returns passages;
reading them, summarising and answering are your work.

**Where kb is.** Prefer the MCP tools (`kb_*`). For what they do not cover,
run the CLI as `~/.procode/bin/kb`: procode's VS Code extension puts it
there. Without the extension, use `kb` from PATH. This skill writes `kb` for
short; read it as whichever of the two you have.

## The loop

1. **Search before fetching.** Before any web search or fetch on a topic,
   `kb_search` it. `kb_collections` shows which topics are already covered.
2. **Read the passage whole.** A hit is a snippet; `kb_get` its chunk id
   (`C-n`) for the passage with its neighbours, or a document id (`D-n`)
   for the whole text. Answer from what you read, not from the snippet.
3. **File what you read.** When you had to fetch after all, `kb_add` what you
   read, in the collection for its topic, with its `url` and `mime`, as you
   go rather than at the end. Filing is idempotent by content, so filing a
   page twice is harmless.
4. **Cite.** Name the passage an answer rests on (its document id and URL),
   so the user can check it.

## The MCP tools

| tool | what it does |
|---|---|
| `kb_search` | Ranked snippets for a query: keyword and semantic, fused. `mode: "keyword"` for an exact name or identifier; `rerank: true` when the best passage matters more than a second of latency; `collection` to narrow the scope. |
| `kb_get` | One chunk (`C-n`) whole with `expand` neighbours, or one document (`D-n`) with its text. |
| `kb_add` | File `documents` (each with `title`, `content`, `collection`, and when known `url`, `mime`, `meta`), all or none; or a whole folder with `dir` and `collection`. |
| `kb_collections` | The topics, with their counts and when each was last added to. |
| `kb_links` | Read a document's links (`op: "list"`), or state one (`op: "add"`): `supersedes`, `cites`, `analogue_of`, `implements`, `see_also`. |
| `kb_stale` | Documents older than a threshold, which may be out of date. |

## Practices

- **Collections are topics**, flat and named for what they hold
  (`win32-iocp`, `react-router`), not for the task at hand. Reuse an existing
  one before making a new one.
- **Give `mime`** when you know it: it decides how the document is split.
  **Give `url`** always: it is the provenance a later reader checks.
- **An exact name** (a function, a flag, an error message) searches best with
  `mode: "keyword"`. When an answer reports `unembedded: N`, recently filed
  text is not in the semantic index yet; keyword search still finds it.
- **Old is not wrong, but say so.** A hit tells how old it is; for
  fast-moving documentation, mention the date, and check `kb_stale` before
  relying on a topic's passages. Refetching is the user's call.
- **State relationships** the text cannot: when two documents solve the same
  problem differently, `kb_links` `analogue_of`; when one replaces another,
  `supersedes`.
- **Do not create a store.** When the workspace has no `.kb/`, `kb_add`
  fails: tell the user, who may run `kb init`.

## The CLI, for what MCP does not cover

Every command takes `--json`; `kb <command> --help` lists its flags.

```sh
kb status                        # the store's path, counts and disk use
kb ls --collection react-router  # a collection's documents, a page at a time
kb sources                       # every source, with its document count
kb forget D-241                  # forget a document (kb compact drops its text)
kb embed                         # finish embeddings an add left pending
```

Forgetting and deleting collections lose research: do them only when the
user asks.
