# lap — specification (v0.1)

`lap` is a CLI that records **fine-grained, human-interpretable edit history**
for AI agents. It sits below git: agents record every small edit with its
intent and what it does; git keeps its normal human-scale history.

## Core rules

1. **A commit is one edit of one file.** An *edit* is one contiguous run of
   changed lines (insertions, deletions, replacements). Two changed areas
   separated by at least one **non-blank** unchanged line are two edits and
   must be committed separately. Blank (whitespace-only) lines carry no
   identity, so a gap made only of them never splits an edit: the changed
   runs merge into one region that spans the gap, blank lines included.
2. **Every commit states its intent and its behavior**, both required
   (§Messages). The *intent* says why the edit exists — the goal it serves.
   The *behavior* says what this edit makes the code do. Edits that serve
   one goal share an intent; each still describes its own behavior, so a
   hunk that cannot say what it does stands out.
3. **Commits belong to sessions.** A session is a named group of commits with
   a purpose ("fix the parser bug"). Committing requires an active session
   unless `--no-session` is passed explicitly.
4. **lap never writes to tracked files.** The working tree is authoritative;
   history is a recording. There is no checkout or staging. Branches
   (§Branches) are lines of history in other folders, never a switch of the
   working tree.
5. **Commit ids are short and sequential** (`L1`, `L2`, ...; sessions `S1`,
   `S2`, ...). Integrity comes from a hash chain, not content-addressed ids.
   Every commit also has a **hash** (§The log), which is how one commit's
   text refers to another.
6. **One folder, one line of history.** Parallel work happens in other
   folders, as branches (§Branches). When work arrives from elsewhere (a
   merged pull request), it lands in the working tree like any other change
   and is committed edit by edit.

## Repository layout

```
<root>/
  .lapignore        ignore patterns (tracked like any file)
  .lap/
    log/            TRUTH — the append-only record log, hash-chained, as
                    chunk files (§Chunks)
    state.json      cache: counters, active session, last chain hash
    shadow/         cache: last-committed content of every tracked file
    index           cache: one fixed-width entry per record (§acceleration)
    paths           cache: path table; file_id = line number
    heads           cache: per-file chain tail + snapshot byte budget
    snapshots/      cache: periodic full-content snapshots, per file
    statcache       cache: stat of every file status last found clean
    lock            exclusive lock file taken by writing commands
    lineage         a branch folder's id (§Branches): machine-local
    parent          a branch folder's parent path, for other tools (the
                    board): machine-local, a hint lap never reads
    branches.json   the branches started from this folder: machine-local
```

- Commands find the repository by walking upward from the cwd (like git).
- **The cache contract:** everything in `.lap/` except `log/` is a
  derived, disposable cache. Any of it may be deleted at any time; the next
  writing command (or `lap rebuild`) reconstructs it from the log. Cache
  formats may change between versions with no migrations — an unrecognized
  or stale cache is rebuilt. Caches are native-endian and single-machine:
  transport a repo as its `log/` (plus working tree) and rebuild on arrival.
- Writing commands hold the lock for their whole run and keep every cache
  in step; readers never lock and never write — with one exception, the
  stat cache below, which `status` refreshes. A reader that finds a cache
  missing or stale falls back to the log — correct, just slower — and heals
  counters in memory only. This holds **per file**: deleting any single
  cache must change speed only, never output. In particular a missing
  shadow entry is reconstructed by replay rather than reported as an
  untracked file, and an index whose `paths`/`heads` sidecars are missing
  or inconsistent is rejected whole rather than half-used.
- Cache publication is ordered so a crash can only ever understate what is
  cached: the index writes its entries and sidecars first and its header —
  the `covered` freshness marker readers trust — last, each step durable
  before the next.

## The acceleration layer

- **Index** (`.lap/index`): a 40-byte header then one 64-byte entry per log
  record — byte offset/length into the history (its chunks read as one
  stream, §Chunks), kind, op, numeric session,
  `file_id`, epoch timestamp, the region coordinates, a running commit
  counter (ids are dense integers, so finding `L<n>` is a binary search and
  per-record access is a seek), and `prev_same_file`: the entry index of
  the file's previous commit. Blame, per-file log, and replay walk that
  chain and never touch unrelated records. `amend` records are entries of
  their own kind: a reader decodes them once and gives each commit it
  fetches its latest text, as a full scan would.
- **Snapshots** (`.lap/snapshots/<path>.jsonl`): one JSON line per snapshot
  `{"at": <record index>, "eof_nl": …, "content": "…"}`. Policy (tunable,
  cache-only): a writer emits one after a commit when the file has
  accumulated more than 2× its current size in record bytes and at least
  16 commits since the last snapshot.
- **The replay contract (normative):** replay of a file to record k starts
  from the nearest *available* snapshot at or before k and applies the
  file's deltas after it; with no snapshot available it replays from
  birth. The results are identical either way — snapshots change speed,
  never outcome — so any reader may ignore snapshot containers it does not
  understand.
- **Stat cache** (`.lap/statcache`): for each tracked file `status` last
  found equal to its committed state, the file's size and mtime (with
  nanoseconds where the platform keeps them) and the index entry of the
  file's last commit. `status` reads a file only when its size, mtime or
  last commit differs from its entry; with an index, it also finds deleted
  files from the index rather than by walking the shadow store.
  - **The reader exception.** `status` is a reader, and this is the one
    cache a reader writes: only when its entries changed, only under the
    writer lock taken with a non-blocking try (a held lock means no
    refresh, never a wait), and atomically (a temp file renamed over).
    Only files this run read and found clean are added.
  - **Racy entries.** A file is entered only when its mtime is older than
    the second the run started, so an edit that lands within the same
    timestamp tick as a check — same size, same mtime — is never taken
    for clean.
  - Like every cache, deleting it changes speed only. It trusts size and
    mtime, as git's index does: content changed with both put back is not
    seen until the entry goes.
- `lap verify --deep` audits the layer: every shadow and every snapshot
  must equal a from-birth replay of the truth.
- History is **linear by construction** in each folder: a branch is
  another folder's line (§Branches); within a folder, concurrency is
  serialized by the writer lock and expressed as interleaved sessions.
- Resolving a commit by hash (§References) scans the log's commit records;
  the index does not store hashes. A cache may be added for it later under
  the cache contract, without changing any output.

## The log

One JSON object per line. Field order is fixed at encode time. Every record
carries `prev`: the SHA-256 (lowercase hex) of the previous record's exact
bytes; the first record chains from 64 zeros. `lap verify` walks the chain.

A record's **hash** is the SHA-256 of its own exact bytes — the value the
next record carries as `prev`. It is never stored in the record itself.
A commit's hash is its stable name for references (§References); its
**short hash** is the first 7 hex digits.

Record types:

```jsonc
{"type":"init","version":1,"ts":"...","prev":"..."}

{"type":"commit","id":"L7","user":"jane",      // who committed (see below);
                                               // absent in pre-user logs
 "session":"S2",                               // session null if --no-session
 "file":"src/foo.c","op":"edit",               // op: edit | create | delete
 "old_start":10,"old_lines":2,"new_start":10,"new_lines":3,
 "eof_nl":true,                                 // trailing-\n state after commit
 "old_text":["..."],"new_text":["..."],         // full replaced/replacement lines
 "intent":"...","behavior":"...",               // §Messages
 "forced":true,                                 // present only with --force-message
 "ts":"...","prev":"..."}

{"type":"session_start","id":"S2","user":"jane","msg":"purpose",
 "meta":{"ticket":"T-12"},"ts":"...","prev":"..."}   // meta: optional
{"type":"session_end","id":"S2","ts":"...","prev":"..."}

{"type":"branch","id":"7c1e9a02d4b8","name":"parser-fix", // §Branches
 "parent":"main","base":"<parent head>","base_chunk":3,
 "user":"jane","ts":"...","prev":"<the same base>"}

{"type":"merge","branch":"7c1e9a02d4b8","name":"parser-fix", // §Merging
 "head":"<branch hash adopted up to>","adopted":41,"left":6,
 "stopped":[{"file":"src/foo.c","at":"<first commit not adopted>"}],
 "user":"jane","ts":"...","prev":"..."}

{"type":"amend","of":"<the commit's hash>",       // §Amendments
 "user":"jane","intent":"...","behavior":"...",
 "forced":true,                                 // present only with --force-message
 "ts":"...","prev":"..."}
```

A commit, `session_start` or `session_end` that `lap merge` adopted carries
`"from":"<hash of the branch's record>"` (after `forced`, after `meta`,
after `id` respectively) and keeps the branch record's `ts` and `user`.

A commit record without both `intent` and `behavior` is malformed,
including one that carries a single `msg` in their place.

**Amendments.** An `amend` record corrects what a commit says, never what
it did: `lap amend` appends it, and nothing already written changes — a
rewritten record would change its hash and every hash after it, and with
them branches' bases, `from` links, `#hash` citations, chunks sealed under
git and the chain's evidence. `of` always names the **commit** (never an
earlier amendment); a commit amended twice has two records with the same
`of`, and the **last in log order is the latest**. Its `intent`,
`behavior` and `forced` replace the commit's wherever the commit is shown:
`log`, `show`, `search` (`--msg` matches the latest text), `rr` and every
JSON shape (`"amended":<n>` counts the amendments). `show` also lists the
earlier texts, oldest first, each with who wrote it and when (JSON
`earlier`, with `amended_by` and `amended_ts` for the latest). An amend
record is no history entry of its own: `log`, `rr`, `search` and the views
never list one. The commit → latest-text link is derived by readers, and
cached only under the cache contract (the index keeps amend records as
entries of their own kind); it is never written into the log. An `amend`
without `of`, `intent` or `behavior` is malformed. One that `lap merge`
carried from a branch has `from`, after `of` (§Merging).

Sessions keep `msg`: a session's purpose is already an intent.

`user` identifies the committer on `commit` and `session_start` records.
Resolution order: `$LAP_USER` (agents/orchestrators set this to tag their
identity, e.g. `LAP_USER="claude/refactor-worker-2"`) > `git config
user.name` (best-effort shell-out; git is NOT a dependency) >
`$USER`/`$USERNAME` > `"unknown"`. Old logs without the field stay valid.

Region semantics: replace old lines `[old_start, old_start+old_lines)` with
`new_text`. Starts are 1-based; `old_lines == 0` is a pure insertion before
`old_start`, `new_lines == 0` a pure deletion. `create` records the whole
file as one insertion (empty history = one big change, like git); `delete`
records the entire removed content. `new_start` is a **committed-file**
coordinate: the committed state is the previous state with only this region
applied, so `new_start == old_start` by construction — never the
working-file position, which may be shifted by other still-pending edits.
Blame and hunk headers rely on this.

Timestamps are stored as UTC ISO-8601 (`2026-09-20T12:34:56Z`), and JSON
output carries them as stored. Human output shows them in the machine's
time zone, to the second, year first: `2026-09-20 14:34:56`; `lap show`
adds the offset (`2026-09-20 14:34:56 +02:00`) so a shared copy still names
one moment. `--since`/`--until` take what the output shows: a date, or a
date and time (`T` or a space between, seconds optional), read as local
time; with `Z` or an offset (`+02:00`, `-0400`) it is taken as given. In
the hour that repeats when clocks go back, a local time means the earlier
of its two moments. Anything else is refused with `bad_time`. Bounds are
converted to UTC and compared as strings, which sort as times.

**Records of a newer type.** From this version on, a lap reads a history a
newer lap wrote as far as it understands it. A record whose `type` it does
not know is kept in the chain (its `prev`, `ts` and hash are checked like
any record's) and otherwise skipped, with one notice per command: *"this
history has records of a newer type ("annotate") … Update lap to see them"*.
`verify` checks their chain and reports them (`unknown_records`,
`unknown_type` in JSON). A **writer refuses** such a history
(`newer_history`) before it repairs, heals or writes anything: what those
records mean — a merge's, an amendment's — could make its write wrong.
The rule protects the versions from this one on; an older lap refuses the
whole history, as it always did.

**Version note.** A history holding `amend` records needs the lap that
added `lap amend`, or a later one. A lap from the version above reads it
(commits show their original text) and writes nothing to it; an older one
refuses it whole.

### Chunks

The log is kept as chunk files in `.lap/log/`, read in order as one stream:

```
.lap/log/
  main.000001.jsonl      sealed
  main.000002.jsonl      sealed
  main.000003.jsonl      open: appends go here
```

- A chunk is named `<lineage>.<n>.jsonl`: the lineage is `main` (other
  lineages come with branches, §Branches), and `n` counts its chunks
  from 1, six digits, zero-padded. Other files in the directory are ignored.
- A lineage's **open** chunk is its highest `n`; every lower one is **sealed**
  and never written again. When an append would take the open chunk past
  4 MB, the record starts chunk `n + 1` instead; a single record larger than
  that is a chunk of its own. 4 MB is a constant, not a setting, so every
  folder chunks alike. (`LAP_TEST_CHUNK_BYTES` overrides it, for tests
  only.)
- **Order is the hash chain.** Chunks follow `n`, and a chunk's first record
  carries the previous chunk's last hash as `prev`, so the chain runs on
  across chunks exactly as it would in one file. No manifest lists the
  chunks: there is nothing for git to conflict on. A missing number (chunks
  1 and 3 but not 2) is refused, naming the missing chunk.
- Positions in the history — the index's offsets — are offsets into the
  chunks read as one stream. Only the open chunk grows, so a position never
  moves.
- **Torn tails** can occur only in the open chunk (§Concurrency).
- **`lap verify`** walks the chain across chunks and names positions by chunk
  and line. A break that points at a sealed chunk is reported as
  *"sealed chunk `main.000002.jsonl` was modified"*: lap never writes a
  sealed chunk, so only a mistake (a bad conflict resolution, a
  repository-wide replace) can have changed one. At a chunk's first record
  the boundary alone cannot say which side changed, so its `prev` decides:
  a record inside the chunk before means that chunk was appended to (it is
  the one modified); the last record of another chunk means this chunk is
  a stray or a duplicate (*"chunk `main.000006.jsonl` does not belong after
  `main.000005.jsonl`: it continues `main.000002.jsonl`"*); anything else
  names both (*"chunk `main.000003.jsonl` does not continue sealed chunk
  `main.000002.jsonl`"*).
- git tracks `.lap/log/`. Appending changes one file, the open chunk; a
  sealed chunk never changes again. A chunk written whole (a branch's
  first, one `lap merge` copies) goes through a temp file in `.lap/` and
  one rename, so `.lap/log/` never holds a temp file; one left there by an
  older lap is not a chunk to readers, and the next writer, holding the
  lock, removes it.
- **The single-file log of older versions.** A folder holding
  `.lap/log.jsonl` and no main chunk is read as it is, and readers never
  change it. The first writing command splits it at record boundaries into
  `main.000001.jsonl`, … at the limit, then removes it (a torn final line
  is dropped, as any writer drops one; an old file with no complete record
  becomes one empty `main.000001.jsonl`, a history with nothing in it yet,
  never none). Records and hashes are unchanged,
  so `verify` passes before and after, and git sees one deleted file and
  some new ones, once. The chunks are written into `.lap/log.converting/`
  and published as `log/` with one rename (an existing `log/` moved aside
  first); only then is the old file removed. So a reader finds the old file
  or all the chunks, never some — and a reader finding both reads the old
  file unless the chunks hold all of it, and reads it whole when it opens
  it, since a conversion may remove it meanwhile. A writer finding both
  finishes a split whose chunks are a prefix of the old file, removes an
  old file that is a prefix of the chunks when what follows it continues
  its chain (records appended since), splits it again when what follows
  breaks the chain and repeats its records (leftover chunks of a run under
  another chunk limit), and refuses anything else,
  keeping both.

## Branches

A **branch** is a lap repository in another folder — a git worktree or a
plain copy — whose history is its parent's up to a **base** (the hash of
the parent's last record when the branch started), then its own. Its own
records form a **lineage** named by a 12-hex-digit id; the first folder's
lineage is `main`. A branch's chunks are `<id>.<n>.jsonl` beside its
parent's in `.lap/log/`, so two lines of history never write one file,
and a `git merge` of the branch's code brings its chunks as new files.

- A branch folder's history is the parent's chunks `1 … base_chunk`, then
  its own. The **branch record** opens its own chunk 1: it names the
  branch, its parent lineage, the base and the parent's chunk the base
  ends, and chains from the base. The base is always the last record of a
  sealed parent chunk (below), so the history is whole chunks.
- **Which lineage a folder writes** is `main`, unless `.lap/lineage` names
  a branch id. That file is machine-local, like the registry: after a `git
  merge` the parent holds the branch's chunks too, and the chunks alone
  cannot say which lineage is the folder's own. So it never goes through
  git: `lap init` and `lap branch start` write `.lap/.gitignore`
  (`/*`, `!/.gitignore`, `!/log/`, kept if one is there), leaving git only
  the history. Should a `lineage` arrive anyway, with the `.lap/parent`
  that names this very folder (compared as a directory on disk), it is
  ignored with a notice: the folder stays `main`.
- Ids go on from the base: the branch's next `L` and `S` numbers follow
  its parent's at the base, so one folder never shows an id twice. Two
  folders do — the parent goes on from the base too — so text that leaves
  a folder names commits by hash and sessions as `<branch>/S<n>`
  (§Naming sessions).
- A branch starts with no session open, whatever its parent had open.
- **A branch of a branch** starts from a branch folder the same way: its
  record's parent is that branch's lineage, and its base chunk one of
  that branch's own chunks. A branch record holds one hop, so a nested
  branch's history is found by following branch records one at a time up
  to `main`: main's chunks to the first branch's base chunk, that branch's
  own chunks to the next one's base chunk, and so on, then its own. A
  chain that leads back to itself (more than 32 hops) is a broken
  history. Every branch a history spans is labelled by its name.

### Starting one

The folder is made by whoever wants it — `git worktree add`, or a plain
copy of the parent folder; lap does not create folders or run git. Then, in
the new folder, `lap branch start [name] --from <parent folder>`, which,
holding the parent's lock throughout:

1. **History.** A folder with no history gets the parent's. One that has a
   history (a copy, or a worktree whose git commit carries `.lap/log/`)
   must hold a prefix of the parent's, byte for byte, else
   `unrelated_history`; it is brought up to the parent's head.
2. **Files.** Every file must equal the parent's committed state at the
   base, as `lap status` would find it there; else `not_clean`, listing
   them and naming the usual causes: a worktree checked out from a git
   commit older than lap's history, and files lap tracks that git ignores.
3. **Sealing.** The parent's open chunk is sealed (its next chunk created,
   empty). The chunk this folder copies is then final on both sides, and a
   later `git merge` finds it unchanged. An open chunk that is still empty
   is not sealed again: a second branch started before the parent appended
   anything shares the first one's base.
4. **Registration.** The branch is added to the parent's `branches.json`
   (§Registry). A parent that cannot be written refuses the start
   (`parent_read_only`) before anything is written in this folder.
5. **The branch record** starts the folder's lineage, and `.lap/lineage`
   names it. A registry copied along with the folder is dropped.

The id is the first 12 hex digits of SHA-256 over the base, the name, the
time and a nonce, so two copies of one folder never make the same one. The
name is 1–64 letters, digits, `.`, `_` or `-`, not `main`, unique among the
parent's branches (`name_taken`); it defaults to the id.

**The recommended layout** keeps the first folder quiet: agents work in
branch folders, and the first folder only merges. A branch then always
starts from a folder nobody is editing.

### Committing: say where

In a folder that is a branch, or that has branches registered, `lap
commit` (dry runs included) and `lap session start` need `--branch
<name>` — the branch's name or id, or `main` — or else `branch_required`.
A name that is not this folder's is `wrong_branch`, in any folder. Both
errors name this folder's branch, so the fix is in the message. The
environment variable `LAP_BRANCH` counts as the flag when the flag is not
given (the flag wins), so an orchestrator sets it once per agent and tools
that call lap need not pass it. A folder with no branches at all keeps its
commands unchanged.

It costs a flag and catches the one mistake that corrupts a merge: an agent
recording in a folder it thinks it is not in.

### Merging

git merges the code; lap adopts the history. The order is always: commit
your own pending work, `git merge` the branch's code, then, in the parent
folder, `lap merge <branch> [--dry-run]`.

1. **Finding it.** The branch is named by its name or id, found in the
   registry — this folder's, or the registry of one of its branches, for
   a branch of a branch — or among the branch chunks in `.lap/log/`
   (`git merge` brings them); none → `branch_not_found`. A branch folder
   merges the branches started from it, as main does. **The chain** is
   the branch and each branch it started from, up to the first whose
   history this folder already holds: the branch alone when it merges
   into the branch it started from; the branches between as well when a
   branch of a branch merges straight into main — git's merge of it
   brings their code up to its base, so lap adopts their history up to
   it too. A branch whose history this folder already holds (its own, or
   one it started from) → `merge_in_branch`. The outer branch record's
   base must be in this folder's history, else `unrelated_history`.
2. **Its chunks: the ones here first.** lap reads the branch's history
   from this folder's copies of its chunks, as they are: `git merge`
   brought them (or an earlier merge wrote them). Adoption runs as far as
   those copies reach, which under git is exactly the history whose code
   `git merge` brought, so history and files agree at every step; the next
   `git merge` and `lap merge` adopt the rest. lap never rewrites a copy
   git brought, so the next `git merge` finds that file unchanged on this
   side and never conflicts on `.lap/log/`. Nothing in the branch folder is
   sealed or written. A chain is read as one history: this folder's up to
   the outer branch's base chunk, then each branch's own chunks up to the
   next one's base chunk, each branch's chunks found as below.

   The registered folder (reachable: it exists and its `.lap/lineage`
   names the branch) fills in only what is not here:
   - **A git checkout** (the branch folder has a `.git` entry, a directory
     or a worktree's file; lap never runs git): with no chunk of the branch
     here, `lap merge` refuses with **`git_merge_first`** and writes
     nothing — run `git merge` first. Taken from the folder, the history
     would run ahead of the code, and the next `git merge` would conflict
     on the copies. **`--copy-from-folder`** takes the folder's chunks
     anyway, for a user who wants the history before the code, knowingly.
   - **No git** (plain copies): nothing brings the history by itself, so
     lap takes from the folder every chunk missing here, and extends a copy
     here that is a byte prefix of the folder's. History may then run ahead
     of files the user has not copied back yet; `lap status` shows that
     difference.

   Chunks taken from the folder are written here once every check has
   passed (a dry run writes none), so the history stays readable after the
   folder is gone. Only complete lines are taken: a line the branch is
   still writing waits for the next merge.
3. **Placing, file by file.** A merge takes the branch's records after the
   last merge's `head` (after the branch record, the first time); for a
   chain, after the latest head this folder took in of any of its
   branches — a branch the chain passes through that was merged here past
   the chain's base is taken in whole. The chain's commits are placed as
   one run, in history order, so a conflict in an outer branch's part
   stops the file for the branches after it too. For each
   file those commits touch, the three versions are the branch's version at
   that head (for the first merge, the file at the base), the parent's
   version now, and the commits. The parent's changes are the diff between
   the first two, lap's own (§Edit detection, effort cap included: a file
   the parent rewrote past the cap is one change, and every branch commit to
   it conflicts). A commit whose region neither overlaps nor touches a
   parent change moves by the net lines the parent added or removed above
   it; each placed commit shifts the later ones. Overlapping or touching —
   both sides inserting at one point, or one at the edge of the other's
   change — is a conflict, and so is a commit whose old text is not the
   parent's text where it would land (compared blind to CRLF `\r`). **The
   first conflict stops that file**: none of its later commits are adopted,
   in this merge or later ones. Other files carry on.

   **Whether the file exists** is judged first, since an empty file and
   a missing one have the same (no) lines: a commit that edits a file the
   parent deleted is a conflict (`the parent deleted the file`); a create
   of a file the parent has is already done when the lines (and the final
   newline) are the same, else a conflict (`both sides create it`); a
   delete of a file the parent deleted is already done.

   **Identical changes are already done.** A commit that overlaps exactly
   one parent change, of exactly its region and size, and lands where the
   parent's text already is its new text (with the same final newline) is
   not a conflict: the parent made the same change. Nothing is adopted for
   it and nothing stops; that change is common ground for the file's later
   commits. A file deleted on both sides, or created on both with the same
   lines, is this case. Identical means the text, as lap's diff aligns it:
   a change one line different is a conflict. A rename (delete one path,
   create another) is two changes, each judged by itself.
4. **Appending**, under the lock, in the branch's order:
   - a branch `session_start` not adopted before, as a new session with
     the next `S` id, the same purpose and meta (so `--meta ticket=T-12`
     still finds the work) and `from`. Adopted before counts both ways
     work can arrive: straight from its branch, or by way of a branch that
     had adopted it (its own `from` names one adopted here); its commits
     then belong to that session, and its end is not appended twice;
   - each placed commit, with the next `L` id, its region at the placed
     start, its session's adopted id, and the same text, intent, behavior,
     `forced` and `user` — the message checks are not run again;
   - a branch `session_end`, for its adopted session;
   - a branch `amend`, with `of` naming the commit adopted here for the
     one it amends — found in one step through that commit's `from` (or,
     when the branch had itself adopted the commit, through the `from` of
     that) — and `from` naming the branch's amend record. An amendment of
     a commit not adopted here (a stopped file, a change already done
     here) stays in the branch and is counted as left; one already here,
     straight or by way of a branch between, is not appended again.
   Adopted records keep the branch's `ts`. They never change this folder's
   own active session: a `session_start` or `session_end` with `from` is
   history, not state. A session still open at the branch's head stays open
   among the adopted ones until a later merge carries its end, and that
   merge appends to the session already adopted. The shadow takes the
   placed commits; the working tree is never written (core rule 4).
5. **The merge record** closes the run: the branch, its head, the commits
   adopted and left, the files a conflict stopped with the first commit
   not adopted in each, and `already`: the hashes of the commits already
   done here (left out when there are none), so the history says they were
   seen, not lost. A later merge of the branch starts after `head` and
   keeps every stopped file stopped. A chain writes one merge record per
   branch with anything new, outer first, each with its own counts and
   stops: a branch the chain passed through advances to the base of the
   next, so its own later merge adopts only what came after — and a file
   stopped in an outer branch's part is stopped in the inner branches'
   records too.

**What is left.** The working tree holds what `git merge` made; the shadow
holds this history plus what was adopted. The difference — conflict
resolutions, stopped files, anything git did that lap could not place —
shows in `lap status` and is committed as usual, with a behavior that cites
the branch commits it stands for (`#9f3e21a`). When everything was adopted
and git merged cleanly, there is no difference.

`--dry-run` reports what would be adopted, what is already done and where
each file would stop, reading exactly what the merge would read (rule 2),
and writes nothing.

### Naming sessions

A branch and its parent both go on counting sessions from the base, and an
adopted session gets a new id in the parent, so a bare `S<n>` is ambiguous
once it leaves its folder — in a ticket comment, say. **`<branch>/S<n>`**
(the branch's name or id) names a session of that branch:

- Every command that takes a session (`lap rr <session>`, `lap log
  --session`, `lap search --session`) accepts it, reading that branch as
  `--branch` does — from any folder it descends from, so a branch of a
  branch is named the same way from main. A bare `S<n>` still means this
  folder's own.
- Once `lap merge` adopted that session into this folder, the name leads
  to the adopted session here, found by its `from` link: `lap rr
  feat/S4` in the parent reviews the adopted one, labelled `S9 (adopted
  from feat/S4)`, its commits showing their `from`. An old comment written
  in the branch still finds the work.
- lap prints sessions in that form wherever they are a branch's: in a
  branch folder (`session feat/S4 started`, `session current`, `session
  list`, log and search rows, `show`), and for another branch's history
  read with `--branch`. JSON carries it as `ref` (sessions) and
  `session_ref` (commits); `id` and `session` stay the bare ids.
- A name that is no branch here is `unknown_branch`; a session the branch
  does not have is `unknown_session` — including one from before its base,
  which its history holds but which is its parent's: `<branch>/S<n>` names
  only the branch's own sessions, after its branch record.
- A branch name names one branch for good: `lap branch start` refuses a
  name any branch here ever had (`name_taken`) — in the registry, among
  the branch chunks here, or in a merge record — so a name in an old
  ticket comment never comes to mean another branch.

### Reading another branch

`lap log`, `lap show` and `lap rr` take `--branch <name>` to read another
branch's history instead of this folder's: a registered branch from its
folder when that is reachable, else from its chunks here (a `git merge`
brought them, or `lap merge` copied them). In a branch folder, `--branch
main` reads the parent's history up to the base. Naming this folder's own
branch changes nothing; a name found nowhere is `unknown_branch`. It is a
reader's flag: `LAP_BRANCH` does not set it.

`lap show <hash>` without `--branch` also looks, when this history lacks
the hash, in every branch whose chunks are here, so an adopted commit's
`from` link opens its original. Commits in JSON output carry `branch`: the
branch whose chunk holds the record (`main`, or the branch's name); `lap
show` prints a `branch:` line for a commit that is not `main`'s. An
adopted commit carries its `from` in JSON, a `from:` line in `lap show`,
and `(from #<short hash>)` after its behavior in `lap rr`'s trajectory.

### Registry

The parent's `.lap/branches.json` lists the branches started from it:
`[{"id","name","path","base","started"}]`, the path absolute and canonical (symlinks resolved, one spelling however it was typed). It is
machine-local (not committed: a path means nothing elsewhere), neither
history nor a cache — nothing rebuilds it — and **hints only**: a missing
or malformed registry reads as no branches, and no command fails because
of what it says. The one error about a branch's whereabouts is `lap merge`
finding its history nowhere.

- **Checked, never trusted.** An entry's folder counts only while it
  exists and its `.lap/lineage` names that branch; a folder moved away, or
  reused for something else, is not the branch.
- **States.** `merged`: a merge adopted it up to its head, with no file
  stopped. `partly merged`: a merge stopped a file. `missing`: its folder
  is gone and it was not merged up to its head. `active`: anything else.
- **Merged and gone → dropped silently** by the next writing command: its
  history is in this folder's chunks, nothing is lost. Everything else
  stays — an unmerged or stopped branch whose folder is gone is shown as
  `missing` until `lap branch forget` drops it or `lap branch move` points
  it at the folder's new place.
- **Branches of branches.** A branch folder has its own registry, of the
  branches started from it. Readers here (`branch list`, `merge`,
  `--branch`, `<branch>/S<n>`) also read the registry of each listed
  branch whose folder is still that branch, and so on down; nothing is
  ever copied between registries. `lap branch list` shows a nested branch
  indented under the branch it started from, with `via` (that branch's
  id) in `--json`, and judges its state against that branch's history,
  where its merges are recorded. It is tended (`forget`, `move`) in that
  branch's folder.
- **Known by its chunks only.** A branch whose chunks are in `.lap/log/`
  but which no registry lists — brought by `git merge` from a clone that
  started it, or dropped once merged and gone — is listed too, after the
  registered ones, from its branch record: `registered: false`, no path,
  never `missing` (no folder of it is known here; one not merged is
  `active`), and `via` its parent when that is another branch listed here.
  Its sessions read with `--branch`, and the sessions adopted from it keep
  its name.

### Not in this version

- Bringing a parent's newer work into a branch (merging the other way):
  merge the branch into its parent, then start a fresh one.
- Creating or deleting folders, or running git: lap records; git and the
  user move code.
- Adopting across unrelated histories, or merging two branches with each
  other without their common parent.
- Retrying a stopped file (`lap merge --resume <file>`) once the parent has
  resolved the conflict: a stopped file stays stopped for its branch.

The chunk limit stays a constant (4 MB) so every folder chunks alike; it
is revisited only with evidence.

## Messages

A commit's text is two fields, each a non-empty string, multiline welcome,
trailing whitespace trimmed. The first line of each is its summary.

- **intent** — why the edit exists. Written as the goal, not the mechanics:
  "Route the graph place to its view."
- **behavior** — what this edit makes the code do: "Render Graph when the
  target place is graph; every other place still gets Collections." For a
  supporting edit, behavior says what it supports: "Import Graph for the
  graph route in #fa9cebd."

### Checks

`lap commit` refuses a message that fails any of these, with the error code
shown, before anything is written. Words are counted after lowercasing and
splitting on every character that is not a letter or a digit (bytes ≥ 0x80
count as letters, so non-ASCII words stay whole).

1. **Length** (`message_too_short`): intent and behavior each have at least
   3 words.
2. **Behavior is not the intent** (`behavior_repeats_intent`): the word-set
   Jaccard similarity of behavior and intent is below 0.8.
3. **Behavior is not the previous behavior** (`behavior_repeats_previous`):
   the Jaccard similarity of behavior and the behavior of the most recent
   commit in the same session is below 0.8. Skipped when there is none,
   and under `--no-session`.
4. **Behavior is not the code** (`behavior_restates_code`): the Jaccard
   similarity of behavior and the words of the edit's changed lines (its
   `new_text`, or its `old_text` for a pure deletion) is below 0.8.

Intent may repeat freely: it is shared by design.

`--force-message` skips checks 2–4, never check 1, for the rare honest
near-duplicate. The commit record then carries `"forced":true`, and `show`
and `rr` mark it, so a reviewer sees which messages bypassed the checks.

The thresholds are constants, chosen against this repository's own
history (§Unresolved).

### References

A commit's text refers to another commit by its hash, written as `#`
followed by 7 to 64 hex digits (`#fa9cebd`), in any case. lap does not
parse or validate references when committing: they are plain text, and the
words around them ("needs", "fixes", "reverts") are the author's. They are
for readers — an agent following the chain with `lap show`, or a UI that
turns each one into a link.

Wherever a command takes a commit, it accepts an id (`L42`), a hash, or a
hash prefix of at least 7 hex digits, with or without the `#`. A prefix
that matches more than one commit is refused with `ambiguous_ref`, listing
the matches; one that matches none is `unknown_ref`. Only commit records
are addressable by hash.

## Edit detection

- Diff runs between the shadow copy (last-committed state) and the working
  file: common prefix/suffix trim, then Myers O(ND) on line hashes with
  content verification.
- Effort cap: past 1024 edit steps the changed span collapses into a single
  region (a rewrite that large is one big change; the record marks nothing
  special, it is simply one region).
- Region merging: after detection, adjacent regions whose separating
  unchanged gap consists only of blank lines (empty, or spaces/tabs/`\r`)
  merge into one region spanning the gap. Rationale: diff alignment on
  content-free lines is coincidence, and it fragmented whole-block rewrites
  into artificial edit sequences.
- A change only in the trailing-newline state of the last line is a 1-line
  edit on that line.
- lap reads CRLF as LF, as git stores text: a working file's lines lose the
  `\r` of a CRLF ending before they are compared or recorded, so new
  commits hold LF, and a trailing `\r` on a shadow line is ignored when
  comparing. A checkout whose line endings git converted (Windows,
  `core.autocrlf`) therefore shows the same edits as one that was not, and
  a change of line endings alone is no change. lap does not read
  `.gitattributes`. The shadow is still exactly what the log replays to:
  lines recorded with a `\r` by an older lap keep it, and `verify --deep`
  compares bytes. A `\r` on a last line that no `\n` ends is content, not a
  line ending.
- Files with a NUL byte in the first 8 KB are binary: shown in status once
  tracked, refused by commit.
- Files larger than 64 MB are refused. Symlinks are skipped entirely.

## Commands

Every command accepts `--json` for machine-readable output on stdout.
Exit codes: `0` success, `1` user/repo error, `2` internal failure.
JSON errors are `{"ok":false,"error":"<code>","message":"..."}`.

Every JSON object that describes a commit carries its full `hash`, and every
human listing of commits prints the short hash beside the id
(`L42 fa9cebd`).

Argument parsing: flags that take values consume the next word entirely, so
a message like `-i "--no-session"` is never misread as a flag; a literal
`--` ends flag parsing, letting file names that start with `-` be committed
(`lap commit -i "..." -b "..." -- -weird.txt`). A word before `--` that
starts with `-` and is not one of the command's flags (or lap's own colour
flags) is refused with `unknown_flag`, naming it; nothing is run. A
skipped flag would let its value pass for an argument — `-x "text" f.c`
would name a file `text`.

### Colour

`--color=auto|always|never` and `--no-color` are lap's own flags rather
than any command's, and may appear on either side of the command name; the
last one given wins. They are spelled with `=` so that the mode is part of
the flag's own word and can never be mistaken for a positional argument;
every command's parser accepts them. The scan that finds them applies the
same rule every command does: **a value-taking flag's value is data**, so
`lap commit -i "--color=always"` records that intent and changes nothing
about the display.

Colour is bound by one contract: **it never changes what the output says,
only how it looks.** Concretely —

- `auto` (the default) styles a stream only when that stream is an
  interactive terminal. stdout and stderr are decided independently, so
  `lap log | less` still shows a human a red `error:`.
- A non-empty `NO_COLOR` disables colour unconditionally, overriding even
  `--color=always`: it is the environment's kill switch, and honouring it
  is what lets a caller guarantee escape-free output.
- `TERM=dumb` disables colour in `auto`.
- `--json` output is never styled, in any mode. This is enforced where the
  mode is resolved rather than in each command's JSON branch, so a styled
  helper reached from a JSON path emits nothing instead of depending on
  every branch to remember.
- Removing the escape sequences from styled output yields the unstyled
  output byte for byte. Nothing is padded, truncated, or re-ordered for
  colour, and every aligned column is padded by visible width rather than
  by byte count.

### `lap init`
Creates `.lap/` in the cwd plus a starter `.lapignore` (kept if present).

### `lap status`
Active session, then every file with pending changes: `new` (line count),
`modified` (numbered edit list with line ranges), `deleted`, or `binary`.

### `lap commit <file> (-i "intent" -b "behavior" | -F <file|->) [--edit N | --lines A-B] [--force-message] [--no-session] [--dry-run] [--branch B]`
Records exactly one edit. The message comes from `-i`/`--intent` and
`-b`/`--behavior` together, or from `-F`, never a mix:

- `-F <file>` reads both fields from a file (`-F -` reads stdin — the
  reliable path for long, multiline text that would fight shell quoting).
  The file holds two sections, each opened by a line that is exactly
  `Intent:` or `Behavior:` (any order, each once); a section's text is the
  lines up to the next header or the end. Text before the first header, a
  missing or repeated section, or an empty one is refused with
  `bad_message_file`.
- A missing field is `missing_intent` or `missing_behavior`.
- The message then passes the checks in §Messages, or the commit is
  refused.

Then, by the number of pending edits in the file:
- 0 → error `no_changes`.
- 1 → committed.
- \>1 → error `multiple_edits` listing numbered regions; retry
  with `--edit N` (pick from the list) or `--lines A-B` (must exactly match
  one region's range as shown by `lap status`: current-file lines, or
  last-committed lines for pure deletions). Remaining edits stay pending and
  are re-detected (with fresh coordinates) on the next run.
- New file → `create` (whole content, one edit). Deleted file → `delete`.

On success it prints the new commit's id and short hash (`L42 fa9cebd`);
`--json` returns the id and the full `hash`. This is how an agent learns
the hash to cite in its next commit.

`--dry-run` does all of the above except write: it resolves the file and
the edit, runs the message checks, and prints what would be recorded — id,
session, op, file, region, intent and behavior — failing exactly as the
commit would. `--json` returns `{"ok":true,"dry_run":true,"record":{…}}`,
the record as it would be written without `prev` (and so without a hash,
which neither exists until it is written). A dry run is a reader: it takes
no lock and repairs nothing (no torn-tail cut, no state rewrite), and its
predicted id is the one a commit made next would get.

### `lap amend <commit> (-i "intent" -b "behavior" | -F <file|->) [--force-message] [--branch B]`
Corrects what a commit says (§The log → Amendments): appends an `amend`
record, and changes nothing already written. The code is never touched —
a wrong edit is fixed by a new commit. `<commit>` is an id, a hash or a
hash prefix (§References). The message is given as for `lap commit`: both
fields, always (repeat one to keep it), from `-i`/`-b` or `-F`, and it
passes the same checks (§Messages) — `behavior_repeats_previous` against
the commit before it in its session, `behavior_restates_code` against the
commit's own changed lines. `--branch` (or `LAP_BRANCH`) is required where
branches exist, as for commits. Needs no session.

Only commits of **this folder's own line of history** can be amended: in
a branch folder, a commit from before its base is its parent's
(`not_own_commit`: amend it there). Also refused: `not_a_commit` (the
reference names another record), `same_message` (the commit already says
that), `unknown_ref`/`ambiguous_ref`. Prints `[L42 fa9cebd] amended (<n>)`
and the new intent's first line; `--json` returns the commit's `id`,
`hash` and `amended` (the count).

### `lap log [--session S] [--file F] [-n N] [--branch B]`
Commits newest-first: id, short hash, timestamp, session, op, file, range,
intent summary.

### `lap show <commit> [--full-file] [--branch B]`
Full record: metadata with the full hash, the complete intent and behavior,
`forced` when set, for an amended commit how often and last when and by
whom with its earlier texts after the behavior, and a unified-diff-style
hunk.
`<commit>` is an id, a hash or a hash prefix (§References).
`--full-file` additionally reconstructs the whole file as of that commit by
replaying its history.

### `lap search ...`
Asks the history questions; criteria AND together:
- `--file F --line N` — which commit introduced/last touched *current* line
  N (blame). Pending lines are reported as pending. Line numbers are traced
  back through both pending edits and every commit's line shifts.
- `--text STR [--added|--removed]` — commits whose changed lines contain STR.
- `--msg STR` — substring of the intent or the behavior.
- `--session S`, `--since TS`, `--until TS`, `--limit N`.

### `lap session [start "purpose" | end | list | current] [--meta key=value]...`
One active session at a time; `start` requires a purpose, given as its
argument or read with `-F <file|->` (the whole text); a crashed
session simply stays open. `list` shows every session with commit counts.

`start` takes `--branch` where branches exist (§Branches → Committing: say
where).

`--meta key=value` (repeatable) on `start` tags the session with metadata,
stored as a flat object on the `session_start` record — always present, `{}`
when there is none. Keys are identifiers (`[A-Za-z_][A-Za-z0-9_]*`); a key
given twice is refused. A value is written as a JSON number when it is one
(`n=1` → `"n":1`), as `true`/`false` when it is one of those, and as a string
otherwise (`ticket=T-12` → `"ticket":"T-12"`). `list --meta key=value` keeps only the sessions
carrying every given pair, and `list --json` / `current --json` always carry
a `meta` object. This is how other tools link work to a session: coboard
starts sessions with `--meta ticket=T-12` and finds a ticket's sessions with
`lap session list --meta ticket=T-12`.

`list --json` gives each session its `hash` (its `session_start` record's)
and, for one `lap merge` adopted, its `from`. `list --branch <name>` lists
another branch's sessions (§Branches → Reading another branch); there a
session is active when it is still open. Together they let a tool follow
a ticket's work into its branches and see each piece once: a branch
session whose hash is an adopted session's `from` was adopted.

### `lap rr [<session>] [<from> <to>] [--no-diff] [--branch B]`
A **review request**: what a run of work changed, and why. Two halves —
the *trajectory* (every commit in the order the work happened) and the
*net change* (each touched file replayed to just before the range and
again at its end, then diffed). Edits that cancelled out show as no net
change; ten commits to one function show as one coherent change.

In the human trajectory, consecutive commits with the same intent print
under one intent heading, each followed by its own behavior. The JSON
trajectory stays one entry per commit, in order, and leaves grouping to
the reader.

The target is a session, an inclusive commit range (`<from>` and `<to>`
are commits, §References), or — with no argument — the most recent
session. Targets are given only as arguments; there are no `--session`,
`--from` or `--to` flags. `--no-diff` keeps the per-file summary and drops
the hunks, in both the human and JSON shapes. Read-only.

### `lap verify [--deep]`
Walks the hash chain across every chunk, naming a modified sealed chunk
(§Chunks), then the chain of every other branch whose chunks are here, one
line each (`branch b: chain ok: 8 records`; in JSON, `branches:
[{branch, records, chain_ok, chain_error?}]`). A broken one fails the
command. `--deep` also replays every file's history from
birth and compares the result byte-for-byte with the shadow store and
every snapshot. Verification never uses the caches it is checking.

### `lap branch start [name] --from <folder>`
Makes the current folder a branch of `<folder>` (§Branches → Starting one),
which may itself be a branch folder. Prints the name, the id, the parent
and the base's short hash; `--json` returns `id`, `name`, `parent`,
`base`, `base_chunk`. Errors: `missing_from`, `bad_name`, `same_folder`,
`already_branch`, `no_parent`, `unrelated_history`, `not_clean`,
`name_taken`, `parent_read_only`. `same_folder` is the folder itself under
any spelling — another case, a symlink, a `..` path — told by the
directory on disk (device and inode; volume and file id on Windows), not by
the path's text.

### `lap branch list`, `lap branch forget <branch>`, `lap branch move <branch> <path>`
`list` shows, in a branch folder, its own name, id, parent and base, then
each branch this folder started (§Registry), and under each the branches
started from it, indented: its name, state and last known path (`(gone)`
when its folder is not there), its commits since its base and since the
last merge, the files a merge stopped, and for a `missing` one the two
fixes. `--json`: `{"ok":true,"self":{id, name, parent, base} |
null,"branches":[{id, name, state, present, path, registered, base, started,
since_base, since_merge, merged, stopped, stops, via}]}`, `registered`
false (and `path` empty) for a branch known here only by its chunks, the counts
`null` when the branch's history is nowhere to be read, `merged` the last
merged head or `null`, `stopped` the stopped files, `stops` the same with
the first commit not adopted in each (`[{file, at}]`), and `via` the id of
the branch a nested one started from (`null` for this folder's own).

`forget` drops an entry; `move` points it at a folder that holds that
branch (else `not_that_branch`). Both are writers; an entry not in the
registry is `unknown_branch`.

### `lap merge <branch> [--dry-run] [--copy-from-folder]`
Adopts a branch's history into this folder's (§Branches → Merging).
`--copy-from-folder` takes a git checkout's missing chunks from its folder
instead of refusing with `git_merge_first`. Prints
what was adopted of how many commits, with the new ids, then each stopped
file with the first commit not adopted and why, then each commit already
done here, then how many of the branch's amendments were carried and how
many stayed in the branch (a dry run does not count them); `nothing new to
adopt` when the branch has nothing after the
last merge. `--json` returns `dry_run`, `branch`, `name`, `new`, `adopted`,
`left`, `head`, `stopped` (`[{file, at, why}]`), `already` (hashes),
`amendments` (`{carried, left}`) and
`commits` (`[{id, from}]`). Errors:
`branch_not_found`, `merge_in_branch`, `unrelated_history`, `log_broken`,
`git_merge_first`.

### `lap rebuild [--verify]`
Deletes and reconstructs every derived cache from the log — the executable
proof of the cache contract. `--verify` additionally fails when the hash
chain is broken. Run it after transporting a bare log, deleting caches, or
upgrading across a cache-format change.

## `.lapignore`

gitignore subset: `#` comments; trailing `/` = directories only; a pattern
containing `/` (or starting with `/`) is anchored to the root; otherwise it
matches basenames at any depth; `*`, `?` within a segment; `**` spans
segments. Negation (`!`) is not supported. Always ignored: `.lap/`, `.git`
(a directory, or the file a git worktree has in its place), `.hg/`,
`.svn/`, `.DS_Store`.

## Concurrency & crash safety

- One exclusive lock (`.lap/lock`) serializes writers; readers never lock
  and never write.
- **Torn tail**: a crash mid-append leaves an unterminated final line in the
  open chunk. Readers drop it (it was never acknowledged) and continue; `lap
  verify` notes it; the next writing command truncates it away under the
  lock before appending.
- Write order: log append (fsync) → shadow update → state write (atomic,
  per-process temp name). A crash at any point leaves `state.json` behind
  the log tail; the next **writer** detects the mismatch and heals —
  rebuilding counters, the active session, *and the entire shadow tree*
  from a full replay. Readers that hit the mismatch heal in memory only.
- **Damage is not a crash.** A torn line anywhere but the open chunk's end
  was not made by a crash (lap repairs its own before sealing or merging):
  a sealed chunk cut or copied short. Every command refuses such a
  history, naming the chunk. Writers also refuse a chain broken between
  chunks (a stray chunk, one truncated at a line, a wrong `prev`): lap
  never builds on a history it cannot vouch for. Both checks read a few
  bytes per chunk; `lap verify` reads everything.
- **An interrupted `lap merge`** (a crash, a full disk) leaves some adopted
  records and no merge record; everything else heals as above. Running the
  same merge again finds those records by their `from` links: when they
  are the last thing recorded here, it places the branch's commits against
  this folder's history from before them and appends only what they lack,
  so the result is the uninterrupted merge's, ids included. When other
  work was recorded since, their commits come out already done (§Merging,
  rule 3) and the rest is adopted.
- The log itself has no size cap and its tail is hashed through a bounded
  tail-window read, so growth never makes the repo unreadable. (User files
  keep the 64 MB cap.)
- `lap verify --deep` checks both directions: every shadow file must match
  its replay, and every file the log says exists must have a shadow.

## Portability

C11, zero dependencies. POSIX (macOS/Linux) is the tested platform; the
`_WIN32` branches (paths, locking, directory walking) are best-effort and
currently untested. All internal paths use `/` separators.

## Unresolved

- **Thresholds.** 3 words and 0.8 are starting values. Before they are
  fixed, run the checks over the messages this repository recorded before
  commits had an intent and a behavior (each `msg` taken as a behavior,
  paired with its session neighbour and its hunk) and look at what they
  would have refused. Those messages stay in git history, so this can run
  from `git show <rev>:.lap/log.jsonl`.
