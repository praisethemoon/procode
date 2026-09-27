<img src="../../media/lap.svg" alt="lap" width="96" align="right">

# lap

**A flight recorder for AI agent work sessions.** lap records code changes
the way agents actually make them — one small edit at a time, each with its
intent and what it does — below git, without touching git.

```
$ lap session start "add retry logic to the fetcher"
session S4 started: add retry logic to the fetcher

$ lap commit src/fetch.c -i "survive the flaky staging DNS, which drops ~2% of lookups" \
    -b "adds retry(): three attempts with exponential backoff"
[L23 fa9cebd] S4 src/fetch.c: lines 10-24 (insertion)  "survive the flaky staging DNS, which drops ~2% of lookups"

$ lap search --file src/fetch.c --line 12
line 12 of src/fetch.c was last touched by L23 fa9cebd (2026-09-20T12:31:07Z)
```

Where git answers *what changed between commits*, lap answers *why does this
line exist* and *what was the agent doing when it appeared* — at the
granularity agents work at.

## The rules

1. **One commit = one edit.** A contiguous run of changed lines in one file.
   Two edits separated by a non-blank unchanged line are two commits —
   `lap commit` enforces this and walks you through committing them one at
   a time. Blank lines are not anchors: a gap of only blank lines never
   splits an edit.
2. **Every commit explains itself, twice.** Its **intent** (`-i`) says why
   the edit exists — edits serving one goal share it — and its
   **behavior** (`-b`) says what this edit makes the code do. Both are
   required (`-F <file>` / `-F -` reads them from `Intent:` and
   `Behavior:` sections when they are long), and lap refuses a message that
   is too short or only repeats the intent, the previous behavior or the
   code. A commit cites another by its hash: `#fa9cebd`.
3. **Work happens in sessions.** `lap session start "purpose"` groups the
   commits of one task. Committing without a session requires an explicit
   `--no-session`.
4. **lap never writes to your files.** It records; it does not time-travel.
   No branches, no checkout, no merge, no staging area.

## Build

Zero third-party dependencies — a C11 compiler is all you need. No
libraries to fetch, no build-time code generation, no network access.

```sh
make                          # -> bin/lap
make test                     # unit tests + end-to-end suite
make unit                     # unit tests alone
make e2e                      # end-to-end suite alone (needs sh)
sudo make install             # -> /usr/local/bin/lap
sudo make uninstall
```

`PREFIX=/opt make install` installs elsewhere; `DESTDIR=` is honoured for
packaging. `CC` and `CFLAGS` override the compiler and flags, which is how
the sanitizer build runs:

```sh
make CFLAGS="-std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined" test
```

Or with CMake — the cross-platform path, and the one to use on Windows with
MSVC or MinGW:

```sh
cmake -B build && cmake --build build
ctest --test-dir build        # unit (+ e2e on POSIX)
cmake --install build         # CMAKE_INSTALL_PREFIX to change the target
cmake --build build --target uninstall
```

POSIX (macOS/Linux) is the tested platform; see [Limitations](#limitations).

## Use

Record work as you do it:

```sh
lap init                                    # creates .lap/ and a starter .lapignore
lap session start "add retry handling"      # name the task before you begin
# ... edit src/fetch.c ...
lap commit src/fetch.c -i "retry on 429: staging returns it under load" \
    -b "calls retry() when send() answers 429"
lap session end
```

The cadence that matters is **one edit, one commit, immediately** — not a
batch at the end. If you changed two separate places, lap refuses and shows
you how to split them:

```
$ lap commit src/fetch.c -i "..." -b "..."
error: 2 separate edits detected in src/fetch.c
  [1] lines 10-12
  [2] lines 48-49
a commit is one edit; pick one:
  lap commit src/fetch.c -i "..." -b "..." --edit <n>
  lap commit src/fetch.c -i "..." -b "..." --lines <start>-<end>
```

Commit them one at a time: the same intent if they serve one goal, each
with its own behavior. After each commit the remaining edits are
re-detected and **renumbered**, so read the fresh listing (or `lap status`)
rather than reusing old numbers.

Long messages avoid shell-quoting pain through a file or stdin, as two
sections:

```sh
printf 'Intent:\nretry on 429...\n\nBehavior:\ncalls retry() when...\n' | lap commit src/fetch.c -F -
```

Then interrogate the history:

```sh
lap status                              # what is pending, per file, numbered
lap search --file src/fetch.c --line 12 # why does this line exist?
lap search --text retry_with_backoff    # when did this string appear or vanish?
lap search --msg "429"                  # which intents/behaviors mention this?
lap session list                        # what tasks happened
lap log --session S4                    # what was done in one of them
lap show L23 --full-file                # one commit, plus the file as of it
lap show fa9cebd                        # the same commit, by hash or prefix
lap rr S4                               # the whole task: why, then what
lap verify                              # is the history intact?
```

Deleting a file is a commit too: `rm` it, then `lap commit <path> -i "why" -b "what is gone"`.

### Commands

| command | purpose |
|---|---|
| `lap init` | create a repository in the current directory |
| `lap status` | pending edits per file, numbered |
| `lap commit <file> -i "intent" -b "behavior"` | record one edit and print its id and short hash (`-F` for both from a file, `--edit N` / `--lines A-B` to pick among several, `--force-message` to skip the repetition checks, `--no-session` to bypass sessions) |
| `lap log` | commits, newest first (`--session`, `--file`, `-n`) |
| `lap show <commit>` | one commit in full, by id, hash or hash prefix (`--full-file` reconstructs the file) |
| `lap search` | blame a line (`--file F --line N`), find text (`--text`), intents and behaviors (`--msg`), sessions, time ranges |
| `lap session` | `start "purpose"` (or `-F <file>`) / `end` / `list` / `current` |
| `lap rr` | review request: a run of work as trajectory, grouped by intent, + net change (`<session>`, `<from> <to>` as ids or hashes, `--no-diff`) |
| `lap verify` | check the log's hash chain (`--deep`: replay everything, audit caches) |
| `lap rebuild` | reconstruct every cache from the log (`--verify`: fail on a broken chain) |

Every command takes `--json` for machine-readable output. Exit codes: `0`
success, `1` user or repository error, `2` internal failure.

`LAP_USER` sets the committer identity (agents should set it, e.g.
`LAP_USER="claude/bugfix-agent"`); otherwise lap falls back to
`git config user.name`, then the OS user.

## Colour

Output is coloured only when it is going to a terminal. A pipe, a file, or
`--json` gets the same bytes it always did — no escape sequence ever
reaches a parser or an agent's context. `--color=always` forces it on
(useful for `less -R`), `--color=never`, `--no-color` and a non-empty
`NO_COLOR` force it off, and the two modes differ only by the escapes:
colour never moves a column.

## How it stores things

An append-only JSONL log in `.lap/log.jsonl` — every record hash-chained to
the previous one (tamper- and corruption-evident, checked by `lap verify`)
— is the sole truth. Everything else in `.lap/` is a disposable
acceleration cache: a fixed-width index with per-file chains (O(1) commit
lookup, blame that walks only the file's own history), byte-budgeted
content snapshots that bound replay cost, and the shadow store. Delete any
of it, `lap rebuild` restores it; reads stay correct (slower) even without
it. `.lapignore` (gitignore-like subset) controls what is tracked. Details
in [SPEC.md](SPEC.md).

## Limitations

Worth knowing before you rely on it.

**lap cannot restore anything.** It records; it never writes to your files.
There is no checkout, revert, branch, merge, staging area, or remote. To
get old content back you read it out of `lap show --full-file` yourself.

**lap does not watch git.** It compares the working tree against its own
last-committed copy and nothing else. So when git changes files underneath
it — `checkout`, `pull`, `rebase`, `merge` — those changes are
indistinguishable from edits you made, and `lap status` will report them as
your pending work. Commit or discard before switching branches.

**A shared history file can be corrupted by merging it.** If
`.lap/log.jsonl` is tracked by git and two branches both record commits,
merging them conflicts on a single append-only file, and resolving that by
interleaving lines breaks the hash chain. `lap verify` will detect the
damage but cannot repair it. Either keep recorded history on one line of
work, or add `.lap/` to `.gitignore` and let it stay machine-local.

**Renames are two commits**, because lap tracks paths, not file identity —
record the old path's disappearance and the new path's appearance, and name
the other path in each behavior. Blame and `lap log --file` cannot follow a
rename.

**Text files only.** A NUL byte in the first 8 KB marks a file binary and
lap refuses to track it. Files larger than 64 MB are refused outright.

**One commit is one contiguous run of lines.** A single logical change
scattered across a file becomes several commits — by design, but it does
mean the unit of history is mechanical rather than semantic.

**`lap status` reads every tracked file.** There is no stat cache, so it is
linear in the size of the tree: roughly 0.6 s on a 7,450-file checkout
against git's 0.02 s. Committing is unaffected.

**One writer at a time.** An exclusive lock serializes writing commands for
their whole run; readers never lock and never block. A second writer waits
rather than failing.

**Caches are machine-local** and native-endian. Transport a repository as
its `log.jsonl` plus the working tree, then run `lap rebuild` on arrival.

**Windows is best-effort and untested.** The `_WIN32` branches exist and
compile via CMake, but POSIX is the only platform covered by the suites.

## For agents

`skill/lap/SKILL.md` is a drop-in skill for Claude Code (copy it to your
project's `.claude/skills/lap/`) that teaches the cadence: start a session,
edit → commit → edit → commit, end the session, and how to recover when a
commit is rejected for containing more than one edit.

## License

MIT — see [../../LICENSE](../../LICENSE).
