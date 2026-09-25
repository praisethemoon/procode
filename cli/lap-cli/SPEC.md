# lap — specification (v0.1)

`lap` is a CLI that records **fine-grained, human-interpretable edit history**
for AI agents. It sits below git: agents record every small edit with a
reason; git keeps its normal human-scale history.

## Core rules

1. **A commit is one edit of one file.** An *edit* is one contiguous run of
   changed lines (insertions, deletions, replacements). Two changed areas
   separated by at least one **non-blank** unchanged line are two edits and
   must be committed separately. Blank (whitespace-only) lines carry no
   identity, so a gap made only of them never splits an edit: the changed
   runs merge into one region that spans the gap, blank lines included.
2. **Every commit has a message** — a single non-empty string, multiline
   welcome, given with `-m "text"` or `-F <file>` (`-F -` reads stdin;
   trailing whitespace is trimmed). The first line is used as the summary in
   listings. Messages explain *why* the edit exists.
3. **Commits belong to sessions.** A session is a named group of commits with
   a purpose ("fix the parser bug"). Committing requires an active session
   unless `--no-session` is passed explicitly.
4. **lap never writes to tracked files.** The working tree is authoritative;
   history is a recording. There is no checkout, branch, merge, or staging.
5. **Commit ids are short and sequential** (`L1`, `L2`, ...; sessions `S1`,
   `S2`, ...). Integrity comes from a hash chain, not content-addressed ids.

## Repository layout

```
<root>/
  .lapignore        ignore patterns (tracked like any file)
  .lap/
    log.jsonl       TRUTH — append-only record log, hash-chained
    state.json      cache: counters, active session, last chain hash
    shadow/         cache: last-committed content of every tracked file
    index           cache: one fixed-width entry per record (§acceleration)
    paths           cache: path table; file_id = line number
    heads           cache: per-file chain tail + snapshot byte budget
    snapshots/      cache: periodic full-content snapshots, per file
    lock            exclusive lock file taken by writing commands
```

- Commands find the repository by walking upward from the cwd (like git).
- **The cache contract:** everything in `.lap/` except `log.jsonl` is a
  derived, disposable cache. Any of it may be deleted at any time; the next
  writing command (or `lap rebuild`) reconstructs it from the log. Cache
  formats may change between versions with no migrations — an unrecognized
  or stale cache is rebuilt. Caches are native-endian and single-machine:
  transport a repo as its log (plus working tree) and rebuild on arrival.
- Writing commands hold the lock for their whole run and keep every cache
  in step; readers never lock and never write. A reader that finds a cache
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
  record — byte offset/length into the log, kind, op, numeric session,
  `file_id`, epoch timestamp, the region coordinates, a running commit
  counter (ids are dense integers, so finding `L<n>` is a binary search and
  per-record access is a seek), and `prev_same_file`: the entry index of
  the file's previous commit. Blame, per-file log, and replay walk that
  chain and never touch unrelated records.
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
- `lap verify --deep` audits the layer: every shadow and every snapshot
  must equal a from-birth replay of the truth.
- History is **linear by construction**: there are no branches, refs, or
  merges; concurrency is serialized by the writer lock and expressed as
  interleaved sessions.

## The log

One JSON object per line. Field order is fixed at encode time. Every record
carries `prev`: the SHA-256 (lowercase hex) of the previous record's exact
bytes; the first record chains from 64 zeros. `lap verify` walks the chain.

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
 "msg":"...","ts":"...","prev":"..."}

{"type":"session_start","id":"S2","user":"jane","msg":"purpose","ts":"...","prev":"..."}
{"type":"session_end","id":"S2","ts":"...","prev":"..."}
```

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

Timestamps are UTC ISO-8601 (`2026-09-20T12:34:56Z`); since they are
lexicographically ordered, `--since`/`--until` compare as strings.

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
- CRLF is preserved byte-faithfully (the `\r` stays in the line content).
- Files with a NUL byte in the first 8 KB are binary: shown in status once
  tracked, refused by commit.
- Files larger than 64 MB are refused. Symlinks are skipped entirely.

## Commands

Every command accepts `--json` for machine-readable output on stdout.
Exit codes: `0` success, `1` user/repo error, `2` internal failure.
JSON errors are `{"ok":false,"error":"<code>","message":"..."}`.

Argument parsing: flags that take values consume the next word entirely, so
a message like `-m "--no-session"` is never misread as a flag; a literal
`--` ends flag parsing, letting file names that start with `-` be committed
(`lap commit -m "msg" -- -weird.txt`).

### Colour

`--color=auto|always|never` and `--no-color` are lap's own flags rather
than any command's, and may appear on either side of the command name; the
last one given wins. They are spelled with `=` so that a command's own
parser, which skips anything starting with `-`, cannot mistake the mode for
a positional argument. The scan that finds them applies the same rule every
command does: **a value-taking flag's value is data**, so
`lap commit -m "--color=always"` records that message and changes nothing
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

### `lap commit <file> -m "msg" | -F <file|-> [--edit N | --lines A-B] [--no-session]`
Records exactly one edit (`-m` and `-F` are mutually exclusive; `-F -`
reads the message from stdin — the reliable path for long, multiline
rationales that would fight shell quoting):
- 0 pending edits → error `no_changes`.
- 1 pending edit → committed.
- \>1 pending edits → error `multiple_edits` listing numbered regions; retry
  with `--edit N` (pick from the list) or `--lines A-B` (must exactly match
  one region's range as shown by `lap status`: current-file lines, or
  last-committed lines for pure deletions). Remaining edits stay pending and
  are re-detected (with fresh coordinates) on the next run.
- New file → `create` (whole content, one edit). Deleted file → `delete`.

### `lap log [--session S] [--file F] [-n N]`
Commits newest-first: id, timestamp, session, op, file, range, message
summary.

### `lap show <id> [--full-file]`
Full record: metadata, complete message, unified-diff-style hunk.
`--full-file` additionally reconstructs the whole file as of that commit by
replaying its history.

### `lap search ...`
Asks the history questions; criteria AND together:
- `--file F --line N` — which commit introduced/last touched *current* line
  N (blame). Pending lines are reported as pending. Line numbers are traced
  back through both pending edits and every commit's line shifts.
- `--text STR [--added|--removed]` — commits whose changed lines contain STR.
- `--msg STR` — message substring.
- `--session S`, `--since TS`, `--until TS`, `--limit N`.

### `lap session [start "purpose" | end | list | current]`
One active session at a time; `start` requires a purpose message; a crashed
session simply stays open. `list` shows every session with commit counts.

### `lap rr [<session>] [<from> <to>] [--no-diff]`
A **review request**: what a run of work changed, and why. Two halves —
the *trajectory* (every commit's message in the order the work happened)
and the *net change* (each touched file replayed to just before the range
and again at its end, then diffed). Edits that cancelled out show as no
net change; ten commits to one function show as one coherent change.

The target is a session, an inclusive commit range, or — with no argument
— the most recent session. `--no-diff` keeps the per-file summary and
drops the hunks, in both the human and JSON shapes. Read-only.

### `lap verify [--deep]`
Walks the hash chain. `--deep` also replays every file's history from
birth and compares the result byte-for-byte with the shadow store and
every snapshot. Verification never uses the caches it is checking.

### `lap rebuild [--verify]`
Deletes and reconstructs every derived cache from the log — the executable
proof of the cache contract. `--verify` additionally fails when the hash
chain is broken. Run it after transporting a bare log, deleting caches, or
upgrading across a cache-format change.

## `.lapignore`

gitignore subset: `#` comments; trailing `/` = directories only; a pattern
containing `/` (or starting with `/`) is anchored to the root; otherwise it
matches basenames at any depth; `*`, `?` within a segment; `**` spans
segments. Negation (`!`) is not supported. Always ignored: `.lap/`, `.git/`,
`.hg/`, `.svn/`, `.DS_Store`.

## Concurrency & crash safety

- One exclusive lock (`.lap/lock`) serializes writers; readers never lock
  and never write.
- **Torn tail**: a crash mid-append leaves an unterminated final log line.
  Readers drop it (it was never acknowledged) and continue; `lap verify`
  notes it; the next writing command truncates it away under the lock
  before appending.
- Write order: log append (fsync) → shadow update → state write (atomic,
  per-process temp name). A crash at any point leaves `state.json` behind
  the log tail; the next **writer** detects the mismatch and heals —
  rebuilding counters, the active session, *and the entire shadow tree*
  from a full replay. Readers that hit the mismatch heal in memory only.
- The log itself has no size cap and its tail is hashed through a bounded
  tail-window read, so growth never makes the repo unreadable. (User files
  keep the 64 MB cap.)
- `lap verify --deep` checks both directions: every shadow file must match
  its replay, and every file the log says exists must have a shadow.

## Portability

C11, zero dependencies. POSIX (macOS/Linux) is the tested platform; the
`_WIN32` branches (paths, locking, directory walking) are best-effort and
currently untested. All internal paths use `/` separators.
