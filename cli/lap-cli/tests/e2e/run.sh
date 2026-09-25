#!/bin/sh
# End-to-end tests: drive the real lap binary through full workflows in a
# throwaway directory. Requires LAP=<absolute path to the lap binary>.
set -u
LAP="${LAP:?set LAP to the lap binary path}"

TESTS=0
FAILED=0
CUR=""

t() { CUR="$1"; TESTS=$((TESTS + 1)); }
fail() { FAILED=$((FAILED + 1)); echo "FAIL [$CUR] $1"; }

expect_ok() {
    if ! "$@" >/dev/null 2>&1; then fail "expected success: $*"; fi
}
expect_fail() {
    if "$@" >/dev/null 2>&1; then fail "expected failure: $*"; fi
}
# expect_grep <pattern> <cmd...>: command output (stdout+stderr) must match
expect_grep() {
    pat="$1"; shift
    out=$("$@" 2>&1)
    if ! printf '%s\n' "$out" | grep -q "$pat"; then
        fail "expected /$pat/ in: $*
--- output ---
$out
--------------"
    fi
}
expect_not_grep() {
    pat="$1"; shift
    out=$("$@" 2>&1)
    if printf '%s\n' "$out" | grep -q "$pat"; then
        fail "expected NO /$pat/ in: $*
--- output ---
$out
--------------"
    fi
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/lap-e2e.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1

# ---------------------------------------------------------------- init
t "init creates the repository"
expect_ok "$LAP" init
[ -d .lap ] || fail ".lap directory missing"
[ -f .lap/log.jsonl ] || fail "log.jsonl missing"
[ -f .lap/state.json ] || fail "state.json missing"
[ -f .lapignore ] || fail "starter .lapignore missing"

t "second init fails"
expect_fail "$LAP" init

t "verify passes on a fresh repository"
expect_grep "chain ok" "$LAP" verify

# ------------------------------------------------------- session gating
t "commit without a session is rejected"
printf 'alpha\nbeta\ngamma\n' > notes.txt
expect_fail "$LAP" commit notes.txt -m "should be rejected"
expect_grep "no_session" "$LAP" commit notes.txt -m "x" --json

t "commit with --no-session works without a session"
printf 'standalone\n' > solo.txt
expect_ok "$LAP" commit solo.txt -m "standalone note, deliberately outside sessions" --no-session
expect_grep '"session":null' "$LAP" log --json
expect_ok "$LAP" commit .lapignore -m "starter ignore list from lap init" --no-session

t "session start/current/end lifecycle"
expect_ok "$LAP" session start "capture the notes file"
expect_grep "S1" "$LAP" session current
expect_fail "$LAP" session start "second session while one is active"
expect_ok "$LAP" session end
expect_fail "$LAP" session end
expect_ok "$LAP" session start "capture the notes file, take two"

t "session needs a message"
"$LAP" session end >/dev/null 2>&1
expect_fail "$LAP" session start
expect_ok "$LAP" session start "real work"

# --------------------------------------------------------- create commit
t "new file commits whole content as one edit"
expect_ok "$LAP" commit notes.txt -m "seed notes: alpha/beta/gamma baseline"
expect_grep "create" "$LAP" log
expect_grep "no changes" "$LAP" commit notes.txt -m "nothing changed"

# ------------------------------------------------- single edit commit
t "single-region change commits without a selector"
printf 'alpha\nBETA\ngamma\n' > notes.txt
expect_ok "$LAP" commit notes.txt -m "shout beta: it is the important one"
expect_grep "clean" "$LAP" status

# -------------------------------------------------- multi edit workflow
t "two separated edits are rejected and listed"
printf 'ALPHA\nBETA\nGAMMA\n' > notes.txt
expect_fail "$LAP" commit notes.txt -m "two edits at once"
expect_grep "2 separate edits" "$LAP" commit notes.txt -m "two edits"
expect_grep "multiple_edits" "$LAP" commit notes.txt -m "x" --json

t "--edit selects one region; the rest stays pending"
expect_ok "$LAP" commit notes.txt -m "uppercase alpha" --edit 1
expect_grep "1 edit" "$LAP" status
expect_ok "$LAP" commit notes.txt -m "uppercase gamma" --edit 1
expect_grep "clean" "$LAP" status

t "--lines must match a detected region exactly"
printf 'ALPHA\nbeta2\nGAMMA\ndelta\n' > notes.txt
expect_fail "$LAP" commit notes.txt -m "bad range" --lines 1-4
expect_ok "$LAP" commit notes.txt -m "lower beta again" --lines 2-2
expect_ok "$LAP" commit notes.txt -m "append delta" --lines 4-4
expect_grep "clean" "$LAP" status

t "--edit out of range is a clean error"
printf 'ALPHA\nbeta3\nGAMMA\ndelta\n' > notes.txt
expect_fail "$LAP" commit notes.txt -m "x" --edit 7
expect_ok "$LAP" commit notes.txt -m "beta version 3"

# ------------------------------------------------------------- status
t "status reports states and numbered edits"
printf 'one\ntwo\n' > fresh.txt
printf 'ALPHA\nbeta4\nGAMMA\nDELTA\n' > notes.txt
expect_grep "new       fresh.txt" "$LAP" status
expect_grep "modified  notes.txt" "$LAP" status
expect_grep "\[2\]" "$LAP" status
expect_grep '"state":"new"' "$LAP" status --json
rm fresh.txt
expect_ok "$LAP" commit notes.txt -m "beta version 4" --edit 1
expect_ok "$LAP" commit notes.txt -m "uppercase delta" --edit 1

# --------------------------------------------------------------- log
t "log filters by file and session"
expect_grep "notes.txt" "$LAP" log --file notes.txt
expect_not_grep "solo.txt" "$LAP" log --file notes.txt
expect_grep '"commits"' "$LAP" log --json
expect_grep "uppercase delta" "$LAP" log -n 1
expect_not_grep "seed notes" "$LAP" log -n 1

# --------------------------------------------------------------- show
t "show renders a commit and reconstructs the file"
ID=$("$LAP" log --file notes.txt --json | sed 's/.*"commits":\[{"id":"\([^"]*\)".*/\1/')
[ -n "$ID" ] || fail "could not extract a commit id"
expect_grep "commit $ID" "$LAP" show "$ID"
expect_grep "@@" "$LAP" show "$ID"
expect_grep "file after this commit" "$LAP" show "$ID" --full-file
expect_grep '"file_content"' "$LAP" show "$ID" --full-file --json
expect_fail "$LAP" show L9999

# ------------------------------------------------------------- search
t "search --line traces a committed line"
expect_grep "last touched by" "$LAP" search --file notes.txt --line 2
expect_grep '"pending":false' "$LAP" search --file notes.txt --line 2 --json

t "search --line flags pending edits"
printf 'ALPHA\nbeta4\nGAMMA\nDELTA\nnew tail\n' > notes.txt
expect_grep "pending" "$LAP" search --file notes.txt --line 5
expect_ok "$LAP" commit notes.txt -m "tail marker for search tests"

t "search --text finds added content"
expect_grep "tail marker" "$LAP" search --text "new tail"
expect_grep "added line" "$LAP" search --text "new tail"
expect_grep "no matching commits" "$LAP" search --text "never-written-string"

t "search --msg and --session filters"
expect_grep "seed notes" "$LAP" search --msg "baseline"
expect_grep "standalone" "$LAP" search --msg "standalone"
expect_not_grep "standalone" "$LAP" search --session S3
expect_grep "no matching commits" "$LAP" search --msg baseline --session S1

# ------------------------------------------------------------- delete
t "deleting a file is a commit"
rm notes.txt
expect_grep "deleted   notes.txt" "$LAP" status
expect_ok "$LAP" commit notes.txt -m "notes.txt retired after the tests"
expect_grep "clean" "$LAP" status
expect_grep "delete" "$LAP" log -n 1

# ------------------------------------------------------------ ignore
t ".lapignore hides files and blocks commits"
printf '*.log\n' >> .lapignore
printf 'noise\n' > debug.log
expect_not_grep "debug.log" "$LAP" status
expect_fail "$LAP" commit debug.log -m "should be refused"
expect_ok "$LAP" commit .lapignore -m "ignore build noise: *.log"

# ------------------------------------------------------ subdirectories
t "files in subdirectories work from repo root and from inside"
mkdir -p src/deep
printf 'content\n' > src/deep/mod.c
expect_ok "$LAP" commit src/deep/mod.c -m "deep module placeholder"
( cd src/deep && "$LAP" status >/dev/null 2>&1 ) || fail "status from subdir"
( cd src/deep && "$LAP" commit mod.c -m "no change expected" ) \
    >/dev/null 2>&1 && fail "expected no-changes failure from subdir"

t "paths outside the repository are rejected"
expect_fail "$LAP" commit /etc/hosts -m "outside"

# ------------------------------------------- trailing-newline handling
t "trailing-newline-only change is one committable edit"
printf 'x\ny' > tail.txt
expect_ok "$LAP" commit tail.txt -m "tail file without trailing newline"
printf 'x\ny\n' > tail.txt
expect_grep "1 edit" "$LAP" status
expect_ok "$LAP" commit tail.txt -m "add trailing newline for POSIX tools"
expect_grep "clean" "$LAP" status

# ------------------------------------------------------------- verify
t "verify --deep passes on a healthy repository"
expect_grep "0 mismatch" "$LAP" verify --deep
expect_grep '"chain_ok":true' "$LAP" verify --json

t "state.json loss self-heals (readers in memory, writers persist)"
NEXT_BEFORE=$(sed 's/.*"next_commit":\([0-9]*\).*/\1/' .lap/state.json)
rm .lap/state.json
expect_grep "clean" "$LAP" status
[ -f .lap/state.json ] && fail "a read-only command must not write state"
printf 'healed\n' > heal0.txt
expect_ok "$LAP" commit heal0.txt -m "the first writer after state loss persists the healed state"
NEXT_AFTER=$(sed 's/.*"next_commit":\([0-9]*\).*/\1/' .lap/state.json)
[ "$((NEXT_BEFORE + 1))" = "$NEXT_AFTER" ] || \
    fail "healed next_commit $NEXT_AFTER != $NEXT_BEFORE + 1"

t "log tampering is detected"
cp .lap/log.jsonl .lap/log.jsonl.bak
sed 's/retired after the tests/RETIRED AFTER THE TESTS/' \
    .lap/log.jsonl > .lap/log.tampered && mv .lap/log.tampered .lap/log.jsonl
expect_grep "CHAIN BROKEN" "$LAP" verify
expect_fail "$LAP" verify
mv .lap/log.jsonl.bak .lap/log.jsonl
expect_grep "chain ok" "$LAP" verify

# ------------------------------------------- blank lines are not anchors
t "edits separated only by blank lines are ONE edit"
printf 'aaa\n\nbbb\nxxx\nccc\n\nddd\n' > blanky.txt
expect_ok "$LAP" commit blanky.txt -m "blanky baseline: two blocks split by a real anchor line xxx"
printf 'AAA\n\nBBB\nxxx\nccc\n\nddd\n' > blanky.txt
expect_grep "1 edit" "$LAP" status
expect_ok "$LAP" commit blanky.txt -m "rewrite the first block: blank gap must not split it"
printf 'AAA\n\nBBB\nxxx\nCCC\n\nDDD\n' > blanky.txt
expect_grep "1 edit" "$LAP" status
expect_ok "$LAP" commit blanky.txt -m "rewrite the second block in one commit too"
expect_grep "clean" "$LAP" status
expect_grep "0 mismatch" "$LAP" verify --deep

t "edits across a non-blank line still split"
printf 'ZZZ\n\nBBB\nxxx\nCCC\n\nQQQ\n' > blanky.txt
expect_grep "2 edits" "$LAP" status
expect_ok "$LAP" commit blanky.txt -m "first block again" --edit 1
expect_ok "$LAP" commit blanky.txt -m "last block again" --edit 1

# ------------------------------------------------- message via -F / stdin
t "message from a file with -F"
printf 'summary from file\n\nlong rationale line two\n' > msg.tmp
printf 'file-msg-test\n' > fmsg.txt
expect_ok "$LAP" commit fmsg.txt -F msg.tmp
expect_grep "summary from file" "$LAP" log -n 1
# --msg matches the FULL message (the hit word is on line 3), while the
# result row displays the summary line
expect_grep "summary from file" "$LAP" search --msg "rationale"
expect_grep "long rationale line two" "$LAP" search --msg "rationale" --json

t "message from stdin with -F -"
printf 'stdin-msg-test\n' > smsg.txt
printf 'piped summary\npiped detail' | "$LAP" commit smsg.txt -F - \
    >/dev/null 2>&1 || fail "stdin commit failed"
expect_grep "piped summary" "$LAP" log -n 1

t "-m and -F together are rejected; empty -F is rejected"
printf 'x\n' > conflict.txt
expect_fail "$LAP" commit conflict.txt -m "a" -F msg.tmp
: > empty.tmp
expect_fail "$LAP" commit conflict.txt -F empty.tmp
expect_ok "$LAP" commit conflict.txt -m "conflict.txt landed with -m as usual"

t "commit confirmation echoes the message summary"
printf 'echo-check\n' > echocheck.txt
expect_grep "message summary appears in output" \
    "$LAP" commit echocheck.txt -m "message summary appears in output"

# -------------------------------------------------- crash-safety repairs
t "torn log tail: readers tolerate it, the next writer repairs it"
printf '{"type":"commit","id":"L9' >> .lap/log.jsonl
expect_ok "$LAP" log
expect_grep "torn trailing record" "$LAP" verify
printf 'torn-recovery\n' > torn.txt
expect_ok "$LAP" commit torn.txt -m "commit after a crash-torn append: the writer truncates the torn bytes first"
expect_grep "chain ok" "$LAP" verify
expect_grep "0 mismatch" "$LAP" verify --deep

t "verify --deep detects a missing shadow file"
rm .lap/shadow/torn.txt
expect_fail "$LAP" verify --deep
expect_grep "missing shadow" "$LAP" verify --deep

t "a writing command heals lost state.json and rebuilds shadows"
rm .lap/state.json
printf 'heal-me\n' > healfile.txt
expect_ok "$LAP" commit healfile.txt -m "this write triggers a full heal first"
expect_grep "chain ok" "$LAP" verify
expect_grep "0 mismatch" "$LAP" verify --deep

# ------------------------------------ partial-commit coordinate integrity
t "partial commit stores committed-file coordinates; blame stays correct"
printf 'p1\np2\np3\np4\np5\n' > coord.txt
expect_ok "$LAP" commit coord.txt -m "coord baseline"
printf 'TOP\nTOPB\np1\np2\np3\nP4\np5\n' > coord.txt
CID=$("$LAP" commit coord.txt -m "uppercase p4 (committed before the top insertion)" --edit 2 --json | sed 's/.*"id":"\([^"]*\)".*/\1/')
[ -n "$CID" ] || fail "could not extract the partial commit id"
expect_grep "last touched by $CID" "$LAP" search --file coord.txt --line 6
expect_ok "$LAP" commit coord.txt -m "top insertion, committed second"
expect_grep "last touched by $CID" "$LAP" search --file coord.txt --line 6

t "blame on an uncommitted new file reports pending"
printf 'n1\nn2\n' > newpend.txt
expect_grep "pending" "$LAP" search --file newpend.txt --line 1
expect_grep '"pending":true' "$LAP" search --file newpend.txt --line 2 --json
expect_ok "$LAP" commit newpend.txt -m "newpend baseline"

# ----------------------------------------------------- argument hygiene
t "a message that looks like a flag is a message, not a flag"
printf 'f1\n' > flagmsg.txt
expect_ok "$LAP" commit flagmsg.txt -m "--no-session"
expect_grep '"session":"S' "$LAP" log -n 1 --json

t "-- ends flags: files named like flags are committable"
printf 'd1\n' > ./-dash.txt
expect_ok "$LAP" commit -m "dash-named file survives the parser" -- -dash.txt
expect_grep "dash-named" "$LAP" log -n 1

t "paths with quotes stay valid JSON in blame output"
printf 'q1\n' > 'q"uote.txt'
expect_grep 'q\\"uote.txt' "$LAP" search --file 'q"uote.txt' --line 1 --json
expect_ok "$LAP" commit 'q"uote.txt' -m "quoted-name file landed"

# ------------------------------------------------------------- user field
t "commits record the user (LAP_USER wins the resolution)"
printf 'u1\n' > userfile.txt
LAP_USER="e2e-test-bot" "$LAP" commit userfile.txt -m "user field: recorded from LAP_USER"
expect_grep '"user":"e2e-test-bot"' "$LAP" log -n 1 --json
UID_SHOW=$("$LAP" log -n 1 --json | sed 's/.*"id":"\([^"]*\)".*/\1/')
expect_grep "user: e2e-test-bot" "$LAP" show "$UID_SHOW"

# --------------------------------------------- acceleration layer / rebuild
t "reads are identical with the index, without it, and after rebuild"
WITH_IDX=$("$LAP" log -n 3)
BLAME_IDX=$("$LAP" search --file coord.txt --line 6)
rm -f .lap/index .lap/paths .lap/heads
NO_IDX=$("$LAP" log -n 3)
BLAME_NO=$("$LAP" search --file coord.txt --line 6)
[ "$WITH_IDX" = "$NO_IDX" ] || fail "log -n differs without the index"
[ "$BLAME_IDX" = "$BLAME_NO" ] || fail "blame differs without the index"
expect_ok "$LAP" rebuild
AFTER=$("$LAP" log -n 3)
[ "$WITH_IDX" = "$AFTER" ] || fail "log -n differs after rebuild"

t "the cache contract, file by file: deleting ANY one cache changes only speed"
# group deletion is not enough — a half-present index is the dangerous state
BLAME=$("$LAP" search --file coord.txt --line 6)
LOGN=$("$LAP" log -n 3)
SHOWF=$("$LAP" show "$CID" --full-file)
# status reads the shadow store and rr replays the log; both answer from a
# different direction than blame does, so both belong in this loop
STAT=$("$LAP" status)
RR=$("$LAP" rr --no-diff)
# the backup lives outside the repository: a copy inside it would show up
# as untracked files and change what status reports
CACHEBAK=$(mktemp -d "${TMPDIR:-/tmp}/lap-cachebak.XXXXXX")
# an unchecked mktemp would leave CACHEBAK empty and aim the loop's
# rm -rf "$CACHEBAK/lap" at /lap
[ -n "$CACHEBAK" ] && [ -d "$CACHEBAK" ] || { echo "mktemp failed for the cache backup" >&2; exit 1; }
for cache in index paths heads state.json snapshots shadow; do
    rm -rf "$CACHEBAK/lap" && cp -R .lap "$CACHEBAK/lap"
    rm -rf ".lap/$cache"
    OUT=$("$LAP" search --file coord.txt --line 6 2>&1)
    [ "$OUT" = "$BLAME" ] || fail "rm .lap/$cache changed blame output"
    OUT=$("$LAP" log -n 3 2>&1)
    [ "$OUT" = "$LOGN" ] || fail "rm .lap/$cache changed log output"
    OUT=$("$LAP" show "$CID" --full-file 2>&1)
    [ "$OUT" = "$SHOWF" ] || fail "rm .lap/$cache changed show --full-file"
    OUT=$("$LAP" status 2>&1)
    [ "$OUT" = "$STAT" ] || fail "rm .lap/$cache changed status output"
    OUT=$("$LAP" rr --no-diff 2>&1)
    [ "$OUT" = "$RR" ] || fail "rm .lap/$cache changed rr output"
    rm -rf .lap && cp -R "$CACHEBAK/lap" .lap
done
rm -rf "$CACHEBAK"
expect_ok "$LAP" rebuild

t "a stale sidecar cannot outlive the index it belongs to"
cp .lap/heads .lap/heads.old && cp .lap/paths .lap/paths.old
printf 'stale probe\n' > stale.txt
expect_ok "$LAP" commit stale.txt -m "commit that grows the path table"
cp .lap/heads.old .lap/heads && cp .lap/paths.old .lap/paths   # rewind sidecars
OUT=$("$LAP" search --file stale.txt --line 1 2>&1)
rm -f .lap/heads.old .lap/paths.old
expect_ok "$LAP" rebuild
EXPECT=$("$LAP" search --file stale.txt --line 1 2>&1)
[ "$OUT" = "$EXPECT" ] || fail "stale sidecars changed blame: [$OUT] vs [$EXPECT]"

t "commit ids are verified, not just parsed"
expect_fail "$LAP" show L1x
expect_grep "no commit named L1x" "$LAP" show L1x
rm -f .lap/index
expect_grep "no commit named L1x" "$LAP" show L1x
expect_ok "$LAP" rebuild

t "session filters mean the same thing with and without the index"
for s in S1 S01 S1x S ""; do
    A=$("$LAP" log --session "$s" -n 5 2>&1)
    mv .lap/index .lap/index.off
    B=$("$LAP" log --session "$s" -n 5 2>&1)
    mv .lap/index.off .lap/index
    [ "$A" = "$B" ] || fail "--session '$s' differs with/without the index"
done

t "--text results match with and without the index"
A=$("$LAP" search --text "tail marker" 2>&1)
mv .lap/index .lap/index.off
B=$("$LAP" search --text "tail marker" 2>&1)
mv .lap/index.off .lap/index
[ "$A" = "$B" ] || fail "--text differs with/without the index"

t "a crashed atomic write leaves no phantom in verify"
printf 'junk\n' > .lap/shadow/coord.txt.tmp.987654
expect_grep "0 mismatch" "$LAP" verify --deep
rm -f .lap/shadow/coord.txt.tmp.987654

t "the cache contract: rm every cache, readers still work, rebuild restores"
rm -rf .lap/shadow .lap/snapshots .lap/state.json \
       .lap/index .lap/paths .lap/heads
expect_ok "$LAP" log -n 1
expect_ok "$LAP" rebuild
expect_grep "chain ok" "$LAP" verify
expect_grep "0 mismatch" "$LAP" verify --deep
expect_grep "clean" "$LAP" status

t "rebuild --verify fails on a tampered log"
cp .lap/log.jsonl .lap/log.jsonl.bak
sed 's/quoted-name file landed/QUOTED-NAME FILE LANDED/' \
    .lap/log.jsonl > .lap/log.t && mv .lap/log.t .lap/log.jsonl
expect_fail "$LAP" rebuild --verify
mv .lap/log.jsonl.bak .lap/log.jsonl
expect_ok "$LAP" rebuild --verify

# ----------------------------------------------------------- snapshots
t "hot files earn snapshots; replay stays exact; verify audits them"
printf 'snap base\n' > snapfile.txt
expect_ok "$LAP" commit snapfile.txt -m "snapshot probe baseline"
i=0
while [ $i -lt 20 ]; do
    printf 'snap base edited %s\n' "$i" > snapfile.txt
    "$LAP" commit snapfile.txt -m "snapshot probe edit $i" >/dev/null \
        || fail "snapshot probe commit $i"
    i=$((i + 1))
done
[ -f .lap/snapshots/snapfile.txt.jsonl ] || fail "no snapshot was taken"
SID=$("$LAP" log -n 1 --file snapfile.txt --json | \
      sed 's/.*"id":"\([^"]*\)".*/\1/')
expect_grep "snap base edited 19" "$LAP" show "$SID" --full-file
expect_grep "0 mismatch" "$LAP" verify --deep

t "a corrupted snapshot is caught by verify --deep and cured by rebuild"
cp .lap/snapshots/snapfile.txt.jsonl .lap/snap.bak
sed 's/snap base/SNAP BASE/' .lap/snapshots/snapfile.txt.jsonl \
    > .lap/snap.t && mv .lap/snap.t .lap/snapshots/snapfile.txt.jsonl
expect_grep "stale snapshot" "$LAP" verify --deep
expect_fail "$LAP" verify --deep
expect_ok "$LAP" rebuild
expect_grep "0 mismatch" "$LAP" verify --deep
rm -f .lap/snap.bak

t "snapshots are throwaway: deleting them changes nothing but speed"
BEFORE_SNAP=$("$LAP" show "$SID" --full-file)
rm -rf .lap/snapshots
AFTER_SNAP=$("$LAP" show "$SID" --full-file)
[ "$BEFORE_SNAP" = "$AFTER_SNAP" ] || fail "replay differs without snapshots"
expect_ok "$LAP" rebuild

# --------------------------------------------------------------- json
t "json outputs stay machine-readable"
expect_grep '"ok":true' "$LAP" status --json
expect_grep '"ok":true' "$LAP" session list --json
expect_grep '"ok":false' "$LAP" show L9999 --json

# ---------------------------------------------------- session metadata
t "a session carries metadata, and list filters by it"
mkdir -p "$WORK/meta" && cd "$WORK/meta"
"$LAP" init >/dev/null 2>&1
expect_grep '"id":"S1"' "$LAP" session start "fix the parser" \
    --meta ticket=T-12 --meta epic=E-1 --meta tries=2 --json
expect_grep '"meta":{"ticket":"T-12","epic":"E-1","tries":2}' \
    "$LAP" session current --json
expect_grep '"meta":{"ticket":"T-12","epic":"E-1","tries":2}' \
    cat .lap/log.jsonl
"$LAP" session end >/dev/null 2>&1
"$LAP" session start "unrelated" >/dev/null 2>&1
expect_grep '"meta":{}' "$LAP" session list --json
expect_grep '"id":"S1"' "$LAP" session list --meta ticket=T-12 --json
expect_grep '"id":"S1"' "$LAP" session list --meta tries=2 --json
expect_not_grep '"id":"S1"' "$LAP" session list --meta tries=3 --json
expect_not_grep '"id":"S2"' "$LAP" session list --meta ticket=T-12 --json
expect_grep 'ticket=T-12' "$LAP" session list
expect_grep 'chain ok' "$LAP" verify
expect_ok "$LAP" rebuild
expect_grep '"ticket":"T-12"' "$LAP" session list --json

t "bad metadata is refused before anything is written"
"$LAP" session end >/dev/null 2>&1
expect_grep '"error":"bad_meta"' "$LAP" session start x --meta noequals --json
expect_grep '"error":"bad_meta"' "$LAP" session start x --meta 1up=1 --json
expect_grep '"error":"bad_meta"' "$LAP" session start x --meta a-b=1 --json
expect_grep '"error":"bad_meta"' "$LAP" session start x \
    --meta a=1 --meta a=2 --json
expect_grep 'no active session' "$LAP" session current
cd "$WORK"

# --------------------------------------------------- review requests
t "rr collapses a session into its trajectory and net change"
mkdir -p "$WORK/rr" && cd "$WORK/rr"
"$LAP" init >/dev/null 2>&1
"$LAP" session start "add retry handling" >/dev/null 2>&1
printf 'int fetch(void) {\n    return send();\n}\n' > fetch.c
"$LAP" commit fetch.c -m "the fetcher as it was" >/dev/null 2>&1
printf 'int fetch(void) {\n    int s = send();\n    return s;\n}\n' > fetch.c
"$LAP" commit fetch.c -m "hold the status so we can branch on it" >/dev/null 2>&1
printf 'int fetch(void) {\n    int s = send();\n    if (s == 429) return retry();\n    return s;\n}\n' > fetch.c
"$LAP" commit fetch.c -m "retry on 429: staging returns it under load" >/dev/null 2>&1
printf 'tmp\n' > scratch.txt
"$LAP" commit scratch.txt -m "scratch for the experiment" >/dev/null 2>&1
rm scratch.txt
"$LAP" commit scratch.txt -m "experiment done" >/dev/null 2>&1
"$LAP" session end >/dev/null 2>&1

expect_grep "add retry handling" "$LAP" rr S1
expect_grep "trajectory:" "$LAP" rr S1
expect_grep "retry on 429" "$LAP" rr S1          # the reasoning, in order
expect_grep "net change:" "$LAP" rr S1
expect_grep "429" "$LAP" rr S1                   # the code, collapsed

t "rr shows edits that cancelled out as no net change"
expect_grep "no net change" "$LAP" rr S1

t "rr collapses many commits to one file into one hunk"
# three commits touched fetch.c; the net change must read as a single
# create, not as three successive diffs
HUNKS=$("$LAP" rr S1 | grep -c '@@')
[ "$HUNKS" = "1" ] || fail "expected 1 net hunk, got $HUNKS"

t "rr accepts an inclusive commit range"
expect_grep "L2..L3" "$LAP" rr L2 L3
expect_not_grep "the fetcher as it was" "$LAP" rr L2 L3
expect_grep "hold the status" "$LAP" rr L2 L3

t "rr with no target reviews the most recent session"
expect_grep "add retry handling" "$LAP" rr

t "rr is read-only and machine-readable"
BEFORE_RR=$("$LAP" verify)
"$LAP" rr S1 >/dev/null 2>&1
[ "$("$LAP" verify)" = "$BEFORE_RR" ] || fail "rr changed the repository"
expect_grep '"trajectory"' "$LAP" rr S1 --json
expect_grep '"added"' "$LAP" rr S1 --json
expect_fail "$LAP" rr S99

t "--no-diff drops the hunks but keeps the summary, in both shapes"
expect_not_grep '@@' "$LAP" rr S1 --no-diff
expect_grep "fetch.c" "$LAP" rr S1 --no-diff
expect_not_grep '"diff"' "$LAP" rr S1 --no-diff --json
expect_grep '"added"' "$LAP" rr S1 --no-diff --json
cd "$WORK"

# ------------------------------------------------------------- colour
# Every other scenario in this file reads lap through a pipe, which is
# exactly where colour must not appear — so these checks compare the two
# modes against each other instead of describing either one.
mkdir -p "$WORK/color" && cd "$WORK/color"
"$LAP" init >/dev/null 2>&1
"$LAP" session start "paint the terminal" >/dev/null 2>&1
printf 'alpha\nbeta\n' > tint.txt
"$LAP" commit tint.txt -m "seed the file the colour tests read" >/dev/null 2>&1
printf 'alpha\nBETA\n' > tint.txt
"$LAP" commit tint.txt -m "shout beta so there is a diff to colour" >/dev/null 2>&1
printf 'alpha\nBETA\ngamma\n' > tint.txt

ESC=$(printf '\033')
strip_ansi() { sed "s/${ESC}\[[0-9;]*m//g"; }
has_esc() { printf '%s' "$1" | grep -q "$ESC"; }

# check_color <args...>: a pipe stays plain, --color=never matches it
# byte for byte, --color=always adds escapes, and removing those escapes
# gives the piped bytes back — that last one is the layout guarantee.
check_color() {
    plain=$("$LAP" "$@" 2>&1)
    never=$("$LAP" --color=never "$@" 2>&1)
    always=$("$LAP" --color=always "$@" 2>&1)
    ! has_esc "$plain" || fail "escapes reached a pipe: lap $*"
    [ "$plain" = "$never" ] || fail "--color=never differs from a pipe: lap $*"
    has_esc "$always" || fail "--color=always printed no colour: lap $*"
    [ "$(printf '%s' "$always" | strip_ansi)" = "$plain" ] || \
        fail "colour changed the layout: lap $*"
}

t "colour reaches a terminal, never a pipe, and never moves anything"
check_color status
check_color log
check_color show L1
check_color show L1 --full-file
check_color search --text BETA
check_color search --file tint.txt --line 1
check_color session list
check_color session current
check_color rr
check_color rr --no-diff
check_color verify --deep

t "json is never coloured, in either spelling of an escape"
# a raw ESC cannot survive JSON encoding — it comes out as  — so
# grepping for the byte alone passes while escapes sit in the payload
no_escapes_json() {
    out=$("$LAP" --color=always "$@" 2>&1)
    ! printf '%s' "$out" | grep -q "$ESC" || fail "raw escape in json: lap $*"
    ! printf '%s' "$out" | grep -q 'u001b' || fail "encoded escape: lap $*"
}
no_escapes_json status --json
no_escapes_json log --json
no_escapes_json show L1 --json
no_escapes_json show L1 --full-file --json
no_escapes_json search --text BETA --json
no_escapes_json session list --json
no_escapes_json rr --json
no_escapes_json rr --no-diff --json
no_escapes_json verify --deep --json
no_escapes_json show L9999 --json

t "NO_COLOR overrides even an explicit --color=always"
has_esc "$(NO_COLOR=1 "$LAP" --color=always log 2>&1)" && \
    fail "NO_COLOR did not disable colour"
has_esc "$(NO_COLOR= "$LAP" --color=always log 2>&1)" || \
    fail "an empty NO_COLOR disabled colour"

t "the last colour flag on the line wins"
has_esc "$("$LAP" --color=always --no-color log 2>&1)" && \
    fail "--no-color did not override an earlier --color=always"
has_esc "$("$LAP" --no-color --color=always log 2>&1)" || \
    fail "--color=always did not override an earlier --no-color"

t "errors are coloured for a human and plain for a pipe"
has_esc "$("$LAP" --color=always show L9999 2>&1)" || fail "error uncoloured"
has_esc "$("$LAP" show L9999 2>&1)" && fail "escapes in a piped error"

t "--color rejects a mode that does not exist"
expect_fail "$LAP" --color=purple log
expect_grep "auto, always or never" "$LAP" --color=purple log
expect_grep '"ok":false' "$LAP" --color=purple status --json

t "the colour flag reads the same before or after the command"
[ "$("$LAP" --color=always log 2>&1)" = "$("$LAP" log --color=always 2>&1)" ] \
    || fail "--color behaved differently before and after the command"

t "a value that looks like a flag is data, not a flag"
# these used to abort before the command ran, committing nothing
expect_ok "$LAP" commit tint.txt -m "--color=true was replaced by --color=auto"
printf 'alpha\nBETA\ngamma\ndelta\n' > tint.txt
expect_ok "$LAP" commit tint.txt -m "--no-color is also spelled --color=never"
expect_ok "$LAP" search --text "--color=1"
has_esc "$("$LAP" log -n 1 2>&1)" && fail "a message switched colour on"

t "a path after -- is a path, not a colour flag"
printf 'x\n' > './--color=always'
has_esc "$("$LAP" commit -m "a file whose name looks like a flag" \
    -- --color=always 2>&1)" && fail "a filename switched colour on"
rm -f './--color=always'
cd "$WORK"

# ----------------------------------- control bytes in recorded data
# Recorded history is meant to be shared, so a message, a path or a line of
# a file may have been written by someone else. None of it may emit a raw
# escape when read back, and none of it may silently widen a column.
mkdir -p "$WORK/ctl" && cd "$WORK/ctl"
"$LAP" init >/dev/null 2>&1
"$LAP" session start "a purpose" >/dev/null 2>&1
printf 'line\033[31mone\n' > esc.c
"$LAP" commit esc.c -m "$(printf 'why \033[2J here')" >/dev/null 2>&1

t "recorded data cannot drive the reader's terminal"
for c in "log" "status" "show L1" "show L1 --full-file" \
         "search --text line" "rr --no-diff"; do
    out=$("$LAP" $c 2>&1)
    ! printf '%s' "$out" | grep -q "$ESC" || \
        fail "a raw escape reached the output of: lap $c"
done
# it is shown, not swallowed: caret notation, as a pager renders it
expect_grep '\^\[\[31m' "$LAP" show L1 --full-file
expect_grep '\^\[\[2J' "$LAP" log

# Column alignment needs no separate test: once no invisible byte can reach
# the output, every byte printed occupies the column it is counted for.

t "one deep path does not pad every other row"
mkdir -p "$WORK/ctl/a_very_deeply_nested_directory/with_another_level"
printf 'x\n' > a_very_deeply_nested_directory/with_another_level/deep.txt
printf 'y\n' > s.txt
"$LAP" commit a_very_deeply_nested_directory/with_another_level/deep.txt \
    -m "a deeply nested file" >/dev/null 2>&1
"$LAP" commit s.txt -m "a short one" >/dev/null 2>&1
SHORTW=$("$LAP" rr --no-diff | grep ' s.txt' | head -1 | awk '{print length}')
[ -n "$SHORTW" ] || fail "could not measure the short row"
# the path is 58 columns: uncapped it pads this row past 80, capped to 40
# it lands near 63
[ "$SHORTW" -lt 70 ] || fail "a short row was padded to $SHORTW columns"
cd "$WORK"

# ------------------------------------------------------------ summary
echo "e2e: $TESTS scenarios, $FAILED failure(s)"
[ "$FAILED" -eq 0 ] || exit 1
exit 0
