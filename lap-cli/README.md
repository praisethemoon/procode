# lap

**A flight recorder for AI agent work sessions.** lap records code changes
the way agents actually make them — one small edit at a time, each with a
stated reason — below git, without touching git.

```
$ lap session start "add retry logic to the fetcher"
session S4 started: add retry logic to the fetcher

$ lap commit src/fetch.c -m "retry helper: 3 attempts with backoff,
because the flaky staging DNS drops ~2% of lookups"
[L23] S4 src/fetch.c: lines 10-24 (insertion)

$ lap search --file src/fetch.c --line 12
line 12 of src/fetch.c was last touched by L23 (2026-09-20T12:31:07Z)
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
2. **Every commit explains itself.** The message is mandatory (`-m "..."`,
   or `-F <file>` / `-F -` for stdin when it is long) and should say why,
   not what (the diff already says what).
3. **Work happens in sessions.** `lap session start "purpose"` groups the
   commits of one task. Committing without a session requires an explicit
   `--no-session`.
4. **lap never writes to your files.** It records; it does not time-travel.
   No branches, no checkout, no merge, no staging area.

## Build

Zero dependencies — a C11 compiler is all you need.

```sh
make                          # -> bin/lap
make test                     # unit tests + end-to-end suite
sudo make install             # -> /usr/local/bin/lap  (PREFIX=... to change)
sudo make uninstall
```

Or with CMake (the cross-platform path — use this on Windows with MSVC or
MinGW, where the `_WIN32` branches are best-effort and untested):

```sh
cmake -B build && cmake --build build
ctest --test-dir build        # unit (+ e2e on POSIX)
cmake --install build         # CMAKE_INSTALL_PREFIX to change the target
cmake --build build --target uninstall
```

POSIX (macOS/Linux) is the tested platform.

## Commands

| command | purpose |
|---|---|
| `lap init` | create a repository in the current directory |
| `lap status` | pending edits per file, numbered |
| `lap commit <file> -m "msg"` | record one edit (`--edit N` / `--lines A-B` to pick among several, `--no-session` to bypass sessions) |
| `lap log` | commits, newest first (`--session`, `--file`, `-n`) |
| `lap show <id>` | one commit in full (`--full-file` reconstructs the file) |
| `lap search` | blame a line (`--file F --line N`), find text (`--text`), messages (`--msg`), sessions, time ranges |
| `lap session` | `start "purpose"` / `end` / `list` / `current` |
| `lap verify` | check the log's hash chain (`--deep`: replay everything, audit caches) |
| `lap rebuild` | reconstruct every cache from the log (`--verify`: fail on a broken chain) |

Every command takes `--json` for machine-readable output.

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

## For agents

`skill/lap/SKILL.md` is a drop-in skill for Claude Code (copy it to your
project's `.claude/skills/lap/`) that teaches the cadence: start a session,
edit → commit → edit → commit, end the session, and how to recover when a
commit is rejected for containing more than one edit.

## License

MIT — see [../LICENSE](../LICENSE).
