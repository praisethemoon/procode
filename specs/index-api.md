# knowledge base — API specification

Status: draft. Working name `kb`; the tool's name is undecided and every
`kb`-prefixed identifier here is provisional.

A local, offline knowledge base over documentation, source trees and papers.
Research an agent has already done is indexed rather than discarded, and both
the agent and the reader search it afterwards.

No service, no daemon, no container. Embeddings are produced in-process by a
model loaded from disk (§8).

Bindings, in the pattern lap and coboard share:

| binding | consumer | notes |
|---|---|---|
| `kb` CLI (C) | scripts, humans, everything else | the only code that writes |
| MCP tools | agents | a subset (§9) |
| `kb-js` | `index-vscode` | shells out to the CLI; see §10 |

Every command takes `--json`. Exit codes: `0` success, `1` user or store error,
`2` internal failure.

---

## 1. Model

### 1.1 Identifiers

One identifier per entity, public, prefixed, monotonic, never reused:

```
S-3        source
D-241      document
C-99812    chunk
```

There is no hidden internal handle. The prefix carries the kind, so a reference
is self-describing in a search result, a citation, a log line or a prompt.

### 1.2 Entities

```
Source    { id, kind: url | file | dir | inline, locator, title, collection,
            fetchedAt, contentHash, etag?, docCount, bytes, status }

Document  { id, source, path, title, mime, contentHash, bytes,
            fetchedAt, indexedAt, chunkCount, meta{} }

Chunk     { id, document, ordinal, heading?, span: { start, end },
            tokens, text }

Link      { from, to, type }
```

`Source` is what was ingested from — a URL, a file, a directory walk, or
content handed in directly. `Document` is one addressable item within it.
`Chunk` is the retrieval unit.

`meta` is free-form per-document: for a paper, its authors and year; for a
page, its canonical URL and section path; for source code, its language and
symbol list. It is filterable but not schema-bound.

### 1.3 Collections

A collection is a flat named scope — `win32-iocp`, `io-uring`, `papers`,
`typec-refs`. It is created implicitly on first ingest and exists to keep one
research topic out of another's results.

Collections do not nest. A document belongs to exactly one.

### 1.4 The store

One store, and it belongs to the workspace.

| path | holds |
|---|---|
| `.kb/` at the workspace root, found by walking up like `.git` | the research belonging to this codebase |

There is no store in the home directory and no environment variable naming
one. `kb init` creates `.kb/` in the current directory; every other command
uses the first `.kb/` at or above it, and fails with `not_found` when there is
none rather than filing or reading anywhere else. Research that should outlive
a project is kept by committing its `.kb/` (§1.5), not by a second, shared
tier.

A store outside the workspace was cut deliberately. It made every read span
two corpora whose provenance a hit then had to carry, it made ingest's
destination depend on whether a `.kb/` happened to exist, and it was the one
place the tool computed a path in the user's home directory — which is where
a test harness that got that path wrong would delete.

Collections (§1.3) are the store's only subdivision. A **collection** decides
what topic a document belongs to; which workspace the store sits in decides
whose research it is.

### 1.5 What to commit

```
.kb/
  sources.jsonl        commit
  documents.jsonl      commit
  blobs/<hash>         commit
  index/               gitignore
```

`index/` is entirely derived and is binary and machine-local; `kb rebuild`
reconstructs it. The logs and blobs are the truth and are worth sharing: a
collaborator clones the repo, runs `kb rebuild`, and has the whole research
corpus without re-fetching or re-embedding anything. This is lap's posture
toward its own caches, for the same reason.

A committed corpus of fetched third-party documentation does grow the
repository and carries whatever licence the source did. `.kbignore` excludes
paths from ingest; a corpus that should not be committed can be kept out of the
repository by ignoring `.kb/` as a whole.

### 1.6 Storage shape

```
<store>/
  sources.jsonl        append-only; the truth
  documents.jsonl      append-only; the truth
  blobs/<hash>         document text, content-addressed
  index/               every derived structure, all disposable
    vectors.bin          int8 chunk vectors, flat
    fts.db               keyword index
    chunks.bin           chunk metadata and offsets
    links.bin            adjacency
    model.json           the model this index was built with (§8)
```

Everything under `index/` is a cache. `kb rebuild` reconstructs all of it from
the logs and the blobs. Document text lives in content-addressed blobs rather
than in the log, so re-ingesting an unchanged page writes nothing.

## 2. Ingest

| route | purpose |
|---|---|
| `POST /sources` | ingest by locator. `{ kind, locator, collection, meta?, options? }`. `url` is fetched; `file` and `dir` are read and walked |
| `POST /documents` | **ingest content directly**: `{ url?, title, content, mime?, collection, meta? }`. The caller already has the text — an agent that has just read a page hands it over instead of causing a second fetch |
| `POST /documents/batch` | many at once, one transaction |
| `GET /sources` | rows. `?collection=&kind=&status=&q=` |
| `GET /sources/{id}` | full, with document count and fetch history |
| `POST /sources/{id}/refresh` | refetch, compare by hash, re-embed only what changed |
| `DELETE /sources/{id}` | forget it and every document under it |
| `GET /documents` | rows. `?collection=&source=&mime=&q=&since=` |
| `GET /documents/{id}` | metadata. `?include=text,chunks,links` |
| `DELETE /documents/{id}` | forget one |

`POST /documents` is the load-bearing route. The intended flow is that an agent
performs deep research with whatever tools it has, and files what it read here
as it goes — so the second question on the same topic is answered from disk.

Ingest is idempotent by content hash: the same text at the same locator
re-indexes nothing and updates `fetchedAt`.

## 3. Chunking

Chunks follow document structure, not a fixed window. A Markdown or HTML
document splits on heading boundaries; source code splits on top-level
declarations; a plain text or PDF document falls back to a sliding window with
overlap.

Every chunk keeps `heading` and `span` back into its document, so a hit can be
shown in place rather than as a floating fragment.

Chunk size targets the model's context (§8) and is recorded in `model.json`,
because changing it invalidates the index exactly as changing the model does.

## 4. Search

```
GET /search?q=<text>
    &collection=win32-iocp,io-uring     scope
    &mode=hybrid | semantic | keyword   default hybrid
    &k=10                               hits, max 100
    &expand=1                           also return N neighbouring chunks
    &source=S-3  &mime=  &since=<iso>   filters
    &minScore=
```

**Hybrid is the default and is not an optimization.** This corpus is dense with
exact identifiers — `CreateIoCompletionPort`, `IORING_SETUP_SQPOLL`,
`EVFILT_READ` — and embeddings place near-synonyms on top of each other:
`io_uring_prep_recv` and `io_uring_prep_send` are neighbours in vector space
and opposites in practice. Keyword retrieval resolves those exactly; semantic
retrieval finds what the reader could not name. Results from both are fused
with reciprocal rank fusion.

A hit:

```
{ chunk: "C-99812", document: "D-241", source: "S-3",
  title, heading, snippet, collection,
  matched: ["keyword", "semantic"],
  scores: { bm25, vector, fused },
  fetchedAt, stale }
```

`matched` names which retrieval paths produced the hit. It is shown in the UI
and returned to agents, because a result found by both is a different kind of
result from one found by either.

`GET /chunks/{id}` returns the full chunk text and its neighbours;
`GET /documents/{id}?include=text` returns the whole document. **Search returns
snippets only.** The same discipline as coboard: a list must not be able to
flood a caller's context.

The knowledge base returns passages. It does not summarise, synthesise or
answer — that is the caller's work, and doing it here would bury the provenance
that makes the store worth having.

## 5. Provenance and staleness

Every document carries `fetchedAt`, its source locator and a content hash.

| route | purpose |
|---|---|
| `GET /stale?olderThan=90d&collection=` | documents whose age exceeds a threshold, newest sources first |
| `POST /refresh?collection=&olderThan=` | refetch a whole scope, re-embedding only changed content |

A hit whose document is older than the staleness threshold carries
`stale: true`. Documentation moves — Win32 pages get rewritten, io_uring's
surface changes release to release — and a passage that cannot say how old it
is will eventually be believed when it should not be.

## 6. Links

A small, optional layer over documents. Not a graph database.

| route | purpose |
|---|---|
| `GET /documents/{id}/links` | outgoing and incoming, resolved to rows |
| `POST /links` | `{ from, to, type }` |
| `DELETE /links/{from}/{type}/{to}` | remove one |

`type` is one of `supersedes`, `cites`, `analogue_of`, `implements`,
`see_also`.

Links connect documents only. Entity nodes — an API symbol, a concept, a
platform as first-class things — are **deliberately absent** until there is a
traversal that retrieval cannot answer. Modelling a graph before knowing the
query it serves produces a schema that is expensive to maintain and never
walked.

`analogue_of` is the one that motivated this layer: IOCP and io_uring and
kqueue solve the same problem three ways, and no amount of semantic similarity
will state that relationship.

## 7. Maintenance

| route | purpose |
|---|---|
| `GET /status` | the store's path (null when there is none), counts, index freshness, model identity, disk use |
| `GET /stats` | per-collection document, chunk and byte counts |
| `POST /rebuild` | reconstruct every derived structure from the logs and blobs |
| `POST /reindex` | rechunk and re-embed. Required after a model or chunker change |
| `POST /compact` | drop superseded blobs no live document references |
| `GET /collections` | names with counts |
| `PATCH /collections/{name}` | rename |
| `DELETE /collections/{name}` | forget a whole topic |

## 8. The model

```json
{ "model": "bge-small-en-v1.5", "dim": 384, "pooling": "cls",
  "maxTokens": 512, "queryPrefix": "Represent this sentence for searching relevant passages: ",
  "documentPrefix": "", "normalize": true,
  "chunkTokens": 400, "chunkOverlap": 60,
  "quantization": "int8" }
```

Recorded in `index/model.json` at first ingest. **Every embedding in the store
was produced by this configuration**, so a mismatch between it and the loaded
model makes the index meaningless rather than merely worse.

`GET /status` reports the mismatch and every search refuses with
`model_mismatch` until `POST /reindex` completes. Silently mixing vectors from
two models produces retrieval that is subtly, unaccountably bad.

The prefixes are part of the configuration because these models are
**asymmetric**: bge and e5 and nomic each want a different marker on a query
than on a document, and omitting it degrades retrieval while breaking nothing
visibly.

Weights load from `safetensors` or `GGUF`, from `~/.kb/models/`: the one
directory outside the workspace kb reads, holding exactly one model file,
shared by every workspace and never written by kb. The binary downloads
nothing (§12.2); a missing model is `model_missing`, and its message is the
command that fetches it:

```sh
mkdir -p ~/.kb/models && curl -fL -o ~/.kb/models/nomic-embed-text-v1.5.Q4_K_M.gguf \
  https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/main/nomic-embed-text-v1.5.Q4_K_M.gguf
```

Which model a workspace was indexed with stays pinned in its own
`index/model.json`, so a different file in `~/.kb/models/` is a
`model_mismatch`, not a silent change. Vectors are stored int8 and scanned
flat — at the scale a personal store reaches, an exact SIMD scan beats an
approximate index that also has to be maintained.

## 9. MCP tool surface

Six tools.

| tool | routes |
|---|---|
| `kb_search` | `GET /search` with every filter |
| `kb_get` | `GET /chunks/{id}`, `GET /documents/{id}` |
| `kb_add` | `POST /documents`, `POST /documents/batch` |
| `kb_collections` | `GET /collections`, `GET /stats` |
| `kb_links` | `GET /documents/{id}/links`, `POST /links` |
| `kb_stale` | `GET /stale` |

Not exposed: `rebuild`, `reindex`, `compact`, every `DELETE`. An agent files
knowledge into the workspace's store and reads from it; forgetting is the
reader's decision.

## 10. The JS layer

`kb-js` shells out to the CLI and parses `--json`. It does not reimplement
retrieval.

This differs from `lap-js`, which folds lap's log in-process — and the reason
is worth stating, because the asymmetry is easy to get wrong. Reading a lap log
is parsing. Searching here requires embedding the query, which requires the
model. A second implementation would mean a second inference path, and two
inference paths that disagree produce a store that returns different answers
depending on which door the caller came through.

One implementation, in C.

## 11. Errors

| code | details |
|---|---|
| `not_found` | — |
| `model_mismatch` | the stored configuration and the loaded one |
| `model_missing` | the expected weights path |
| `index_stale` | which structures need rebuilding |
| `unsupported_mime` | what was seen |
| `fetch_failed` | status and locator |
| `store_locked` | the holding process |
| `collection_in_use` | document count |

## 12. Unresolved

1. **Entity nodes.** §6 links documents only. Whether a symbol or concept
   deserves to be a node depends on a traversal query that has not yet been
   named.
2. **Fetching.** `POST /sources` with `kind: url` requires an HTTP client in
   the CLI. The alternative is to fetch nothing and accept content only through
   `POST /documents`, keeping the binary free of TLS entirely.
3. **PDF.** Papers are a stated target and PDF text extraction is a dependency
   of real size. Ingesting pre-extracted text through `POST /documents` avoids
   it.
