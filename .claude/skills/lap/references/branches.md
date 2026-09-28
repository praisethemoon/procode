# lap branches: parallel work in other folders

Read this before starting, merging or fixing a branch. `SKILL.md` has the
short flow; this is everything else.

## What a branch folder is

Two agents in one folder see each other's edits as their own. Parallel
work goes in **branch folders**: a git worktree (or a plain copy) of the
project, made a branch of the first folder:

```
git worktree add ../proj-parser -b parser     # the folder: git's job
cd ../proj-parser
lap branch start parser --from ../proj         # its own line of history
```

## A branch folder inside the project

The branch folder may also live inside the project (a tool's
`.claude/worktrees/<name>`, `git worktree add sub`): a subfolder with its
own `.lap/` is another repository, so its files never show in the
parent's `lap status`, and a commit of one from the parent is refused
("outside this repository: sub has its own .lap/"). The same rule means a
**leftover `.lap/`** in a subfolder hides that whole folder from lap: if
its files never show in `lap status`, or a commit gives that message for
a folder that is no branch, tell the user — never delete a `.lap/`
yourself.

## Starting one

- The parent's work must be committed (lap and git) first: a branch starts
  from the parent's committed state, and `branch start` refuses files that
  differ (`not_clean`).
- **`branch start` runs in the new folder**, with `--from` naming the
  parent. `has_branches` ("a folder with branches cannot become a branch")
  means you ran it in the parent: move to the new folder. `name_taken`
  means the name is, or was, some branch's that main can see: pick
  another. `ambiguous_branch` ("names more than one branch … name it by
  its id") means two old branches share a name: pass the id it lists.

## A copy is not the branch (`copied_branch`)

**A plain copy of a branch folder is not that branch.** Writers refuse
there with `copied_branch` ("… is a copy of branch <name>, which is
<original>: two folders never record to one branch"). Either start a
branch of its own in the copy (`lap branch start <name> --from
<original>`, the way to make a branch of a branch by copying), or do the
work in the original folder.

## Saying which branch you record to

**Say which branch you record to.** Where branches exist, `lap commit`
and `lap session start` need `--branch <name>` (`--branch main` in the
first folder), or `LAP_BRANCH=<name>` in the environment. A missing or
wrong name is refused and the error names the folder's branch: it means
you may be in the wrong folder — check before retrying. A repository
with no branches ignores `LAP_BRANCH` (an explicit `--branch` is still
checked).

## Merging back

**Merging back**, in the parent folder: commit its own work, `git merge`
the branch, then `lap merge <branch>` (try `--dry-run` first) — always in
that order. lap adopts the history `git merge` brought; on
`git_merge_first` run the `git merge` you skipped, then `lap merge`
again. Never reach for `--copy-from-folder` unless the user asks for
it.

lap adopts every branch commit it can place, with a `from` link to the
original; a file where the branch's work conflicts with the parent's
stops, and what is left shows in `lap status` — commit it as usual,
citing the branch commits (`#<hash>`) it stands for. A change the parent
had already made the same way is reported as already done, not as a
conflict; a file an earlier merge stopped stays stopped (the report says
so). The branch's `lap amend` corrections come along with its commits.

## Branches of branches

A branch folder can start branches too (`lap branch start sub --from
../proj-parser`), and merge them back the same way, in its own folder. A
branch of a branch can also go straight into main: `git merge` it there,
then `lap merge sub`, which adopts the work of the branch between up to
where `sub` started, too. `lap branch list` in main shows `sub` under
`parser`.

## Never merge the parent into a branch

**Merging the parent into a branch is not supported.** Never `git merge`
the parent (main) into a branch folder to stay current: its changes would
show as the branch's own pending edits and, if committed, come back to the
parent as branch work. To take in the parent's newer work, merge the
branch back, then start a fresh branch.

## Naming commits and sessions across folders

Ids repeat across folders (both go on from the base). In text that leaves
the folder — a ticket comment, another branch's commit — cite commits by
hash and sessions as `<branch>/S<n>`. lap prints a branch's sessions that
way, and `lap rr`, `log --session` and `search --session` take it:
`lap rr parser/S4`, from the parent after a merge too.

## Seeing branches, and one that moved

`lap branch list` shows each branch started here (and those known only by
chunks git brought) with its state: active, merged, partly merged,
missing. `--branch <name>` on `log`, `show`, `rr` and `session list` reads
that branch's history from here. A branch whose folder moved is
`missing`: `lap branch move <branch> <path>` points the registry at its
new place, `lap branch forget <branch>` drops it.
