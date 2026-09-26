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
   history is a recording. There is no checkout, branch, merge, or staging.
5. **Commit ids are short and sequential** (`L1`, `L2`, ...; sessions `S1`,
   `S2`, ...). Integrity comes from a hash chain, not content-addressed ids.
   Every commit also has a **hash** (§The log), which is how one commit's
   text refers to another.
6. **lap is for one developer's working copy.** There are no branches, and
   logs are never merged. When work arrives from elsewhere (a merged pull
   request), it lands in the working tree like any other change and is
   committed edit by edit.

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
```

A commit record without both `intent` and `behavior` is malformed,
including one that carries a single `msg` in their place.

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

Timestamps are UTC ISO-8601 (`2026-09-20T12:34:56Z`); since they are
lexicographically ordered, `--since`/`--until` compare as strings.

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
- CRLF is preserved byte-faithfully (the `\r` stays in the line content).
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

### `lap commit <file> (-i "intent" -b "behavior" | -F <file|->) [--edit N | --lines A-B] [--force-message] [--no-session]`
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

### `lap log [--session S] [--file F] [-n N]`
Commits newest-first: id, short hash, timestamp, session, op, file, range,
intent summary.

### `lap show <commit> [--full-file]`
Full record: metadata with the full hash, the complete intent and behavior,
`forced` when set, and a unified-diff-style hunk.
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

### `lap rr [<session>] [<from> <to>] [--no-diff]`
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

## Unresolved

- **Thresholds.** 3 words and 0.8 are starting values. Before they are
  fixed, run the checks over the messages this repository recorded before
  commits had an intent and a behavior (each `msg` taken as a behavior,
  paired with its session neighbour and its hunk) and look at what they
  would have refused. Those messages stay in git history, so this can run
  from `git show <rev>:.lap/log.jsonl`.
