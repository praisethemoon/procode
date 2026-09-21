---
name: lap
description: Record fine-grained edit history with the lap CLI while coding. Use whenever a lap repository (.lap directory) exists in the project, or the user asks to track edits with lap. Teaches the session -> edit -> commit cadence, one-edit-per-commit recovery, and history search.
---

# lap — fine-grained edit recording

lap is a flight recorder for your work: every small edit gets committed with
a reason, grouped into purposeful sessions. It is independent of git and
never modifies files.

## Identity

Commits record who made them. If you are an agent, set `LAP_USER` so your
commits are attributable (e.g. `LAP_USER="claude/bugfix-agent"`); otherwise
lap falls back to `git config user.name`, then the OS username.

## Cadence (follow this strictly)

1. **Before starting a task**: `lap session start "<task purpose>"` — one
   session per coherent task (a bug fix, a feature slice). If a session is
   already active (`lap session current`), decide: continue it if the task
   is the same, otherwise `lap session end` first.
2. **After every file edit you make**: commit it immediately.
   ```
   lap commit <file> -m "<why this edit exists>"
   ```
   One edit tool call ≈ one lap commit. Do not batch several unrelated
   edits and commit later — that is exactly what lap rejects.
3. **When the task is done**: `lap session end`.

## Commit messages

- Say **why**, not what — the diff already records what. Good:
  `"retry helper: staging DNS drops ~2% of lookups"`. Bad: `"add function"`.
- Multiline is welcome; the first line becomes the summary.
- The message is mandatory; empty messages are rejected.
- For long or multi-sentence messages, avoid shell-quoting pain by piping:
  ```
  printf 'summary line\n\nlonger rationale...' | lap commit <file> -F -
  ```
  (`-F <path>` reads from a file; `-m` and `-F` are mutually exclusive.)

## When a commit is rejected: "N separate edits detected"

You changed more than one place in the file, with at least one non-blank
unchanged line between the places (gaps of only blank lines never split an
edit — a rewrite around blank lines is one commit). lap lists the edits,
numbered, with line ranges. Commit them one at a time, each with its own
message:

```
lap commit src/foo.c -m "reason for the first edit" --edit 1
lap commit src/foo.c -m "reason for the second edit" --edit 1   # renumbered!
```

After each commit the remaining edits are re-detected and renumbered — run
`lap status` (or read the fresh error listing) rather than reusing old
numbers or old line ranges. `--lines A-B` also works and must exactly match
a listed range.

## Other situations

- `"no active session"` → start one, or use `--no-session` only for
  genuinely task-independent commits (e.g. committing `.lapignore` itself).
- New files commit whole as one edit — but a whole-file commit is only as
  interpretable as its message. When creating a **large** file, prefer
  building it in meaningful increments: write the skeleton, commit it,
  then add each section with its own commit and reason. Reserve one-shot
  creation for small files or content with a single clear purpose.
- Deleting a file is also a commit:
  `rm` it, then `lap commit <file> -m "why it is gone"`.
- **Renaming is two commits**, because lap tracks paths, not file identity.
  Record the old path's disappearance and the new path's appearance, and
  make each message name the other path:
  ```
  mv src/old.c src/new.c
  lap commit src/old.c -m "moved to src/new.c"
  lap commit src/new.c -m "renamed from src/old.c, no content changes"
  ```
  Blame and `lap log --file` cannot follow a rename, but those messages
  make the trail findable with `lap search --msg "old.c"`. You know it is a
  rename; the history only knows if you say so. If the content changed in
  transit, say what changed instead of hiding it behind "renamed".
- Binary files are not tracked; add noisy artifacts to `.lapignore`.
- `lap status --json`, and `--json` on every command, when you want to
  parse output.

## Using the history (do this before changing unfamiliar code)

- Why does this line exist? →
  `lap search --file <f> --line <n>` (current line number; pending edits
  are reported as pending).
- When did this string appear/disappear? →
  `lap search --text "<str>" [--added|--removed]`.
- What happened in a task? → `lap session list`, then
  `lap log --session S<n>`.
- Read one commit in full (message + diff): `lap show L<n>`; add
  `--full-file` to see the whole file as of that commit.

## Sanity

`lap verify` checks the log's hash chain; `lap verify --deep` also replays
history against the shadow store and snapshot cache. `lap rebuild`
reconstructs every cache from the log — run it after copying a repo by its
log alone, deleting anything under `.lap/` other than `log.jsonl`, or when
a cache looks wrong. Truth lives in the log; everything else is
regenerable.
