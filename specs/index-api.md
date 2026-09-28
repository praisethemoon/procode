# knowledge base — API specification

Status: draft. Working name `kb`; the tool's name is undecided and every
`kb`-prefixed identifier here is provisional.

A local, offline knowledge base over documentation, reference source and
papers. Research an agent has already done is indexed rather than discarded,
and both the agent and the reader search it afterwards.

It holds what was **read**, and, when the reader asks for it, code: another
project's API, a kernel's io_uring code at a pinned version, or a folder of the
workspace's own. A folder is filed with `kb add --dir` (§2.1), which reads it
as git sees it and files it again incrementally, so keeping an index of code
that changes costs one re-run, not a rebuild. grep and the language server
still answer exact questions about the workspace's code; the index answers the
ones nobody can phrase as a pattern.

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

`Source` is what was ingested from — a URL, a file, a folder, or content
handed in directly. `Document` is one addressable item within it: a folder's
documents are its files, each at its `path` under the folder; every other
source has one document, at path `""`.
`Chunk` is the retrieval unit.

`meta` is free-form per-document: for a paper, its authors and year; for a
page, its canonical URL and section path; for source code, its language and
symbol list. It is filterable but not schema-bound.

`etag` is the HTTP ETag the caller's fetch saw, handed over with the content;
the binary fetches nothing itself (§12.2), so it cannot learn one. `status` is
`ok`, or `fetch_failed` once reading the source again failed, until something
is filed from it again.

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
none rather than filing or reading anywhere else.

A `.kb/` counts as a store only when `kb init` made it (it holds
`documents.jsonl` from the start), and **the home directory's `.kb/` never
does**: it holds the machine's models (§8), and were it a store, every folder
under the home directory without one of its own would read and write there.
Discovery walks past both, as it walks past a plain file named `.kb`, and
`kb init` in the home directory is refused with `init_failed`. No command
creates a store implicitly. Research that should outlive
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
| `POST /sources` | ingest by locator. `{ kind: file \| dir, locator, collection, meta? }`. A file is read; a folder is walked (§2.1). A `url` is not fetched by the binary (§12.2); whoever fetched the page files it through `POST /documents` |
| `POST /documents` | **ingest content directly**: `{ url?, title, content, mime?, collection, meta? }`. The caller already has the text — an agent that has just read a page hands it over instead of causing a second fetch |
| `POST /documents/batch` | many at once, one transaction |
| `GET /sources` | rows. `?collection=&kind=&status=&q=` |
| `GET /sources/{id}` | full, with document count and fetch history |
| `POST /sources/{id}/refresh` | refetch, compare by hash, re-embed only what changed. A `file` source is read again, a `dir` source walked again (§2.1); a `url` one is re-filed through `POST /documents` while §12.2 keeps HTTP out of the binary |
| `DELETE /sources/{id}` | forget it and every document under it |
| `GET /documents` | rows. `?collection=&source=&mime=&q=&since=&meta=`. `collection` is a comma list as in §4; `since` a timestamp or a date; `meta` a JSON object whose every key must match, an array value matching any one element |
| `GET /documents/{id}` | metadata. `?include=text,chunks,links` |
| `DELETE /documents/{id}` | forget one |

`POST /documents` is the load-bearing route. The intended flow is that an agent
performs deep research with whatever tools it has, and files what it read here
as it goes — so the second question on the same topic is answered from disk.

Ingest is idempotent by content hash: the same text at the same locator
re-indexes nothing and updates `fetchedAt`.

**Filing does not wait for every embedding.** A document is searchable by
keyword the moment its add returns; its chunks are embedded within a time
budget (20 seconds by default, `--embed-budget S` to change it, `--wait` for
no limit), and whatever is left is `pending` in the answer. `kb embed`
finishes pending embeddings (so do `rebuild` and the next add), and a search
meanwhile uses the vectors that exist (§4). A chunk that is mostly digits,
punctuation or markup, or has almost no whitespace (inline SVG, minified code,
encoded data), is indexed for keyword search only and never embedded.

### 2.1 Folders

`kb add --dir <folder> --collection <c>` files a folder as **one `dir`
source**, located by the folder's absolute path, with one document per file at
its path under the folder and titled `<folder name>/<path>`. The same folder
into the same collection again finds that source and each document by path:

- a file whose text is unchanged is touched, not re-indexed;
- a changed file is a new version of its document, under the same id;
- a new file is a new document;
- a file that is gone, deleted or now ignored, is **forgotten**. With
  `--no-forget` it is kept and reported as `missing` instead; the MCP tool
  always files that way, because forgetting is not an agent's decision (§9).

**Which files.** The folder is read as git would track it: every `.gitignore`
from the repository's root down (the nearest directory at or above the folder
holding `.git`), with git's rules — negation, anchoring, directory-only
patterns, `**` — and `.git/info/exclude`. The user's global excludes file is
not read, since kb reads nothing in the home directory. Beyond git, a file is
left out when it is hidden (a segment starting with `.`), under a directory of
someone else's code (`node_modules`, `vendor`, `third_party`,
`bower_components`, `__pycache__`, `venv`, `site-packages`), not a source or
documentation type kb knows by its name, generated (a lock file, a minified
bundle, generated protobuf code, or a `@generated` / "DO NOT EDIT" marker in its
first five lines), not text (a NUL byte, or not UTF-8), empty, or over 1 MiB
(JSON over 64 KiB: past that it is data). The answer counts each reason:

```json
{ "ok": true, "source": "S-4", "root": "/work/lap/cli/kb-cli", "collection": "code",
  "files": 92, "added": 3, "updated": 1, "unchanged": 88,
  "forgotten": ["D-51"], "missing": [],
  "skipped": { "ignored": 2, "hidden": 0, "vendored": 1, "generated": 0,
               "binary": 0, "large": 1, "unreadable": 0, "otherTypes": 0 },
  "embedded": 14 }
```

A code file's document carries its language and definitions in `meta`:
`{"language": "c", "symbols": [{"name", "kind", "line"}, ...]}`, the first 500.

The whole folder is one locked section, one keyword rebuild and one embedding
pass. Progress goes to standard error when it is a terminal.

## 3. Chunking

Chunks follow document structure, not a fixed window. A Markdown or HTML
document splits on heading boundaries; a plain text or PDF document falls back
to a sliding window with overlap.

**Source code splits along its syntax tree** where kb has a grammar for it
(C, TypeScript, TSX, JavaScript, Python, Go, Rust and assembly, chosen by the
file's extension, or by its mime type for a single file filed without a
path): whole definitions when they fit the chunk budget, split along their
own children when they do not, and small neighbours merged, with the comments
and attributes above a definition kept with it. A chunk's heading is
`container > signature`, the first line of its first definition under the
definitions it sits inside (`export class Kb { > async addDir(...)`). A file
whose tree is more than 10% parse error is cut into line windows instead;
code in a language without a grammar splits on top-level declarations found
line by line.

Every chunk, prose or code, is indexed and embedded under a header line, `<title>
> <heading path> > <heading>`, ahead of its text: the file and the section or
function a passage is in are words a search should match, and the passage
itself seldom says them. The keyword index counts the title three times and the
heading path and heading twice (BM25F by repetition). A code
chunk's **symbols** — the names its grammar's tags query marks as definitions
(functions, methods, classes, types, modules; labels in assembly) — count
three times too, so a search for a name lands on its definition before its
uses. A run of tiny sibling sections (each under an eighth of the budget,
under the same parent heading) shares one chunk while it fits.

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
    &minScore=                          floor on the bm25 score, before fusion
    &rerank=true                        cross-encoder reorders the fused top 10
```

**Hybrid is the default and is not an optimization.** This corpus is dense with
exact identifiers — `CreateIoCompletionPort`, `IORING_SETUP_SQPOLL`,
`EVFILT_READ` — and embeddings place near-synonyms on top of each other:
`io_uring_prep_recv` and `io_uring_prep_send` are neighbours in vector space
and opposites in practice. Keyword retrieval resolves those exactly; semantic
retrieval finds what the reader could not name. Each list is taken to depth
`max(3k, 50)`, and the two are fused **by score**: each list's scores are
min-max normalised over that list, so the best answer of each is 1, and a
chunk scores `a · vector' + (1 − a) · bm25'`, 0 on a side that did not find
it. `a` is 0.6, and 0.3 for a query that is one identifier-shaped word (an
underscore, an inner capital, a digit, `::`, `->`, `.`, `()`, or a leading
dash), which is looking for that exact name. Both weights were chosen by
cross-validation on the benchmark (`cli/kb-cli/bench/tune_fusion.py`).
Reciprocal rank fusion (k = 60) stays as `--fusion rrf`.

A store without vectors — no model recorded, or a vector file written under
another model — answers a search that names no mode with keyword, and says so
in the response's `mode`. Asking for `hybrid` or `semantic` by name is refused
with the reason (§11). A store whose vectors are only partly there (an add
that left its embedding pending, §2) searches with the vectors it has and says
how many chunks the semantic side did not see, as `unembedded` in the answer.
`bm25` and `vector` appear in `scores` only for the paths that found the hit.

**Reranking is opt-in** (`--rerank` on the CLI). A cross-encoder rescores the
fused top 10, reading each pair as the query against the chunk's header line
and text, cut to 512 tokens per pair, and those hits are put in the order of
its scores; the rest keep the fused order after them. It costs seconds a query
on CPU and needs the reranker model file in `~/.kb/models`: without it the
search is refused with `model_missing` rather than answered unreranked. Hits it
rescored carry its logit as `scores.rerank`; the others do not.
`--rerank-depth N` and `--rerank-tokens N` change how many candidates it reads
and how much of each, the two things its latency turns on.

A hit:

```
{ chunk: "C-99812", document: "D-241", source: "S-3",
  title, heading, snippet, collection,
  matched: ["keyword", "semantic"],
  scores: { bm25, vector, fused, rerank },
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
| `DELETE /collections/{name}` | forget a whole topic; refused with `collection_in_use` while it holds documents unless asked to forget them too (`?withDocuments=true`) |

## 8. The model

```json
{ "model": "gte-modernbert-base", "arch": "modernbert", "dim": 768, "pooling": "cls",
  "maxTokens": 1024, "queryPrefix": "", "documentPrefix": "", "normalize": true,
  "chunkTokens": 400, "chunkOverlap": 60,
  "quantization": "int8", "weights": "F16", "tokenizer": 1 }
```

kb's default model is **gte-modernbert-base** (Alibaba-NLP, Apache-2.0), one
embedder for prose and code alike, run in F16 from a GGUF that
`cli/kb-cli/tools/modernbert/convert.py` makes from the Hugging Face weights
(`MODERNBERT.md` there has the revisions, the checksums and every fact the
port reproduces). nomic-embed-text-v1.5 is still supported.

Recorded in `index/model.json` at first ingest, when there is a model to
record, and again by `POST /reindex`. **Every embedding in the store was
produced by this configuration**, so a mismatch between it and the loaded
model makes the index meaningless rather than merely worse. Beside §8's
fields the file carries what a mismatch also turns on: `arch`, how the
weights were quantised (`weights`, e.g. `Q4_K_M` — two quantisations of one
model are two sets of vectors), the tokenizer's version, a `fingerprint` over
all of these, and the model file itself (`modelFile: { name, bytes, sha256 }`).
A store with no model recorded is keyword-only.

`GET /status` reports the mismatch and every search refuses with
`model_mismatch` until `POST /reindex` completes. Silently mixing vectors from
two models produces retrieval that is subtly, unaccountably bad.

The prefixes are part of the configuration because some models are
**asymmetric**: bge and e5 and nomic each want a different marker on a query
than on a document, and omitting it degrades retrieval while breaking nothing
visibly. gte-modernbert-base is symmetric and has none. It reads each text as
sentence-transformers hands it over, with whitespace stripped from both ends.

Weights load from `GGUF` files in `~/.kb/models/`: the one directory outside
the workspace kb reads, shared by every workspace and never written by kb. A
file whose `kb.role` is `reranker` is not an embedder and is passed over. Of
the embedders, one alone is used; with several, `gte-modernbert-base.F16.gguf`
is, and without it kb refuses rather than let a directory listing choose. The
binary downloads nothing (§12.2); a missing model is `model_missing`, and its
message says how to make the default, or fetch nomic's as it is:

```sh
mkdir -p ~/.kb/models && curl -fL -o ~/.kb/models/nomic-embed-text-v1.5.Q4_K_M.gguf \
  https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/main/nomic-embed-text-v1.5.Q4_K_M.gguf
```

A store indexed with nomic, once gte-modernbert-base is in place, reports the
mismatch until `kb reindex` embeds it again under the new model.

Which model a workspace was indexed with stays pinned in its own
`index/model.json`, so a different file in `~/.kb/models/` is a
`model_mismatch`, not a silent change. Vectors are stored int8 and scanned
flat — at the scale a personal store reaches, an exact SIMD scan beats an
approximate index that also has to be maintained.

Documents are embedded with the model's layer products in 8-bit integers
(weights and activations quantised per block of 32, `quant.h`), about a third
faster than float and within cosine 0.999 of it on real text; the benchmark
scores the two the same. The reranker, reading ten passages a search, runs in
int8 too; the one query a search embeds runs in float, since converting a
model for a single text saves nothing. Vectors computed one way are not
mixed with the other: the vector file's fingerprint says which.

`index/vectors.bin` holds one vector per chunk **keyed by chunk id**, with the
model fingerprint it was produced under. A chunk id names one passage forever
(§1.1), so a stored vector stays right for as long as the model does: ingest
and `rebuild` embed only chunks that have none and drop the ones whose chunks
are gone, and only `reindex` embeds everything again. Documents are embedded
with `documentPrefix`, queries with `queryPrefix`. `GET /status` reports the
vectors' count, how many live chunks have none, and whether they are current.

## 9. MCP tool surface

Six tools.

| tool | routes |
|---|---|
| `kb_search` | `GET /search` with every filter, `rerank` included (off by default: slower) |
| `kb_get` | `GET /chunks/{id}`, `GET /documents/{id}` |
| `kb_add` | `POST /documents`, `POST /documents/batch`; with `dir`, `POST /sources` of kind `dir` (§2.1), never forgetting: a file gone from the folder is reported as `missing` |
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

An error is `{ok: false, error, message, details?}`. `error` is the code,
`message` is prose for a person, and `details` carries the fields a caller acts
on, so nothing has to be parsed out of the message. `details` is absent when a
code has none to give.

| code | details |
|---|---|
| `not_found` | — |
| `model_mismatch` | `stored`, `loaded`: the two configurations, as §8 records them |
| `model_missing` | `path`: the expected weights path |
| `index_stale` | `structures`: which need rebuilding (`keyword`, `chunks`); `path` or `document` where one is at fault |
| `unsupported_mime` | `mime`: what was seen. Any `text/` type is accepted, and the `application/` types that are text (JSON, XML, YAML, TOML, JavaScript, TypeScript, shell, XHTML) |
| `fetch_failed` | `locator`; `status` when an HTTP fetch returned one |
| `store_locked` | `store`; `pid` of the holding process when it is known |
| `collection_in_use` | `collection`, `documents`: the count |

Without `--json`, a `usage` error is followed by the command's synopsis
(`usage: kb get <D-n> [--include …] …`). `kb <command> --help`, `-h` anywhere
before `--` (never as a flag's value) and `kb help <command> [<subcommand>]`
print that command's synopsis, what it does and every flag it takes, one line
each, to stdout, and run nothing — no store is needed. The help, that synopsis
and the flags each command accepts are read from one table
(`cli/kb-cli/src/help.c`), so a flag the parser takes is in the help by
construction.

## 12. Unresolved

1. **Entity nodes.** §6 links documents only. Whether a symbol or concept
   deserves to be a node depends on a traversal query that has not yet been
   named.
2. **Fetching — decided: the binary fetches nothing.** It has no HTTP client
   and keeps free of TLS. An agent or a surface fetches a page with its own
   tools and hands the text over through `POST /documents`, with the page's
   `etag` if it saw one. Refreshing a `url` source is therefore the caller's
   job (`index-vscode` asks before it fetches); `POST /sources/{id}/refresh`
   on one is refused with `fetch_failed`, and on a `file` reads it again.
3. **PDF.** Papers are a stated target and PDF text extraction is a dependency
   of real size. Ingesting pre-extracted text through `POST /documents` avoids
   it.
