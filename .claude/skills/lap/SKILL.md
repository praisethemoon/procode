---
name: lap
description: Record fine-grained edit history with the lap CLI while coding. Use whenever a lap repository (.lap directory) exists in the project, or the user asks to track edits with lap. Teaches the session -> edit -> commit cadence, one-edit-per-commit recovery, and history search.
---

# lap — fine-grained edit recording

lap is a flight recorder for your work: every small edit gets committed with
its intent and what it does, grouped into purposeful sessions. It is
independent of git and never modifies files.

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
   lap commit <file> -i "<why this edit exists>" -b "<what it makes the code do>"
   ```
   It prints the new commit's id and short hash: `[L42 fa9cebd] ...`.
   One edit tool call ≈ one lap commit. Do not batch several unrelated
   edits and commit later — that is exactly what lap rejects.
3. **When the task is done**: `lap session end`.

## Commit messages: intent and behavior

Every commit has two fields, both required:

- **intent** (`-i`) — *why* the edit exists: the goal it serves, written as
  the goal, not the mechanics. Edits that serve one goal **share** an
  intent, word for word.
- **behavior** (`-b`) — *what this edit makes the code do*. Every commit
  has its own; a hunk that cannot say what it does is a hunk to rethink.

A goal that takes an import and its use is two commits with one intent:

```
lap commit src/App.tsx -i "Route the graph place to its view." \
  -b "Render Graph when the target place is graph; every other place still gets Collections."
  # -> [L42 fa9cebd] ...
lap commit src/App.tsx -i "Route the graph place to its view." \
  -b "Import Graph for the graph route in #fa9cebd."
```

- **Cite another commit by its hash**, as `#` plus the short hash that
  `lap commit` printed (`#fa9cebd`). Words around it are yours ("needs",
  "fixes", "reverts"); readers follow it with `lap show fa9cebd`, and the
  editor views turn it into a link.
- **Never number fragments** (`(1/3) … (3/3)`, `(n/m)`). A shared intent
  is what ties fragments together; each fragment's behavior says what that
  piece does.
- Multiline is welcome in either field; the first line is its summary.
- For long text, avoid shell quoting by writing both fields to a file (or
  stdin) with `-F`: a line `Intent:`, its text, a line `Behavior:`, its
  text (either order, each once):
  ```
  printf 'Intent:\nRoute the graph place to its view.\n\nBehavior:\nImport Graph for the graph route in #fa9cebd.\n' \
    | lap commit src/App.tsx -F -
  ```
  (`-F` and `-i`/`-b` are mutually exclusive.)

### When lap refuses the message

`lap commit` checks the message before writing anything:

| error | meaning | fix |
|---|---|---|
| `missing_intent` / `missing_behavior` | a field was not given | give both `-i` and `-b` |
| `message_too_short` | intent or behavior has fewer than 3 words | say it in a sentence |
| `behavior_repeats_intent` | the behavior is (nearly) the intent's words | say what *this edit* does, not why |
| `behavior_repeats_previous` | the behavior is (nearly) the session's previous one | say what this edit does that the last one did not |
| `behavior_restates_code` | the behavior is (nearly) the changed lines' words | say what the lines *do*, not what they say |
| `bad_message_file` | the `-F` file is not two `Intent:`/`Behavior:` sections | fix the file |

`--force-message` skips the three repetition checks (never the length
check) and marks the commit `forced`, which `lap show` and `lap rr` print.
Use it only when a near-duplicate is honestly the right description —
e.g. the same one-line fix applied to two copies of a function — never to
get past a message you could have written better.

Unknown flags are refused (`unknown_flag`), naming the flag.

`--dry-run` checks a commit without making it: it picks the edit, runs the
message checks and prints what would be recorded, failing exactly as the
commit would, and writes nothing. Use it to test a message, to see which
edit `--edit N` takes, or before a batch of baseline commits.

## When a commit is rejected: "N separate edits detected"

You changed more than one place in the file, with at least one non-blank
unchanged line between the places (gaps of only blank lines never split an
edit — a rewrite around blank lines is one commit). lap lists the edits,
numbered, with line ranges. Commit them one at a time, oldest first, each
with its own behavior (and the shared intent, if they serve one goal):

```
lap commit src/foo.c -i "<goal>" -b "<what the first edit does>" --edit 1
lap commit src/foo.c -i "<goal>" -b "<what the second edit does>" --edit 1   # renumbered!
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
  then add each section with its own commit. Reserve one-shot
  creation for small files or content with a single clear purpose.
- Deleting a file is also a commit:
  `rm` it, then `lap commit <file> -i "why it is gone" -b "what no longer exists"`.
- **Renaming is two commits**, because lap tracks paths, not file identity.
  Record the old path's disappearance and the new path's appearance, and
  make each behavior name the other path:
  ```
  mv src/old.c src/new.c
  lap commit src/old.c -i "Name the module after what it does." -b "Removes src/old.c; its content moved to src/new.c."
  lap commit src/new.c -i "Name the module after what it does." -b "Adds src/new.c, renamed from src/old.c with no content changes."
  ```
  Blame and `lap log --file` cannot follow a rename, but those messages
  make the trail findable with `lap search --msg "old.c"`. You know it is a
  rename; the history only knows if you say so. If the content changed in
  transit, say what changed instead of hiding it behind "renamed".
- Binary files are not tracked; add noisy artifacts to `.lapignore`.
- `lap status --json`, and `--json` on every command, when you want to
  parse output.
- Output is coloured only at a terminal, so piped output is plain. If you
  run lap through a pseudo-terminal, pass `--color=never` (or set
  `NO_COLOR=1`) to keep escape sequences out of what you read back.

## Using the history (do this before changing unfamiliar code)

- Why does this line exist? →
  `lap search --file <f> --line <n>` (current line number; pending edits
  are reported as pending).
- When did this string appear/disappear? →
  `lap search --text "<str>" [--added|--removed]`.
- What happened in a task? → `lap session list`, then
  `lap log --session S<n>`.
- Read one commit in full (intent, behavior, diff): `lap show L<n>`, or
  by hash or hash prefix (`lap show fa9cebd`, `#fa9cebd` works too); add
  `--full-file` to see the whole file as of that commit.
- **Review a whole piece of work** — before handing it over, or to catch up
  on someone else's: `lap rr <session>` prints the trajectory (commits in
  order, grouped under their shared intent, each with its behavior) and
  the net change (each file diffed from before the
  work to after, so cancelled-out edits vanish and many commits to one
  function read as a single change). `lap rr <from> <to>` reviews a range
  (ids or hashes);
  `lap rr` alone reviews the latest session.

## Sanity

`lap verify` checks the log's hash chain; `lap verify --deep` also replays
history against the shadow store and snapshot cache. `lap rebuild`
reconstructs every cache from the log — run it after copying a repo by its
log alone, deleting anything under `.lap/` other than `log/`, or when
a cache looks wrong. Truth lives in the log; everything else is
regenerable.
