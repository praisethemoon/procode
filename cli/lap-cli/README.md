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
line 12 of src/fetch.c was last touched by L23 fa9cebd (2026-09-20 14:31:07)
session: S4
intent:
  survive the flaky staging DNS, which drops ~2% of lookups
behavior:
  adds retry(): three attempts with exponential backoff
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
   No checkout, no revert, no staging area. Parallel work goes in other
   folders made lap branches, and `lap merge` adopts a branch's *history*
   after git has merged its code — lap never merges code itself.

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

A message written wrong is corrected with `lap amend`, not in a later
commit's message:

```sh
lap amend fa9cebd -i "retry on 429: staging returns it under load" \
    -b "calls retry() when send() answers 429 or 503"
```

It appends an amendment and changes nothing already written, so every hash
stays valid. From then on the commit shows its latest text everywhere,
marked "amended"; `lap show` lists the earlier texts. Only this folder's own
commits can be amended (a branch cannot amend what it inherited), and
`lap merge` carries a branch's amendments with its commits. A history
with amendments needs this version of lap or later.

### Commands

| command | purpose |
|---|---|
| `lap init` | create a repository in the current directory |
| `lap status` | pending edits per file, numbered |
| `lap commit <file> -i "intent" -b "behavior"` | record one edit and print its id and short hash (`-F` for both from a file, `--edit N` / `--lines A-B` to pick among several, `--force-message` to skip the repetition checks, `--no-session` to bypass sessions) |
| `lap amend <commit> -i "intent" -b "behavior"` | correct what a commit of this folder says; nothing written changes (`-F`, `--force-message` as for commit) |
| `lap log` | commits, newest first (`--session`, `--file`, `-n`) |
| `lap show <commit>` | one commit in full, by id, hash or hash prefix (`--full-file` reconstructs the file) |
| `lap search` | blame a line (`--file F --line N`), find text (`--text`), intents and behaviors (`--msg`), sessions, time ranges |
| `lap session` | `start "purpose"` (or `-F <file>`) / `end` / `list` / `current` |
| `lap rr` | review request: a run of work as trajectory, grouped by intent, + net change (`<session>`, `<from> <to>` as ids or hashes, `--no-diff`) |
| `lap verify` | check the log's hash chain (`--deep`: replay everything, audit caches) |
| `lap rebuild` | reconstruct every cache from the log (`--verify`: fail on a broken chain) |
| `lap branch start [name] --from <folder>` | make this folder a branch of another: its own line of history from that folder's head |
| `lap branch list` | the branches started here (and those known only by chunks git brought): active, merged, partly merged, missing |
| `lap branch forget <branch>` / `move <branch> <path>` | tend that list when a branch's folder is gone or moved |
| `lap merge <branch>` | after `git merge` brought a branch's code, adopt its history here (`--dry-run`, `--copy-from-folder`) |

Where branches exist, `commit`, `amend` and `session start` take
`--branch <name>` (or `LAP_BRANCH`) to say which line of history they record
to; `log`, `show`, `rr` and `session list` take it to read another branch's
history. A session of a branch is named `<branch>/S<n>`. Long flags take
their value apart or joined (`--branch feat`, `--branch=feat`).

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

An append-only JSONL log in `.lap/log/` — every record hash-chained to
the previous one (tamper- and corruption-evident, checked by `lap verify`),
kept as chunk files of up to 4 MB that are **sealed** (never written
again) once the next one starts — is the sole truth. Appending changes one
file, the open chunk, so git sees small diffs and never a rewritten
history. A folder with the single-file `.lap/log.jsonl` of older versions
is moved to chunks by its first write, record for record, published in
one rename so no reader sees it half done. A history with records of a
type a newer lap wrote is read as far as this lap understands it and never
written to.

Everything else in `.lap/` is a disposable acceleration cache: a
fixed-width index with per-file chains (O(1) commit lookup, blame that
walks only the file's own history), byte-budgeted content snapshots that
bound replay cost, the shadow store (each file as last committed), and a
stat cache that lets `lap status` skip files whose size and mtime have not
changed. Delete any of it, `lap rebuild` restores it; reads stay correct
(slower) even without it. Three files are not caches but this machine's
state, never rebuilt and never put in git: `.lap/lineage` (what makes a
folder a branch: delete it and the folder takes itself for main, which
writers and `rebuild` warn of), `.lap/parent` (where a branch folder's
parent is, for other tools) and `.lap/branches.json` (the branches
started here, and where their folders are). `rebuild` and `verify` read the history a chunk
at a time, so their memory stays near one file's state, not the history's.

lap reads CRLF as LF, as git stores text: a working file's `\r\n` endings
are compared and recorded as `\n`. `.lapignore` (gitignore-like subset) controls what is tracked: list
there whatever nobody edits by hand — build output, dependencies, and
vendored or generated code (a copied-in parser can be megabytes, and every
update of it would be another whole-file record). `.lapignore` keeps lap from recording files it has not recorded yet. A
file recorded before it was ignored stays tracked: its edits still show
in `lap status`, so none is lost unseen. To shed tracked files for good,
start a new history (a fresh `.lap/` from `lap init`): rare, and blunt,
since the whole history goes with them. Details in
[SPEC.md](SPEC.md).

## Branches and merging

Parallel work goes in another folder — usually a git worktree — made a
lap branch of the first:

```sh
git worktree add ../proj-parser -b parser
cd ../proj-parser
lap branch start parser --from ../proj
```

The parent's work must be committed first (lap and git): a branch starts
from the parent's committed state, and `branch start` refuses a folder
whose files differ (`not_clean`, naming files lap never recorded apart
from changed ones). The branch records to chunks of its own, so the two
folders never write to the same file. The parent keeps a registry of the
branches it started.

**Say which branch you record to.** Where branches exist, `lap commit`,
`lap amend` and `lap session start` need `--branch <name>` (`--branch
main` in the first folder) or `LAP_BRANCH`; a missing or wrong name is
refused with a message naming this folder's branch — you may be in the
wrong folder. A repository with no branches ignores `LAP_BRANCH` (set it
for a whole run), while an explicit `--branch` is always checked.

**Reading.** `lap branch list` shows each branch's state: active, merged,
partly merged, or missing (its folder gone: `lap branch move` points the
registry at the new place, `lap branch forget` drops it). A merged branch
whose folder is gone leaves the registry by itself, and a branch known only
by chunks git brought is listed too. `--branch <name>` on `log`, `show`,
`rr` and `session list` reads another branch's history from here. Session
ids repeat across folders, so a branch's sessions are named
`<branch>/S<n>` — cite that in text that leaves the folder.

When the work is done, merge it back in the parent folder, in this order:

1. commit the parent's own pending work (lap and git);
2. `git merge` the branch's git branch — this brings its code **and** its
   lap history (the chunks in `.lap/log/`);
3. `lap merge <branch>` (try `--dry-run` first).

lap adopts the branch history that `git merge` brought, as far as it
reaches, and never rewrites those files — so history and code agree, and
the next `git merge` of the branch never conflicts on `.lap/log/`. Work the
branch has not git-committed yet is adopted by the next `git merge` +
`lap merge`.

**`git_merge_first`** means step 2 was skipped: the branch folder is a git
checkout and none of its history is here yet. Run `git merge`, then
`lap merge` again. `--copy-from-folder` takes the history from the branch
folder anyway; use it only when you want the history before the code,
knowing `lap status` will show the difference until the code arrives.
Branches in plain folders without git are read from their folder directly.

Each branch commit is placed file by file against what the parent has done
since the base. Where the parent changed the same lines differently, that
file **stops** at that commit: its later commits are left, the rest of the
branch is still adopted, and what is left shows in `lap status` to commit by
hand, citing the branch commits (`#<hash>`). A change the parent already
made identically is recorded as already done, not as a conflict. Adopted
commits keep a `from` link to the original, and the branch's amendments
(`lap amend`) come along.

**Branches of branches.** A branch folder can start branches too, and merge
them back the same way. A branch of a branch can also be merged straight
into main: `git merge` it there, then `lap merge sub`, which adopts the work
of the branch between up to where `sub` started, too.

**The board.** In a lap branch folder, coboard works the parent's board,
never the branch's git copy of it; if the parent folder has moved, it says
so (`stale_parent`) rather than using the copy.

SPEC.md (Branches) has the details.

## Limitations

Worth knowing before you rely on it.

**lap cannot restore anything.** It records; it never writes to your files.
There is no checkout, revert, staging area, or remote, and `lap merge`
adopts history, never code. To get old content back you read it out of
`lap show --full-file` yourself.

**lap does not watch git.** It compares the working tree against its own
last-committed copy and nothing else. So when git changes files underneath
it — `checkout`, `pull`, `rebase`, `merge` — those changes are
indistinguishable from edits you made, and `lap status` will report them as
your pending work. Commit or discard before switching branches.

**What goes into git.** `lap init` writes `.lap/.gitignore`, so git keeps
only the history (`.lap/log/`) and nothing machine-local — above all not a
branch folder's `lineage` and `parent`, which would make the parent think
it is the branch. To keep the history on this machine too, add `.lap/` to
the project's own `.gitignore`.

**Upgrading from the single-file log.** A project whose `.gitignore` has
the older `.lap/*` and `!.lap/log.jsonl` would hide the chunks from git.
The first write after the upgrade moves `.lap/log.jsonl` to chunks and
writes `.lap/.gitignore` if there is none, whose `!/log/` lets git see
them. Commit `.lap/.gitignore` and `.lap/log/` together with the removal
of `.lap/log.jsonl`, as lap says when it converts.

**A shared history can be corrupted by merging it.** If two git branches of
*one folder* both record commits, merging them conflicts on the chunk both
appended to, and resolving that by interleaving lines breaks the hash
chain; `lap verify` detects it but cannot repair it. For parallel work, use
lap branches (above): each records to its own chunks, and `lap merge`
adopts them without git ever conflicting on `.lap/log/`.

**Renames are two commits**, because lap tracks paths, not file identity —
record the old path's disappearance and the new path's appearance, and name
the other path in each behavior. Blame and `lap log --file` cannot follow a
rename.

**Text files only.** A NUL byte in the first 8 KB marks a file binary and
lap refuses to track it. Files larger than 64 MB are refused outright.

**One commit is one contiguous run of lines.** A single logical change
scattered across a file becomes several commits — by design, but it does
mean the unit of history is mechanical rather than semantic.

**`lap status` walks the whole tree.** A stat cache spares it reading files
whose size and mtime have not changed, but it still visits every file, and
content changed with both put back is not seen until the cache entry goes
(as with git's index).

**Line endings.** lap reads CRLF as LF: a file's `\r\n` endings are recorded
as `\n`, so a change of line endings alone is no edit.

**One writer at a time.** An exclusive lock serializes writing commands for
their whole run; readers never lock and never block. A second writer waits
rather than failing.

**Caches are machine-local** and native-endian. Transport a repository as
its `log/` directory plus the working tree, then run `lap rebuild` on
arrival.

**Windows** builds with MSVC through CMake and runs the unit suite under
ctest. The e2e suite is a shell script, run on macOS and Linux.

## For agents

The repository's `.claude/skills/lap/` is a drop-in skill for Claude Code
(copy it to your project's `.claude/skills/lap/`) that teaches the cadence: start a session,
edit → commit → edit → commit, end the session, and how to recover when a
commit is rejected for containing more than one edit.

## License

MIT — see [../../LICENSE](../../LICENSE).
