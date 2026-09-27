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
expect_fail "$LAP" commit notes.txt -i "should be rejected" -b "applies test step 63"
expect_grep "no_session" "$LAP" commit notes.txt -i "x in this test" -b "applies test step 64" --json

t "commit with --no-session works without a session"
printf 'standalone\n' > solo.txt
expect_ok "$LAP" commit solo.txt -i "standalone note, deliberately outside sessions" -b "applies test step 68" --no-session
expect_grep '"session":null' "$LAP" log --json
expect_ok "$LAP" commit .lapignore -i "starter ignore list from lap init" -b "applies test step 70" --no-session

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
expect_ok "$LAP" commit notes.txt -i "seed notes: alpha/beta/gamma baseline" -b "applies test step 87"
expect_grep "create" "$LAP" log
expect_grep "no changes" "$LAP" commit notes.txt -i "nothing changed in this test" -b "applies test step 89"

# ------------------------------------------------- single edit commit
t "single-region change commits without a selector"
printf 'alpha\nBETA\ngamma\n' > notes.txt
expect_ok "$LAP" commit notes.txt -i "shout beta: it is the important one" -b "applies test step 94"
expect_grep "clean" "$LAP" status

# -------------------------------------------------- multi edit workflow
t "two separated edits are rejected and listed"
printf 'ALPHA\nBETA\nGAMMA\n' > notes.txt
expect_fail "$LAP" commit notes.txt -i "two edits at once" -b "applies test step 100"
expect_grep "2 separate edits" "$LAP" commit notes.txt -i "two edits in this test" -b "applies test step 101"
expect_grep "multiple_edits" "$LAP" commit notes.txt -i "x in this test" -b "applies test step 102" --json

t "--edit selects one region; the rest stays pending"
expect_ok "$LAP" commit notes.txt -i "uppercase alpha in this test" -b "applies test step 105" --edit 1
expect_grep "1 edit" "$LAP" status
expect_ok "$LAP" commit notes.txt -i "uppercase gamma in this test" -b "applies test step 107" --edit 1
expect_grep "clean" "$LAP" status

t "--lines must match a detected region exactly"
printf 'ALPHA\nbeta2\nGAMMA\ndelta\n' > notes.txt
expect_fail "$LAP" commit notes.txt -i "bad range in this test" -b "applies test step 112" --lines 1-4
expect_ok "$LAP" commit notes.txt -i "lower beta again" -b "applies test step 113" --lines 2-2
expect_ok "$LAP" commit notes.txt -i "append delta in this test" -b "applies test step 114" --lines 4-4
expect_grep "clean" "$LAP" status

t "--edit out of range is a clean error"
printf 'ALPHA\nbeta3\nGAMMA\ndelta\n' > notes.txt
expect_fail "$LAP" commit notes.txt -i "x in this test" -b "applies test step 119" --edit 7
expect_ok "$LAP" commit notes.txt -i "beta version 3" -b "applies test step 120"

# ------------------------------------------------------------- status
t "status reports states and numbered edits"
printf 'one\ntwo\n' > fresh.txt
printf 'ALPHA\nbeta4\nGAMMA\nDELTA\n' > notes.txt
expect_grep "new       fresh.txt" "$LAP" status
expect_grep "modified  notes.txt" "$LAP" status
expect_grep "\[2\]" "$LAP" status
expect_grep '"state":"new"' "$LAP" status --json
rm fresh.txt
expect_ok "$LAP" commit notes.txt -i "beta version 4" -b "applies test step 131" --edit 1
expect_ok "$LAP" commit notes.txt -i "uppercase delta in this test" -b "applies test step 132" --edit 1

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
expect_ok "$LAP" commit notes.txt -i "tail marker for search tests" -b "applies test step 160"

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
expect_ok "$LAP" commit notes.txt -i "notes.txt retired after the tests" -b "applies test step 177"
expect_grep "clean" "$LAP" status
expect_grep "delete" "$LAP" log -n 1

# ------------------------------------------------------------ ignore
t ".lapignore hides files and blocks commits"
printf '*.log\n' >> .lapignore
printf 'noise\n' > debug.log
expect_not_grep "debug.log" "$LAP" status
expect_fail "$LAP" commit debug.log -i "should be refused" -b "applies test step 186"
expect_ok "$LAP" commit .lapignore -i "ignore build noise: *.log" -b "applies test step 187"

# ------------------------------------------------------ subdirectories
t "files in subdirectories work from repo root and from inside"
mkdir -p src/deep
printf 'content\n' > src/deep/mod.c
expect_ok "$LAP" commit src/deep/mod.c -i "deep module placeholder" -b "applies test step 193"
( cd src/deep && "$LAP" status >/dev/null 2>&1 ) || fail "status from subdir"
( cd src/deep && "$LAP" commit mod.c -i "no change expected" -b "applies test step 195" ) \
    >/dev/null 2>&1 && fail "expected no-changes failure from subdir"

t "paths outside the repository are rejected"
expect_fail "$LAP" commit /etc/hosts -i "outside in this test" -b "applies test step 199"

# ------------------------------------------- trailing-newline handling
t "trailing-newline-only change is one committable edit"
printf 'x\ny' > tail.txt
expect_ok "$LAP" commit tail.txt -i "tail file without trailing newline" -b "applies test step 204"
printf 'x\ny\n' > tail.txt
expect_grep "1 edit" "$LAP" status
expect_ok "$LAP" commit tail.txt -i "add trailing newline for POSIX tools" -b "applies test step 207"
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
expect_ok "$LAP" commit heal0.txt -i "the first writer after state loss persists the healed state" -b "applies test step 221"
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
expect_ok "$LAP" commit blanky.txt -i "blanky baseline: two blocks split by a real anchor line xxx" -b "applies test step 238"
printf 'AAA\n\nBBB\nxxx\nccc\n\nddd\n' > blanky.txt
expect_grep "1 edit" "$LAP" status
expect_ok "$LAP" commit blanky.txt -i "rewrite the first block: blank gap must not split it" -b "applies test step 241"
printf 'AAA\n\nBBB\nxxx\nCCC\n\nDDD\n' > blanky.txt
expect_grep "1 edit" "$LAP" status
expect_ok "$LAP" commit blanky.txt -i "rewrite the second block in one commit too" -b "applies test step 244"
expect_grep "clean" "$LAP" status
expect_grep "0 mismatch" "$LAP" verify --deep

t "edits across a non-blank line still split"
printf 'ZZZ\n\nBBB\nxxx\nCCC\n\nQQQ\n' > blanky.txt
expect_grep "2 edits" "$LAP" status
expect_ok "$LAP" commit blanky.txt -i "first block again" -b "applies test step 251" --edit 1
expect_ok "$LAP" commit blanky.txt -i "last block again" -b "applies test step 252" --edit 1

# ------------------------------------------------- message via -F / stdin
t "message from a file with -F: Intent: and Behavior: sections"
printf 'Intent:\nsummary from file\n\nlong rationale line two\n\nBehavior:\nwrites the fmsg fixture file\n' > msg.tmp
printf 'file-msg-test\n' > fmsg.txt
expect_ok "$LAP" commit fmsg.txt -F msg.tmp
expect_grep "summary from file" "$LAP" log -n 1
# --msg matches the FULL intent (the hit word is on line 3), while the
# result row displays the summary line
expect_grep "summary from file" "$LAP" search --msg "rationale"
expect_grep "long rationale line two" "$LAP" search --msg "rationale" --json
# and the behavior too
expect_grep "summary from file" "$LAP" search --msg "fmsg fixture"
expect_grep "writes the fmsg fixture file" "$LAP" show L$(( $("$LAP" log -n 1 --json | sed 's/.*"id":"L\([0-9]*\)".*/\1/') ))

t "message from stdin with -F -, sections in either order"
printf 'stdin-msg-test\n' > smsg.txt
printf 'Behavior:\npiped behavior text here\nIntent:\npiped summary\npiped detail' | "$LAP" commit smsg.txt -F - \
    >/dev/null 2>&1 || fail "stdin commit failed"
expect_grep "piped summary" "$LAP" log -n 1

t "bad -F files are refused with bad_message_file"
printf 'x\n' > conflict.txt
printf 'preamble\nIntent:\na b c\nBehavior:\nd e f\n' > pre.tmp
expect_grep "bad_message_file" "$LAP" commit conflict.txt -F pre.tmp --json
printf 'Intent:\na b c\n' > half.tmp
expect_grep "bad_message_file" "$LAP" commit conflict.txt -F half.tmp --json
printf 'Intent:\na b c\nIntent:\nx y z\nBehavior:\nd e f\n' > twice.tmp
expect_grep "bad_message_file" "$LAP" commit conflict.txt -F twice.tmp --json
printf 'Intent:\n\nBehavior:\nd e f\n' > hollow.tmp
expect_grep "bad_message_file" "$LAP" commit conflict.txt -F hollow.tmp --json
: > empty.tmp
expect_grep "bad_message_file" "$LAP" commit conflict.txt -F empty.tmp --json
expect_grep "bad_message_file" "$LAP" commit conflict.txt -F no-such.tmp --json

t "-i/-b and -F together are rejected; each field is required"
expect_fail "$LAP" commit conflict.txt -i "a b c" -b "d e f" -F msg.tmp
expect_grep "missing_intent" "$LAP" commit conflict.txt -b "records the conflict file" --json
expect_grep "missing_behavior" "$LAP" commit conflict.txt -i "land the conflict file" --json
expect_grep "missing_intent" "$LAP" commit conflict.txt -i "   " -b "records the conflict file" --json
expect_ok "$LAP" commit conflict.txt --intent "conflict.txt landed with long flags" --behavior "creates conflict.txt holding one x line"

t "commit confirmation echoes the id, short hash and intent summary"
printf 'echo-check\n' > echocheck.txt
expect_grep '^\[L[0-9]* [0-9a-f]\{7\}\] .*"message summary appears in output"' \
    "$LAP" commit echocheck.txt -i "message summary appears in output" -b "creates the echo-check fixture"
expect_grep '"hash":"[0-9a-f]\{64\}"' "$LAP" log -n 1 --json

# ------------------------------------------------------- message checks
t "the message checks refuse weak messages before anything is written"
BEFORE=$(wc -c < .lap/log.jsonl)
printf 'check-a\n' > checks.txt
expect_grep "message_too_short" "$LAP" commit checks.txt -i "fix it" -b "adds the check-a line" --json
expect_grep "message_too_short" "$LAP" commit checks.txt -i "cover the message checks" -b "adds it" --json
expect_grep "behavior_repeats_intent" "$LAP" commit checks.txt -i "cover the message checks" -b "cover the message checks" --json
expect_grep "behavior_restates_code" "$LAP" commit checks.txt -i "cover the message checks" -b "check a check" --json
[ "$(wc -c < .lap/log.jsonl)" = "$BEFORE" ] || fail "a refused commit wrote to the log"
expect_ok "$LAP" commit checks.txt -i "cover the message checks" -b "creates the file the checks run against"
printf 'check-a\ncheck-b\n' > checks.txt
expect_grep "behavior_repeats_previous" "$LAP" commit checks.txt -i "cover the message checks" -b "creates the file the checks run against" --json

t "--force-message skips the repetition checks, never the length, and is recorded"
expect_grep "message_too_short" "$LAP" commit checks.txt -i "fix it" -b "b" --force-message --json
expect_ok "$LAP" commit checks.txt -i "cover the message checks" -b "creates the file the checks run against" --force-message
expect_grep '"forced":true' "$LAP" log -n 1 --json
expect_grep "forced" "$LAP" show "$("$LAP" log -n 1 --json | sed 's/.*"id":"\([^"]*\)".*/\1/')"
grep -q '"forced":true' .lap/log.jsonl || fail "forced is not in the log"
printf 'check-a\ncheck-b\ncheck-c\n' > checks.txt
expect_ok "$LAP" commit checks.txt -i "cover the message checks" -b "appends a third line to exercise a plain commit"
tail -1 .lap/log.jsonl | grep -q '"forced"' && fail "forced written on an unforced commit"

t "--no-session commits skip the previous-behavior check"
printf 'ns1\n' > ns.txt
expect_ok "$LAP" commit ns.txt -i "outside any session" -b "creates the ns fixture file" --no-session
printf 'ns1\nns2\n' > ns.txt
expect_ok "$LAP" commit ns.txt -i "outside any session" -b "creates the ns fixture file" --no-session

# --------------------------------------------------- commit references
t "a commit is found by id, full hash, prefix, with # and in upper case"
HID=$("$LAP" log -n 1 --json | sed 's/.*"id":"\([^"]*\)".*/\1/')
FULL=$("$LAP" log -n 1 --json | sed 's/.*"hash":"\([0-9a-f]*\)".*/\1/')
SHORT=$(printf '%s' "$FULL" | cut -c1-7)
UP=$(printf '%s' "$SHORT" | tr 'a-f' 'A-F')
expect_grep "commit $HID $FULL" "$LAP" show "$HID"
expect_grep "commit $HID " "$LAP" show "$FULL"
expect_grep "commit $HID " "$LAP" show "$SHORT"
expect_grep "commit $HID " "$LAP" show "#$SHORT"
expect_grep "commit $HID " "$LAP" show "$UP"
expect_grep "\"id\":\"$HID\"" "$LAP" show "$SHORT" --json
expect_grep "$HID  *$SHORT" "$LAP" log -n 1

t "bad references: unknown_ref, too short, not hex"
expect_grep "unknown_ref" "$LAP" show 0000000 --json
expect_grep "unknown_ref" "$LAP" show L99999 --json
expect_grep "unknown_ref" "$LAP" show abc12 --json
expect_grep "unknown_ref" "$LAP" show zzzzzzz --json

t "a hash prefix shared by two commits is ambiguous_ref, listing both"
# Seven hex digits almost never collide by chance, so the fixture forges a
# pair: a birthday search over commit records that differ only in a nonce
# finds two whose hashes share their first 7 digits in ~20k tries. They
# are appended to a throwaway repository's log with an arbitrary prev: the
# chain is broken, which readers tolerate, and lookup never consults it.
if command -v python3 >/dev/null 2>&1; then
    mkdir -p "$WORK/amb" && cd "$WORK/amb" || exit 1
    "$LAP" init >/dev/null 2>&1
    python3 - .lap/log.jsonl <<'PY' || fail "could not forge a collision"
import hashlib, sys
def rec(cid, nonce):
    return ('{"type":"commit","id":"%s","session":null,"file":"f.txt",'
            '"op":"create","old_start":1,"old_lines":0,"new_start":1,'
            '"new_lines":0,"eof_nl":true,"old_text":[],"new_text":[],'
            '"intent":"forge a hash prefix collision",'
            '"behavior":"nonce %d of the search","ts":"2026-01-01T00:00:00Z",'
            '"prev":"%s"}' % (cid, nonce, "0" * 64))
pfx = lambda l: hashlib.sha256(l.encode()).hexdigest()[:7]
one, two = {}, {}
n = 0
while True:
    a, b = rec("L1", n), rec("L2", n)
    one[pfx(a)] = a
    two[pfx(b)] = b
    hit = pfx(a) if pfx(a) in two else (pfx(b) if pfx(b) in one else None)
    if hit:
        with open(sys.argv[1], "a") as f:
            f.write(one[hit] + "\n" + two[hit] + "\n")
        break
    n += 1
PY
    PFX=$(python3 -c "
import hashlib
l=open('.lap/log.jsonl','rb').read().split(b'\\n')
print(hashlib.sha256(l[-2]).hexdigest()[:7])")
    expect_grep "ambiguous_ref" "$LAP" show "$PFX" --json
    expect_grep "L1 $PFX, L2 $PFX" "$LAP" show "$PFX"
    cd "$WORK" && rm -rf amb
else
    echo "skip: python3 not found, the ambiguous_ref fixture did not run"
fi

t "unknown flags are refused, naming the flag, before anything runs"
BEFORE=$(wc -c < .lap/log.jsonl)
printf 'uf\n' > uf.txt
expect_grep "unknown_flag" "$LAP" commit uf.txt -m "old spelling" --json
expect_grep "unknown flag -m" "$LAP" commit uf.txt -m "old spelling"
expect_grep "unknown flag -x" "$LAP" commit uf.txt -x "text" -i "a b c" -b "d e f"
expect_grep "unknown_flag" "$LAP" session start "x y z" -m "old" --json
expect_grep "unknown_flag" "$LAP" log --since 2020 --json
expect_grep "unknown_flag" "$LAP" show L1 --bogus --json
expect_grep "unknown_flag" "$LAP" status --verbose --json
expect_grep "unknown_flag" "$LAP" verify --quick --json
expect_grep "unknown_flag" "$LAP" rebuild --force --json
expect_grep "unknown_flag" "$LAP" init --bare --json
expect_grep "unknown_flag" "$LAP" search --msg x --regex --json
expect_grep "unknown_flag" "$LAP" rr --session S1 --json
expect_grep "unknown_flag" "$LAP" rr --from L1 --to L2 --json
[ "$(wc -c < .lap/log.jsonl)" = "$BEFORE" ] || fail "a refused command wrote to the log"
# colour flags belong to lap and are accepted by every command
expect_ok "$LAP" log -n 1 --color=never
expect_ok "$LAP" status --no-color
rm -f uf.txt

t "session start takes its purpose from -F, not both"
"$LAP" session end >/dev/null 2>&1
printf 'purpose from a file\n\nwith detail\n' > purpose.tmp
expect_grep "purpose from a file" "$LAP" session start -F purpose.tmp
"$LAP" session end >/dev/null 2>&1
expect_fail "$LAP" session start "an argument" -F purpose.tmp
printf 'piped purpose\n' | "$LAP" session start -F - >/dev/null 2>&1 \
    || fail "session start -F - failed"
expect_grep "piped purpose" "$LAP" session current
"$LAP" session end >/dev/null 2>&1
expect_ok "$LAP" session start "real work, resumed"

t "a log holding a msg-only commit is refused"
mkdir -p "$WORK/oldlog" && cd "$WORK/oldlog" || exit 1
"$LAP" init >/dev/null 2>&1
printf '{"type":"commit","id":"L1","session":null,"file":"f.txt","op":"create","old_start":1,"old_lines":0,"new_start":1,"new_lines":0,"eof_nl":true,"old_text":[],"new_text":[],"msg":"an old message","ts":"2026-01-01T00:00:00Z","prev":"%s"}\n' \
    "$(python3 -c "import hashlib;print(hashlib.sha256(open('.lap/log.jsonl','rb').read().rstrip(b'\\n')).hexdigest())" 2>/dev/null || echo 0)" >> .lap/log.jsonl
expect_fail "$LAP" log
expect_grep "no intent and behavior" "$LAP" log
cd "$WORK" && rm -rf oldlog

# -------------------------------------------------- crash-safety repairs
t "torn log tail: readers tolerate it, the next writer repairs it"
printf '{"type":"commit","id":"L9' >> .lap/log.jsonl
expect_ok "$LAP" log
expect_grep "torn trailing record" "$LAP" verify
printf 'torn-recovery\n' > torn.txt
expect_ok "$LAP" commit torn.txt -i "commit after a crash-torn append: the writer truncates the torn bytes first" -b "applies test step 289"
expect_grep "chain ok" "$LAP" verify
expect_grep "0 mismatch" "$LAP" verify --deep

t "verify --deep detects a missing shadow file"
rm .lap/shadow/torn.txt
expect_fail "$LAP" verify --deep
expect_grep "missing shadow" "$LAP" verify --deep

t "a writing command heals lost state.json and rebuilds shadows"
rm .lap/state.json
printf 'heal-me\n' > healfile.txt
expect_ok "$LAP" commit healfile.txt -i "this write triggers a full heal first" -b "applies test step 301"
expect_grep "chain ok" "$LAP" verify
expect_grep "0 mismatch" "$LAP" verify --deep

# ------------------------------------ partial-commit coordinate integrity
t "partial commit stores committed-file coordinates; blame stays correct"
printf 'p1\np2\np3\np4\np5\n' > coord.txt
expect_ok "$LAP" commit coord.txt -i "coord baseline in this test" -b "applies test step 308"
printf 'TOP\nTOPB\np1\np2\np3\nP4\np5\n' > coord.txt
CID=$("$LAP" commit coord.txt -i "uppercase p4 (committed before the top insertion)" -b "applies test step 310" --edit 2 --json | sed 's/.*"id":"\([^"]*\)".*/\1/')
[ -n "$CID" ] || fail "could not extract the partial commit id"
expect_grep "last touched by $CID" "$LAP" search --file coord.txt --line 6
expect_ok "$LAP" commit coord.txt -i "top insertion, committed second" -b "applies test step 313"
expect_grep "last touched by $CID" "$LAP" search --file coord.txt --line 6

t "blame on an uncommitted new file reports pending"
printf 'n1\nn2\n' > newpend.txt
expect_grep "pending" "$LAP" search --file newpend.txt --line 1
expect_grep '"pending":true' "$LAP" search --file newpend.txt --line 2 --json
expect_ok "$LAP" commit newpend.txt -i "newpend baseline in this test" -b "applies test step 320"

# ----------------------------------------------------- argument hygiene
t "a message that looks like a flag is a message, not a flag"
printf 'f1\n' > flagmsg.txt
expect_ok "$LAP" commit flagmsg.txt -i "--no-session --json --edit" -b "--lines --force-message --bogus"
expect_grep "no-session --json --edit" "$LAP" log -n 1
expect_grep '"session":"S' "$LAP" log -n 1 --json

t "-- ends flags: files named like flags are committable"
printf 'd1\n' > ./-dash.txt
expect_ok "$LAP" commit -i "dash-named file survives the parser" -b "applies test step 330" -- -dash.txt
expect_grep "dash-named" "$LAP" log -n 1

t "paths with quotes stay valid JSON in blame output"
printf 'q1\n' > 'q"uote.txt'
expect_grep 'q\\"uote.txt' "$LAP" search --file 'q"uote.txt' --line 1 --json
expect_ok "$LAP" commit 'q"uote.txt' -i "quoted-name file landed" -b "applies test step 336"

# ------------------------------------------------------------- user field
t "commits record the user (LAP_USER wins the resolution)"
printf 'u1\n' > userfile.txt
LAP_USER="e2e-test-bot" "$LAP" commit userfile.txt -i "user field: recorded from LAP_USER" -b "applies test step 341"
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
for cache in index paths heads state.json snapshots shadow statcache; do
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
expect_ok "$LAP" commit stale.txt -i "commit that grows the path table" -b "applies test step 395"
cp .lap/heads.old .lap/heads && cp .lap/paths.old .lap/paths   # rewind sidecars
OUT=$("$LAP" search --file stale.txt --line 1 2>&1)
rm -f .lap/heads.old .lap/paths.old
expect_ok "$LAP" rebuild
EXPECT=$("$LAP" search --file stale.txt --line 1 2>&1)
[ "$OUT" = "$EXPECT" ] || fail "stale sidecars changed blame: [$OUT] vs [$EXPECT]"

t "commit ids are verified, not just parsed"
expect_fail "$LAP" show L1x
expect_grep "unknown_ref" "$LAP" show L1x --json
rm -f .lap/index
expect_grep "unknown_ref" "$LAP" show L1x --json
expect_grep "unknown_ref" "$LAP" show L1000000 --json
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

t "commit --dry-run shows the record, fails as the commit would, and writes nothing"
mkdir -p "$WORK/dry" && cd "$WORK/dry" || exit 1
"$LAP" init >/dev/null 2>&1
"$LAP" session start "dry run fixture" >/dev/null 2>&1
lapsum() { find .lap -type f | LC_ALL=C sort | xargs shasum; }
printf 'a\nb\nc\n' > d.txt
BEFORE=$(lapsum)
expect_grep "would record L1 in S1" "$LAP" commit d.txt -i "seed the dry run fixture" -b "creates d.txt with three lines" --dry-run
expect_grep '"dry_run":true,"record":{"type":"commit","id":"L1"' "$LAP" commit d.txt -i "seed the dry run fixture" -b "creates d.txt with three lines" --dry-run --json
expect_grep '"intent":"seed the dry run fixture","behavior":"creates d.txt with three lines"' "$LAP" commit d.txt -i "seed the dry run fixture" -b "creates d.txt with three lines" --dry-run --json
expect_not_grep '"prev"\|"hash"' "$LAP" commit d.txt -i "seed the dry run fixture" -b "creates d.txt with three lines" --dry-run --json
# the same refusals as a real commit
expect_grep "message_too_short" "$LAP" commit d.txt -i "seed it" -b "creates d.txt with three lines" --dry-run --json
expect_grep "behavior_repeats_intent" "$LAP" commit d.txt -i "seed the dry run fixture" -b "seed the dry run fixture" --dry-run --json
expect_grep "unknown_file" "$LAP" commit nope.txt -i "seed the dry run fixture" -b "creates d.txt with three lines" --dry-run --json
[ "$(lapsum)" = "$BEFORE" ] || fail "a dry run changed .lap/"
# the id it predicts is the one the commit then gets
PRED=$("$LAP" commit d.txt -i "seed the dry run fixture" -b "creates d.txt with three lines" --dry-run --json | sed 's/.*"id":"\(L[0-9]*\)".*/\1/')
REAL=$("$LAP" commit d.txt -i "seed the dry run fixture" -b "creates d.txt with three lines" --json | sed 's/.*"id":"\(L[0-9]*\)".*/\1/')
[ -n "$PRED" ] && [ "$PRED" = "$REAL" ] || fail "dry run predicted $PRED, commit got $REAL"
expect_grep "no_changes" "$LAP" commit d.txt -i "seed the dry run fixture" -b "changes d.txt again" --dry-run --json
printf 'A\nb\nC\n' > d.txt
expect_grep "multiple_edits" "$LAP" commit d.txt -i "shout the edges" -b "uppercases the first line" --dry-run --json
expect_grep "would record L2 in S1" "$LAP" commit d.txt -i "shout the edges" -b "uppercases the first line" --dry-run --edit 1
expect_grep "behavior_repeats_previous" "$LAP" commit d.txt -i "shout the edges" -b "creates d.txt with three lines" --dry-run --edit 1 --json
# a dry run is a reader: it repairs nothing it finds broken
rm .lap/state.json
printf '{"type":"commit","id":"L9' >> .lap/log.jsonl
BEFORE=$(lapsum)
expect_grep "would record L2 in S1" "$LAP" commit d.txt -i "shout the edges" -b "uppercases the first line" --dry-run --edit 1
[ "$(lapsum)" = "$BEFORE" ] || fail "a dry run repaired the repository"
cd "$WORK" && rm -rf dry

t "times: stored in UTC, shown in local time, filtered as people write them"
mkdir -p "$WORK/tz" && cd "$WORK/tz" || exit 1
"$LAP" init >/dev/null 2>&1
"$LAP" session start "time zone fixture" >/dev/null 2>&1
printf 'x\n' > t.txt
"$LAP" commit t.txt -i "seed the time zone fixture" -b "creates t.txt with one line" >/dev/null 2>&1
UTC=$("$LAP" log -n 1 --json | sed 's/.*"ts":"\([^"]*\)".*/\1/')
case "$UTC" in *T*Z) ;; *) fail "the log's JSON time is not UTC ISO: $UTC" ;; esac
# Tokyo has no daylight saving, so its offset is always +09:00
SHOWN=$(TZ=Asia/Tokyo "$LAP" log -n 1 | head -1 | awk '{print $3" "$4}')
expect_grep "date: $SHOWN +09:00" env TZ=Asia/Tokyo "$LAP" show L1
TZ=UTC "$LAP" log -n 1 | grep -q "$(printf '%s' "$UTC" | tr 'T' ' ' | tr -d Z)" \
    || fail "in UTC the shown time is not the stored one"
# what the output shows is what a filter takes
expect_grep '"id":"L1"' env TZ=Asia/Tokyo "$LAP" search --since "$SHOWN" --json
expect_grep '"id":"L1"' env TZ=Asia/Tokyo "$LAP" search --until "$SHOWN" --json
expect_grep '"id":"L1"' env TZ=Asia/Tokyo "$LAP" search --since "$UTC" --json
expect_grep '"commits":\[\]' env TZ=UTC "$LAP" search --since "$SHOWN" --json
expect_grep "bad_time" "$LAP" search --since yesterday --json
cd "$WORK" && rm -rf tz

t "status keeps a stat cache and trusts it only for settled, unchanged files"
mkdir -p "$WORK/stat" && cd "$WORK/stat" || exit 1
"$LAP" init >/dev/null 2>&1
"$LAP" session start "stat cache fixture" >/dev/null 2>&1
printf 'one\ntwo\n' > kept.txt
printf 'alpha\nbeta\n' > same.txt
printf 'fresh\n' > racy.txt
for f in kept.txt same.txt racy.txt .lapignore; do
    "$LAP" commit "$f" -i "seed the stat cache fixture" -b "records $f as it was written" >/dev/null 2>&1
done
# settled: modified long before any status run; racy.txt is dated in the
# future, so it is never older than a run and must never be cached
touch -t 202001010000 kept.txt same.txt .lapignore
touch -t 209901010000 racy.txt
expect_grep "clean" "$LAP" status
[ -f .lap/statcache ] || fail "status wrote no stat cache"
grep -q ' kept.txt$' .lap/statcache || fail "a settled clean file has no entry"
grep -q ' racy.txt$' .lap/statcache && fail "a file not older than the run was cached"
# served from the cache, not read: new bytes, same size, the old mtime put
# back, and status still calls it clean — until the cache is gone
printf 'ONE\nTWO\n' > kept.txt && touch -t 202001010000 kept.txt
expect_grep "clean" "$LAP" status
rm .lap/statcache
expect_grep "modified  kept.txt" "$LAP" status
printf 'one\ntwo\n' > kept.txt && touch -t 202001010000 kept.txt
"$LAP" status >/dev/null 2>&1
# an edit that keeps the size is found: the mtime moved
printf 'ALPHA\nbeta\n' > same.txt
expect_grep "modified  same.txt" "$LAP" status
printf 'alpha\nbeta\n' > same.txt && touch -t 202001010000 same.txt
# a racy file's edit is found even with its size and mtime unchanged
printf 'FRESH\n' > racy.txt && touch -t 209901010000 racy.txt
expect_grep "modified  racy.txt" "$LAP" status
printf 'fresh\n' > racy.txt && touch -t 209901010000 racy.txt
# a commit moves the file's head, so its old entry no longer applies
printf 'one\ntwo\nthree\n' > kept.txt
"$LAP" commit kept.txt -i "seed the stat cache fixture" -b "appends a third line to kept.txt" >/dev/null 2>&1
touch -t 202001010000 kept.txt
expect_grep "clean" "$LAP" status
printf 'one\ntwo\nTHREE\n' > kept.txt && touch -t 202001010000 kept.txt
rm .lap/statcache
expect_grep "modified  kept.txt" "$LAP" status
printf 'one\ntwo\nthree\n' > kept.txt && touch -t 202001010000 kept.txt
# a deleted tracked file is found through the index, with no shadow walk
rm same.txt
expect_grep "deleted   same.txt" "$LAP" status
rm .lap/index
expect_grep "deleted   same.txt" "$LAP" status
"$LAP" rebuild >/dev/null 2>&1
printf 'alpha\nbeta\n' > same.txt && touch -t 202001010000 same.txt

t "status refreshes the stat cache only when the lock is free"
if command -v python3 >/dev/null 2>&1; then
    rm -f .lap/statcache
    # hold the writer lock while status runs: output unchanged, no cache
    python3 -c '
import fcntl, subprocess, sys
f = open(".lap/lock", "w")
fcntl.flock(f, fcntl.LOCK_EX)
sys.exit(subprocess.run([sys.argv[1], "status"], capture_output=True).returncode)
' "$LAP" || fail "status failed while a writer held the lock"
    [ -f .lap/statcache ] && fail "status wrote the stat cache under a held lock"
    expect_grep "clean" "$LAP" status
    [ -f .lap/statcache ] || fail "status wrote no stat cache once the lock was free"
else
    echo "skip: python3 not found, the held-lock case did not run"
fi
cd "$WORK" && rm -rf stat

t "the cache contract: rm every cache, readers still work, rebuild restores"
rm -rf .lap/shadow .lap/snapshots .lap/state.json \
       .lap/index .lap/paths .lap/heads .lap/statcache
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
expect_ok "$LAP" commit snapfile.txt -i "snapshot probe baseline" -b "applies test step 451"
i=0
while [ $i -lt 20 ]; do
    printf 'snap base edited %s\n' "$i" > snapfile.txt
    "$LAP" commit snapfile.txt -i "snapshot probe edit $i" -b "applies test step 455 round $i" >/dev/null \
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
"$LAP" commit fetch.c -i "the fetcher as it was" -b "creates fetch.c returning send() directly" >/dev/null 2>&1
printf 'int fetch(void) {\n    int s = send();\n    return s;\n}\n' > fetch.c
"$LAP" commit fetch.c -i "retry on 429: staging returns it under load" -b "hold the status so we can branch on it" >/dev/null 2>&1
printf 'int fetch(void) {\n    int s = send();\n    if (s == 429) return retry();\n    return s;\n}\n' > fetch.c
"$LAP" commit fetch.c -i "retry on 429: staging returns it under load" -b "calls retry() when send() answered 429" >/dev/null 2>&1
printf 'tmp\n' > scratch.txt
"$LAP" commit scratch.txt -i "scratch for the experiment" -b "applies test step 532" >/dev/null 2>&1
rm scratch.txt
"$LAP" commit scratch.txt -i "experiment done in this test" -b "applies test step 534" >/dev/null 2>&1
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

t "rr groups consecutive commits under their shared intent"
[ "$("$LAP" rr S1 --no-diff | grep -c 'retry on 429')" = "1" ] \
    || fail "a shared intent was printed more than once"
expect_grep "L2 .*hold the status" "$LAP" rr S1
expect_grep "L3 .*calls retry()" "$LAP" rr S1
[ "$("$LAP" rr S1 --json | grep -o '"intent":' | wc -l | tr -d ' ')" = "5" ] \
    || fail "the JSON trajectory is not one entry per commit"

t "rr accepts an inclusive commit range, by id or by hash"
expect_grep "L2..L3" "$LAP" rr L2 L3
expect_not_grep "the fetcher as it was" "$LAP" rr L2 L3
expect_grep "hold the status" "$LAP" rr L2 L3
H2=$("$LAP" show L2 --json | sed 's/.*"hash":"\([0-9a-f]\{7\}\).*/\1/')
H3=$("$LAP" show L3 --json | sed 's/.*"hash":"\([0-9a-f]*\)".*/\1/')
expect_grep "calls retry()" "$LAP" rr "#$H2" "$H3"
expect_not_grep "the fetcher as it was" "$LAP" rr "#$H2" "$H3"
expect_grep "unknown_ref" "$LAP" rr L2 L99 --json
expect_grep "empty_range" "$LAP" rr L3 L2 --json

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
"$LAP" commit tint.txt -i "seed the file the colour tests read" -b "applies test step 583" >/dev/null 2>&1
printf 'alpha\nBETA\n' > tint.txt
"$LAP" commit tint.txt -i "shout beta so there is a diff to colour" -b "applies test step 585" >/dev/null 2>&1
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
expect_ok "$LAP" commit tint.txt -i "--color=true was replaced by --color=auto" -b "applies test step 665"
printf 'alpha\nBETA\ngamma\ndelta\n' > tint.txt
expect_ok "$LAP" commit tint.txt -i "--no-color is also spelled --color=never" -b "applies test step 667"
expect_ok "$LAP" search --text "--color=1"
has_esc "$("$LAP" log -n 1 2>&1)" && fail "a message switched colour on"

t "a path after -- is a path, not a colour flag"
printf 'x\n' > './--color=always'
has_esc "$("$LAP" commit -i "a file whose name looks like a flag" -b "applies test step 673" \
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
"$LAP" commit esc.c -i "$(printf 'why \033[2J here')" -b "applies test step 686" >/dev/null 2>&1

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
    -i "a deeply nested file" -b "applies test step 707" >/dev/null 2>&1
"$LAP" commit s.txt -i "a short one" -b "applies test step 708" >/dev/null 2>&1
SHORTW=$("$LAP" rr --no-diff | grep ' s.txt' | head -1 | awk '{print length}')
[ -n "$SHORTW" ] || fail "could not measure the short row"
# the path is 58 columns: uncapped it pads this row past 100, capped to 40
# it lands near 83
[ "$SHORTW" -lt 90 ] || fail "a short row was padded to $SHORTW columns"
cd "$WORK"

t "status over many files with only the log replays each shadow from one load"
mkdir -p "$WORK/many" && cd "$WORK/many"
"$LAP" init >/dev/null
i=0
while [ $i -lt 120 ]; do
    awk -v n=$i 'BEGIN { for (j = 1; j <= 150; j++) print "line " j " of file " n " with some text" }' > f$i.txt
    "$LAP" commit f$i.txt --no-session -i "baseline file $i for the many-files test" \
        -b "records all of f$i.txt as its first commit" >/dev/null 2>&1 || fail "baseline f$i.txt"
    i=$((i + 1))
done
# a clone carries the log and the working tree, and no cache
mkdir -p "$WORK/many-clone/.lap"
cp f*.txt "$WORK/many-clone/"
cp .lap/log.jsonl "$WORK/many-clone/.lap/"
cd "$WORK/many-clone"
sed 's/^line 7 of/LINE 7 OF/' f42.txt > f42.new && mv f42.new f42.txt
# Loading the log once per file grew with files x log size; one load stays
# small. Linux enforces the cap; elsewhere this only checks the answer.
out=$( (ulimit -v 524288 2>/dev/null; "$LAP" status) 2>&1)
printf '%s' "$out" | grep -q "modified  f42.txt" || fail "the edit to f42.txt was not found: $out"
[ "$(printf '%s\n' "$out" | grep -c 'modified')" -eq 1 ] || fail "files other than f42.txt were reported: $out"
[ ! -e .lap/index ] || fail "status, a reader, wrote the index"
cd "$WORK"

# ------------------------------------------------------------ summary
echo "e2e: $TESTS scenarios, $FAILED failure(s)"
[ "$FAILED" -eq 0 ] || exit 1
exit 0
