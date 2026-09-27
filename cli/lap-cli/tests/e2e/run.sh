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

# The history is chunk files under .lap/log/. Appends go to the open chunk
# (the highest-numbered main chunk); history prints every chunk, in order.
open_chunk() { ls .lap/log/main.*.jsonl | LC_ALL=C sort | tail -1; }
history() { cat $(ls .lap/log/main.*.jsonl | LC_ALL=C sort); }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/lap-e2e.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1

# ---------------------------------------------------------------- init
t "init creates the repository"
expect_ok "$LAP" init
[ -d .lap ] || fail ".lap directory missing"
[ -f .lap/log/main.000001.jsonl ] || fail "the first chunk is missing"
[ -e .lap/log.jsonl ] && fail "init created the single-file log"
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
C=$(open_chunk)
cp "$C" .lap/tamper.bak
sed 's/retired after the tests/RETIRED AFTER THE TESTS/' \
    .lap/tamper.bak > "$C"
expect_grep "CHAIN BROKEN" "$LAP" verify
expect_fail "$LAP" verify
mv .lap/tamper.bak "$C"
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
BEFORE=$(history | wc -c)
printf 'check-a\n' > checks.txt
expect_grep "message_too_short" "$LAP" commit checks.txt -i "fix it" -b "adds the check-a line" --json
expect_grep "message_too_short" "$LAP" commit checks.txt -i "cover the message checks" -b "adds it" --json
expect_grep "behavior_repeats_intent" "$LAP" commit checks.txt -i "cover the message checks" -b "cover the message checks" --json
expect_grep "behavior_restates_code" "$LAP" commit checks.txt -i "cover the message checks" -b "check a check" --json
[ "$(history | wc -c)" = "$BEFORE" ] || fail "a refused commit wrote to the log"
expect_ok "$LAP" commit checks.txt -i "cover the message checks" -b "creates the file the checks run against"
printf 'check-a\ncheck-b\n' > checks.txt
expect_grep "behavior_repeats_previous" "$LAP" commit checks.txt -i "cover the message checks" -b "creates the file the checks run against" --json

t "--force-message skips the repetition checks, never the length, and is recorded"
expect_grep "message_too_short" "$LAP" commit checks.txt -i "fix it" -b "b" --force-message --json
expect_ok "$LAP" commit checks.txt -i "cover the message checks" -b "creates the file the checks run against" --force-message
expect_grep '"forced":true' "$LAP" log -n 1 --json
expect_grep "forced" "$LAP" show "$("$LAP" log -n 1 --json | sed 's/.*"id":"\([^"]*\)".*/\1/')"
history | grep -q '"forced":true' || fail "forced is not in the log"
printf 'check-a\ncheck-b\ncheck-c\n' > checks.txt
expect_ok "$LAP" commit checks.txt -i "cover the message checks" -b "appends a third line to exercise a plain commit"
tail -1 "$(open_chunk)" | grep -q '"forced"' && fail "forced written on an unforced commit"

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
    python3 - "$(open_chunk)" <<'PY' || fail "could not forge a collision"
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
l=open('$(open_chunk)','rb').read().split(b'\\n')
print(hashlib.sha256(l[-2]).hexdigest()[:7])")
    expect_grep "ambiguous_ref" "$LAP" show "$PFX" --json
    expect_grep "L1 $PFX, L2 $PFX" "$LAP" show "$PFX"
    cd "$WORK" && rm -rf amb
else
    echo "skip: python3 not found, the ambiguous_ref fixture did not run"
fi

t "unknown flags are refused, naming the flag, before anything runs"
BEFORE=$(history | wc -c)
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
[ "$(history | wc -c)" = "$BEFORE" ] || fail "a refused command wrote to the log"
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
    "$(python3 -c "import hashlib;print(hashlib.sha256(open('$(open_chunk)','rb').read().rstrip(b'\\n')).hexdigest())" 2>/dev/null || echo 0)" >> "$(open_chunk)"
expect_fail "$LAP" log
expect_grep "no intent and behavior" "$LAP" log
cd "$WORK" && rm -rf oldlog

# -------------------------------------------------- crash-safety repairs
t "torn log tail: readers tolerate it, the next writer repairs it"
printf '{"type":"commit","id":"L9' >> "$(open_chunk)"
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
printf '{"type":"commit","id":"L9' >> "$(open_chunk)"
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
C=$(open_chunk)
cp "$C" .lap/tamper.bak
sed 's/quoted-name file landed/QUOTED-NAME FILE LANDED/' \
    .lap/tamper.bak > "$C"
expect_fail "$LAP" rebuild --verify
mv .lap/tamper.bak "$C"
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
    history
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
cp -R .lap/log "$WORK/many-clone/.lap/"
cd "$WORK/many-clone"
sed 's/^line 7 of/LINE 7 OF/' f42.txt > f42.new && mv f42.new f42.txt
# Loading the log once per file grew with files x log size; one load stays
# small. Linux enforces the cap; elsewhere this only checks the answer.
out=$( (ulimit -v 524288 2>/dev/null; "$LAP" status) 2>&1)
printf '%s' "$out" | grep -q "modified  f42.txt" || fail "the edit to f42.txt was not found: $out"
[ "$(printf '%s\n' "$out" | grep -c 'modified')" -eq 1 ] || fail "files other than f42.txt were reported: $out"
[ ! -e .lap/index ] || fail "status, a reader, wrote the index"
cd "$WORK"

t "a CRLF checkout of LF-recorded files is clean, and its edits are recorded as LF"
mkdir -p "$WORK/eol" && cd "$WORK/eol"
"$LAP" init >/dev/null
"$LAP" commit .lapignore --no-session -i "keep the starter ignore list out of the test" \
    -b "records the .lapignore that lap init wrote" >/dev/null 2>&1 || fail "baseline .lapignore"
printf 'one\ntwo\nthree\n' > a.txt
printf 'no final newline' > b.txt
"$LAP" commit a.txt --no-session -i "baseline a.txt for the line-ending test" \
    -b "records a.txt with LF endings" >/dev/null 2>&1 || fail "baseline a.txt"
"$LAP" commit b.txt --no-session -i "baseline b.txt for the line-ending test" \
    -b "records b.txt without a final newline" >/dev/null 2>&1 || fail "baseline b.txt"
# what git writes on Windows with core.autocrlf=true
printf 'one\r\ntwo\r\nthree\r\n' > a.txt
expect_grep "clean" "$LAP" status
printf 'one\r\nTWO\r\nthree\r\n' > a.txt
out=$("$LAP" status)
printf '%s' "$out" | grep -q "a.txt  (1 edit)" || fail "a CRLF edit is not one edit: $out"
"$LAP" commit a.txt --no-session -i "shout two in the CRLF copy" \
    -b "uppercases the second line of a.txt" >/dev/null 2>&1 || fail "commit in the CRLF copy"
tail -1 "$(open_chunk)" | grep -q '\\r' && fail "a CRLF edit was recorded with its CR"
expect_grep "clean" "$LAP" status
expect_ok "$LAP" verify --deep
# a lone CR that no newline ends is content, not a line ending
printf 'no final newline\r' > b.txt
expect_grep "b.txt  (1 edit)" "$LAP" status
cd "$WORK"

# ------------------------------------------------------------- chunks
t "the history seals chunks at the limit and chains across them"
mkdir -p "$WORK/chunks" && cd "$WORK/chunks" || exit 1
export LAP_TEST_CHUNK_BYTES=700
"$LAP" init >/dev/null 2>&1
"$LAP" session start "fill several chunks" >/dev/null 2>&1
i=1
while [ $i -le 6 ]; do
    printf 'row %d\n' $i >> rows.txt
    "$LAP" commit rows.txt -i "grow the history past the chunk limit" \
        -b "appends row $i to rows.txt" >/dev/null 2>&1 || fail "commit row $i"
    i=$((i + 1))
done
N=$(ls .lap/log | grep -c '^main\.[0-9]*\.jsonl$')
[ "$N" -ge 3 ] || fail "expected at least three chunks, found $N"
for c in .lap/log/main.*.jsonl; do
    [ "$(wc -c < "$c")" -le 700 ] || fail "$c is past the limit"
done
# a chunk's first record chains from the previous chunk's last one
python3 - <<'PY' || fail "a chunk does not chain from the one before"
import glob, hashlib, json
prev = None
for c in sorted(glob.glob('.lap/log/main.*.jsonl')):
    lines = open(c, 'rb').read().split(b'\n')[:-1]
    if prev is not None:
        assert json.loads(lines[0])['prev'] == prev, c
    prev = hashlib.sha256(lines[-1]).hexdigest()
PY
expect_grep "chain ok: 8 records" "$LAP" verify
expect_grep "0 mismatch" "$LAP" verify --deep
expect_grep "appends row 1" "$LAP" show L1
rm -f .lap/index
expect_grep "appends row 2" "$LAP" show L2
expect_grep "L6 " "$LAP" log -n 1

t "a record larger than the limit is a chunk of its own"
BEFORE=$(ls .lap/log | wc -l)
python3 -c "print('x' * 900)" > big.txt
expect_ok "$LAP" commit big.txt -i "grow the history past the chunk limit" \
    -b "creates big.txt with one 900-byte line"
C=$(open_chunk)
[ "$(wc -l < "$C")" -eq 1 ] || fail "the large record shares its chunk"
grep -q '"file":"big.txt"' "$C" || fail "the large record is not in the open chunk"
[ "$(ls .lap/log | wc -l)" -gt "$BEFORE" ] || fail "no chunk was started"
expect_grep "chain ok" "$LAP" verify

t "a torn tail in the open chunk is dropped by readers, cut by writers"
printf '{"type":"commit","id":"L99' >> "$(open_chunk)"
expect_ok "$LAP" log
expect_grep "torn trailing record" "$LAP" verify
printf 'row 7\n' >> rows.txt
expect_ok "$LAP" commit rows.txt -i "grow the history past the chunk limit" \
    -b "appends row 7 after a torn append"
expect_not_grep "L99" history
expect_grep "chain ok" "$LAP" verify

t "verify names a sealed chunk that was modified"
cp .lap/log/main.000002.jsonl .lap/sealed.bak
sed 's/appends row/APPENDS ROW/' .lap/sealed.bak > .lap/log/main.000002.jsonl
expect_grep "sealed chunk main.000002.jsonl was modified" "$LAP" verify
expect_fail "$LAP" verify
mv .lap/sealed.bak .lap/log/main.000002.jsonl
expect_grep "chain ok" "$LAP" verify

t "a gap in the chunk numbers is refused, naming the missing chunk"
mv .lap/log/main.000002.jsonl .lap/gap.bak
expect_grep "main.000002.jsonl is missing" "$LAP" log
expect_fail "$LAP" status
mv .lap/gap.bak .lap/log/main.000002.jsonl
expect_ok "$LAP" status
unset LAP_TEST_CHUNK_BYTES
cd "$WORK"

# ------------------------------------------------- the single-file log
t "a single-file log is read as it is and converted by the first write"
mkdir -p "$WORK/legacy" && cd "$WORK/legacy" || exit 1
"$LAP" init >/dev/null 2>&1
"$LAP" session start "a history from before chunks" >/dev/null 2>&1
i=1
while [ $i -le 4 ]; do
    printf 'old %d\n' $i >> old.txt
    "$LAP" commit old.txt -i "build a history to convert" \
        -b "appends old line $i" >/dev/null 2>&1 || fail "commit old $i"
    i=$((i + 1))
done
# the shape an older lap left: one file, no chunk directory
history > .lap/log.jsonl
rm .lap/log/main.*.jsonl && rmdir .lap/log
cp .lap/log.jsonl "$WORK/legacy-before.jsonl"
expect_grep "L4 .*old.txt" "$LAP" log
expect_grep "chain ok: 6 records" "$LAP" verify
expect_grep 'session: S1 "a history from before chunks"' "$LAP" status
expect_grep "appends old line 2" "$LAP" show L2
[ -d .lap/log ] && fail "a reader converted the log"
cmp -s .lap/log.jsonl "$WORK/legacy-before.jsonl" || fail "a reader changed the log"
printf 'new 1\n' >> old.txt
expect_grep "moved .lap/log.jsonl into 1 chunk" "$LAP" commit old.txt \
    -i "build a history to convert" -b "appends the first line after conversion"
[ -e .lap/log.jsonl ] && fail "the single-file log is still there"
[ -f .lap/log/main.000001.jsonl ] || fail "no chunk after the conversion"
# every record converted byte for byte, so every hash is unchanged
history | head -n 6 | cmp -s - "$WORK/legacy-before.jsonl" || \
    fail "the converted history differs from the old log"
[ "$(history | wc -l)" -eq 7 ] || fail "the commit after conversion is missing"
expect_grep "chain ok: 7 records" "$LAP" verify
expect_grep "0 mismatch" "$LAP" verify --deep

t "a large single-file log converts into chunks at the limit"
history > .lap/log.jsonl
rm .lap/log/main.*.jsonl && rmdir .lap/log
cp .lap/log.jsonl "$WORK/legacy-before.jsonl"
expect_grep "into [3-9] chunks" env LAP_TEST_CHUNK_BYTES=600 "$LAP" rebuild
history | cmp -s - "$WORK/legacy-before.jsonl" || \
    fail "the converted history differs from the old log"
for c in .lap/log/main.*.jsonl; do
    [ "$(wc -c < "$c")" -le 600 ] || fail "$c is past the limit"
done
expect_grep "chain ok: 7 records" "$LAP" verify

t "a leftover single-file log beside complete chunks is removed"
cp "$WORK/legacy-before.jsonl" .lap/log.jsonl
expect_ok "$LAP" session end
[ -e .lap/log.jsonl ] && fail "the leftover single-file log is still there"
expect_grep "chain ok: 8 records" "$LAP" verify

t "a single-file log that differs from the chunks is refused, both kept"
printf '{"type":"init","version":1,"ts":"2026-01-01T00:00:00Z","prev":"x"}\n' > .lap/log.jsonl
expect_grep "hold history, and they differ" "$LAP" session start "x y z"
[ -e .lap/log.jsonl ] || fail "the differing log was removed"
rm .lap/log.jsonl
expect_ok "$LAP" status
cd "$WORK"

# ------------------------------------------------------------ branches
# A parent folder with some history, as git and lap both keep it.
branch_parent() {
    mkdir -p "$1" && cd "$1" || exit 1
    git init -q . && git config user.name e2e && git config user.email e2e@lap
    printf '.lap/*\n!.lap/log/\n' > .gitignore
    "$LAP" init >/dev/null 2>&1
    "$LAP" session start "the parent's first work" >/dev/null 2>&1
    printf 'one\ntwo\nthree\n' > f.txt
    "$LAP" commit f.txt -i "seed the parent" -b "creates f.txt with three lines" >/dev/null 2>&1
    "$LAP" commit .lapignore -i "seed the parent" -b "records the starter ignore patterns" >/dev/null 2>&1
    "$LAP" commit .gitignore -i "seed the parent" -b "keeps only the chunk directory of .lap in git" >/dev/null 2>&1
    "$LAP" session end >/dev/null 2>&1
    git add -A && git commit -qm "parent base"
}

if command -v git >/dev/null 2>&1; then
t "branch start in a git worktree: own lineage, sealed parent, no session"
branch_parent "$WORK/bp"
C1=$(cat .lap/log/main.000001.jsonl | shasum)
git worktree add -q "$WORK/bw" -b feat
cd "$WORK/bw" || exit 1
expect_grep "branch feat (.*) started from .*/bp at" "$LAP" branch start feat --from ../bp
ID=$(cat .lap/lineage)
[ ${#ID} -eq 12 ] || fail "no branch id in .lap/lineage"
grep -q '^/.*/bp$' .lap/parent || fail ".lap/parent does not name the parent folder"
[ -f ".lap/log/$ID.000001.jsonl" ] || fail "the branch's first chunk is missing"
head -1 ".lap/log/$ID.000001.jsonl" | grep -q '"type":"branch","id":"'"$ID"'","name":"feat","parent":"main"' || fail "the branch record is not first"
grep -q '"base_chunk":1' ".lap/log/$ID.000001.jsonl" || fail "the base chunk is not 1"
[ -f "$WORK/bp/.lap/log/main.000002.jsonl" ] || fail "the parent's chunk was not sealed"
[ -s "$WORK/bp/.lap/log/main.000002.jsonl" ] && fail "the parent's new chunk is not empty"
[ "$(shasum < "$WORK/bp/.lap/log/main.000001.jsonl")" = "$C1" ] || fail "sealing changed the parent's chunk"
grep -q '"name":"feat"' "$WORK/bp/.lap/branches.json" || fail "the parent does not list the branch"
grep -q '"path":"/[^"]*/bw"' "$WORK/bp/.lap/branches.json" || fail "the registry has the wrong path"
expect_grep "session: none" "$LAP" status
expect_grep "clean" "$LAP" status
expect_grep "chain ok: 7 records" "$LAP" verify
expect_grep "0 mismatch" "$LAP" verify --deep

t "a second branch before the parent appends shares the base, no new seal"
cd "$WORK/bp" || exit 1
mkdir -p "$WORK/bw2" && cp f.txt .lapignore .gitignore "$WORK/bw2/" && cd "$WORK/bw2" || exit 1
expect_ok "$LAP" branch start second --from ../bp
[ -e "$WORK/bp/.lap/log/main.000003.jsonl" ] && fail "an empty open chunk was sealed again"
grep -q '"base_chunk":1' .lap/log/*.000001.jsonl 2>/dev/null || fail "the second branch has another base chunk"
expect_grep "chain ok: 7 records" "$LAP" verify
expect_grep "clean" "$LAP" status
cd "$WORK/bw" || exit 1

t "a branch's ids go on from its base, and both folders commit"
"$LAP" session start "branch work" --branch feat >/dev/null 2>&1
expect_grep "S2" "$LAP" session current
printf 'four\n' >> f.txt
expect_grep "L4 " "$LAP" commit f.txt --branch feat -i "extend on the branch" -b "appends a fourth line"
"$LAP" session end >/dev/null 2>&1
git add -A && git commit -qm "branch work"
cd "$WORK/bp" || exit 1
LAP_BRANCH=main "$LAP" session start "parent work" >/dev/null 2>&1
printf 'g\n' > g.txt
expect_grep "L4 " env LAP_BRANCH=main "$LAP" commit g.txt -i "extend on the parent" -b "creates g.txt on the parent"
"$LAP" session end >/dev/null 2>&1
[ -s .lap/log/main.000002.jsonl ] || fail "the parent did not append to its new chunk"
git add -A && git commit -qm "parent work"

t "git merges a branch into its parent with no conflict in .lap/log/"
git merge -q --no-edit feat >/dev/null 2>&1 || fail "git merge failed"
[ -z "$(git diff --name-only --diff-filter=U)" ] || fail "git reported conflicts: $(git diff --name-only --diff-filter=U)"
[ -f ".lap/log/$ID.000001.jsonl" ] || fail "git did not bring the branch's chunk"
expect_grep "chain ok: 9 records" "$LAP" verify
expect_grep "modified  f.txt" "$LAP" status
git checkout -q -- f.txt 2>/dev/null
git reset -q --hard HEAD~1 2>/dev/null
cd "$WORK/bw" && expect_grep "chain ok: 10 records" "$LAP" verify

t "a plain copy of the parent folder becomes a branch, caches and all"
cp -R "$WORK/bp" "$WORK/bc" && cd "$WORK/bc" || exit 1
expect_ok "$LAP" branch start copied --from ../bp
[ -e .lap/branches.json ] && fail "the copy kept the parent's registry"
expect_grep "clean" "$LAP" status
"$LAP" session start "work in the copy" --branch copied >/dev/null 2>&1
printf 'copy\n' >> f.txt
expect_grep "L5 " "$LAP" commit f.txt --branch copied -i "extend in the copy" -b "appends a line in the copied folder"
expect_grep "0 mismatch" "$LAP" verify --deep
grep -q '"name":"copied"' "$WORK/bp/.lap/branches.json" || fail "the parent does not list the copy"

t "branch start refuses what it cannot make a branch of"
cd "$WORK/bp" || exit 1
mkdir -p "$WORK/bx" && cp f.txt g.txt .lapignore .gitignore "$WORK/bx/" && cd "$WORK/bx" || exit 1
expect_grep "missing_from" "$LAP" branch start --json
expect_grep "same_folder" "$LAP" branch start --from . --json
expect_grep "bad_name" "$LAP" branch start main --from ../bp --json
expect_grep "name_taken" "$LAP" branch start feat --from ../bp --json
expect_grep "no_parent" "$LAP" branch start --from "$WORK/nothing-here" --json
printf 'changed\n' > f.txt
expect_grep "not_clean" "$LAP" branch start --from ../bp --json
expect_grep "f.txt" "$LAP" branch start --from ../bp
expect_grep "git-commit the parent's work first" "$LAP" branch start --from ../bp
[ -e .lap/lineage ] && fail "a refused start left a lineage file"
grep -q '"path":"'"$WORK/bx"'"' "$WORK/bp/.lap/branches.json" && fail "a refused start was registered"
cp "$WORK/bp/f.txt" f.txt
# a history of its own (an init alone can match the parent's byte for byte
# when both were made in the same second)
"$LAP" init >/dev/null 2>&1
"$LAP" commit f.txt --no-session -i "start an unrelated history" -b "records f.txt in a history of its own" >/dev/null 2>&1
expect_grep "unrelated_history" "$LAP" branch start --from ../bp --json
cd "$WORK/bw" && expect_grep "already_branch" "$LAP" branch start --from ../bp --json

t "a folder is not a branch of itself under another spelling of its path"
cd "$WORK/bp" || exit 1
lapsum() { find .lap -type f | sort | xargs cksum; }
BEFORE=$(lapsum)
ln -s bp "$WORK/bp-link"
expect_grep "same_folder" "$LAP" branch start self --from ../bp-link --json
expect_grep "same_folder" "$LAP" branch start self --from ../bx/../bp --json
if [ -d ../BP ]; then # a disk that folds case
    expect_grep "same_folder" "$LAP" branch start self --from ../BP --json
fi
[ "$(lapsum)" = "$BEFORE" ] || fail "a refused start changed .lap"
rm "$WORK/bp-link"

t "where branches exist, commits and session starts say which branch"
cd "$WORK/bw" || exit 1
printf 'five\n' >> f.txt
expect_grep "branch_required" "$LAP" commit f.txt -i "say where it goes" -b "appends a fifth line" --json
expect_grep "with --branch feat" "$LAP" commit f.txt -i "say where it goes" -b "appends a fifth line"
expect_grep "branch_required" "$LAP" commit f.txt -i "say where it goes" -b "appends a fifth line" --dry-run --json
expect_grep "branch_required" "$LAP" session start "unnamed" --json
expect_grep "wrong_branch" "$LAP" commit f.txt --branch main -i "say where it goes" -b "appends a fifth line" --json
expect_grep "records to feat, not other" env LAP_BRANCH=other "$LAP" commit f.txt -i "say where it goes" -b "appends a fifth line"
expect_grep "would record" "$LAP" commit f.txt --branch feat -i "say where it goes" -b "appends a fifth line" --dry-run --no-session
expect_grep "would record" "$LAP" commit f.txt --branch "$(cat .lap/lineage)" -i "say where it goes" -b "appends a fifth line" --dry-run --no-session
# the flag wins over the environment
expect_grep "would record" env LAP_BRANCH=other "$LAP" commit f.txt --branch feat -i "say where it goes" -b "appends a fifth line" --dry-run --no-session
expect_grep "S3 started" env LAP_BRANCH=feat "$LAP" session start "named by the environment"
expect_ok env LAP_BRANCH=feat "$LAP" commit f.txt -i "say where it goes" -b "appends a fifth line"
cd "$WORK/bp" || exit 1
expect_grep "has branches: .*--branch main" "$LAP" session start "unnamed"
expect_grep "wrong_branch" "$LAP" session start "on the parent" --branch feat --json
cd "$WORK/legacy" || exit 1
printf 'plain\n' >> old.txt
expect_grep "wrong_branch" "$LAP" commit old.txt --no-session --branch feat -i "no branches here" -b "appends a plain line" --json
expect_ok "$LAP" commit old.txt --no-session --branch main -i "no branches here" -b "appends a plain line"
printf 'plain 2\n' >> old.txt
expect_ok "$LAP" commit old.txt --no-session -i "no branches here" -b "appends a second plain line"

# A parent ($1-p) and a worktree branch of it ($1-w, git branch and lap
# branch both named b), f.txt holding sixty numbered lines.
merge_pair() {
    mkdir -p "$WORK/$1-p" && cd "$WORK/$1-p" || exit 1
    git init -q . && git config user.name e2e && git config user.email e2e@lap
    printf '.lap/*\n!.lap/log/\n' > .gitignore
    "$LAP" init >/dev/null 2>&1
    i=1; : > f.txt
    while [ $i -le 60 ]; do printf 'line %d\n' $i >> f.txt; i=$((i + 1)); done
    printf 'g1\ng2\ng3\n' > g.txt
    for f in f.txt g.txt .lapignore .gitignore; do
        "$LAP" commit "$f" --no-session -i "seed the merge fixture" -b "records $f as the base" >/dev/null 2>&1
    done
    git add -A && git commit -qm base
    git worktree add -q "$WORK/$1-w" -b b
    cd "$WORK/$1-w" && "$LAP" branch start b --from "../$1-p" >/dev/null 2>&1
}
# in_branch/in_parent <file> <sed script> <behavior>: edit and commit there
in_branch() {
    cd "$BW" && sed "$2" "$1" > "$1.new" && mv "$1.new" "$1" &&
        "$LAP" commit "$1" --branch b -i "work on the branch" -b "$3" >/dev/null 2>&1 ||
        fail "branch commit: $3"
}
in_parent() {
    cd "$BP" && sed "$2" "$1" > "$1.new" && mv "$1.new" "$1" &&
        "$LAP" commit "$1" --branch main --no-session -i "work on the parent" -b "$3" >/dev/null 2>&1 ||
        fail "parent commit: $3"
}
git_merge_b() {
    cd "$BW" && git add -A && git commit -qm "branch work" >/dev/null
    cd "$BP" && git add -A && git commit -qm "parent work" >/dev/null
    git merge -q --no-edit b >/dev/null 2>&1
}

t "lap merge adopts a branch whose work is in other files, all of it"
merge_pair m1; BP="$WORK/m1-p"; BW="$WORK/m1-w"
cd "$BW" && "$LAP" session start "T-7: branch work" --branch b --meta ticket=T-7 >/dev/null 2>&1
in_branch g.txt 's/^g2$/G2/' "uppercases g2 on the branch"
in_branch g.txt '$a\
g4' "appends g4 on the branch"
cd "$BW" && "$LAP" session end >/dev/null 2>&1
in_parent f.txt 's/^line 30$/LINE 30/' "uppercases line 30 on the parent"
git_merge_b || fail "git merge m1"
cd "$BP" && "$LAP" session start "the parent's own session" --branch main >/dev/null 2>&1
BEFORE=$(find .lap -type f | LC_ALL=C sort | xargs shasum)
expect_grep "would adopt 2 of 2" "$LAP" merge b --dry-run
[ "$(find .lap -type f | LC_ALL=C sort | xargs shasum)" = "$BEFORE" ] || fail "a dry run wrote to .lap"
expect_grep "adopted 2 of 2 commits (L6, L7)" "$LAP" merge b
expect_grep "clean" "$LAP" status
expect_grep "the parent's own session" "$LAP" session current
expect_grep "T-7: branch work" "$LAP" session list --meta ticket=T-7
expect_grep '"hash":"[0-9a-f]\{64\}","from":"[0-9a-f]\{64\}","msg":"T-7: branch work"' "$LAP" session list --meta ticket=T-7 --json
expect_grep '"from":"' "$LAP" show L6 --json
expect_grep "^from: #" "$LAP" show L7
expect_grep "appends g4 on the branch (from #" "$LAP" rr S2 --no-diff
expect_grep '"type":"merge","branch":"' history
expect_grep "0 mismatch" "$LAP" verify --deep
expect_grep "nothing new to adopt" "$LAP" merge b
ls .lap/log | grep -q '^[0-9a-f]\{12\}\.000001\.jsonl$' || fail "the branch's chunks were not brought"

t "lap merge moves a branch's function past the parent's import"
merge_pair m2; BP="$WORK/m2-p"; BW="$WORK/m2-w"
cd "$BW" && "$LAP" session start "branch work" --branch b >/dev/null 2>&1
in_branch f.txt '51i\
fn() {\
}' "inserts a function before line 51"
in_parent f.txt '4i\
import x' "inserts an import before line 4"
git_merge_b || fail "git merge m2"
cd "$BP" && expect_grep "adopted 1 of 1" "$LAP" merge b
expect_grep "lines 52-53 (insertion)" "$LAP" log -n 1
expect_grep "clean" "$LAP" status
expect_grep "0 mismatch" "$LAP" verify --deep

t "a conflict stops its file at the right commit; other files are adopted"
merge_pair m3; BP="$WORK/m3-p"; BW="$WORK/m3-w"
cd "$BW" && "$LAP" session start "branch work" --branch b >/dev/null 2>&1
in_branch f.txt 's/^line 50$/branch 50/' "rewrites line 50 on the branch"
in_branch f.txt 's/^line 10$/branch 10/' "rewrites line 10 on the branch"
in_branch f.txt 's/^line 55$/branch 55/' "rewrites line 55 on the branch"
in_branch g.txt 's/^g1$/G1/' "uppercases g1 on the branch"
# newest first: g1, line 55, line 10 — the stop is the line 10 commit
STOP=$("$LAP" log -n 3 --json | tr ',' '\n' | grep '"hash"' | sed -n 3p | sed 's/.*"hash":"\([0-9a-f]*\)".*/\1/')
in_parent f.txt 's/^line 10$/parent 10/' "rewrites line 10 on the parent"
git_merge_b && fail "git merged a real conflict cleanly"
cd "$BP" && sed 's/^<<<<<<<.*$//; s/^=======$//; s/^>>>>>>>.*$//' f.txt | grep -v '^$' > f.res && mv f.res f.txt
grep -q '^branch 50$' f.txt && grep -q '^parent 10$' f.txt || fail "the resolution fixture is wrong"
git add -A && git commit -qm "merge with a resolution" >/dev/null
expect_grep "stopped: f.txt at #$(printf %.7s "$STOP")" "$LAP" merge b --dry-run
expect_grep "adopted 2 of 4" "$LAP" merge b
expect_grep "modified  f.txt" "$LAP" status
expect_not_grep "g.txt" "$LAP" status
grep -q '"stopped":\[{"file":"f.txt","at":"'"$STOP"'"}\]' "$(open_chunk)" || fail "the merge record does not name the stop"
# what is left: the branch's line 10 beside the parent's, and its line 55
expect_grep "(2 edits)" "$LAP" status
expect_ok "$LAP" commit f.txt --branch main --no-session --edit 1 \
    -i "take the branch's work on f.txt" \
    -b "keeps the branch's line 10 beside the parent's, from #$(printf %.7s "$STOP")"
expect_ok "$LAP" commit f.txt --branch main --no-session --edit 1 \
    -i "take the branch's work on f.txt" \
    -b "carries the branch's line 55, which followed the stopped commit"
expect_grep "clean" "$LAP" status
expect_grep "0 mismatch" "$LAP" verify --deep

t "a second merge adopts only what is new, and a stopped file stays stopped"
in_branch g.txt 's/^g3$/G3/' "uppercases g3 on the branch later"
in_branch f.txt 's/^line 20$/branch 20/' "rewrites line 20 on the branch later"
git_merge_b || fail "git merge m3 again"
cd "$BP" && expect_grep "adopted 1 of 2" "$LAP" merge b
expect_grep "^b  *partly merged" "$LAP" branch list
expect_grep "  stopped: f.txt" "$LAP" branch list
expect_grep "uppercases g3 on the branch later" "$LAP" log -n 1 --json
expect_grep "modified  f.txt" "$LAP" status
expect_grep "0 mismatch" "$LAP" verify --deep

t "a plain-copy branch's chunks are copied by lap merge and outlive its folder"
cd "$WORK/m2-p" || exit 1
cp -R "$WORK/m2-p" "$WORK/m5-w" && cd "$WORK/m5-w" || exit 1
rm -rf "$WORK/m5-w/.git" # a plain copy: no git on this side
expect_ok "$LAP" branch start copy --from ../m2-p
ID5=$(cat .lap/lineage)
"$LAP" session start "copy work" --branch copy >/dev/null 2>&1
printf 'from the copy\n' >> g.txt
expect_ok "$LAP" commit g.txt --branch copy -i "work in a plain copy" -b "appends a line to g.txt in the copy"
cp g.txt "$WORK/m2-p/g.txt" # the code comes back by hand: no git here
cd "$WORK/m2-p" || exit 1
[ -e ".lap/log/$ID5.000001.jsonl" ] && fail "the copy's chunk was here before the merge"
expect_grep "adopted 1 of 1" "$LAP" merge copy
[ -e ".lap/log/$ID5.000001.jsonl" ] || fail "lap merge did not copy the branch's chunk"
[ -e "$WORK/m5-w/.lap/log/$ID5.000002.jsonl" ] && fail "lap merge sealed the branch's chunk"
rm -rf "$WORK/m5-w"
expect_grep "clean" "$LAP" status
expect_grep "0 mismatch" "$LAP" verify --deep
expect_grep "nothing new to adopt" "$LAP" merge copy

t "log, show and rr read a branch from its parent, before and after git merge"
merge_pair m6; BP="$WORK/m6-p"; BW="$WORK/m6-w"
cd "$BW" && "$LAP" session start "branch review work" --branch b >/dev/null 2>&1
in_branch g.txt 's/^g1$/review me/' "puts a line to review in g.txt"
cd "$BP" || exit 1
# before git merge: the branch is read from its registered folder
expect_grep '"behavior":"puts a line to review in g.txt".*"branch":"b"' "$LAP" log --branch b --json
expect_not_grep "puts a line to review" "$LAP" log --json
expect_grep "puts a line to review in g.txt" "$LAP" rr --branch b
expect_grep '"msg":"branch review work".*"active":true' "$LAP" session list --branch b --json
expect_not_grep "branch review work" "$LAP" session list
expect_grep "branch: b" "$LAP" show L5 --branch b
expect_grep "unknown_branch" "$LAP" log --branch nope --json
cd "$BW" && expect_not_grep "puts a line" "$LAP" log --branch main
expect_grep '"branch":"main"' "$LAP" log --branch main --json
git_merge_b || fail "git merge m6"
# after git merge, with the branch folder gone: read from the chunks here
mv "$BW" "$WORK/m6-away"
cd "$BP" || exit 1
expect_grep "puts a line to review in g.txt" "$LAP" log --branch b --json
expect_grep "adopted 1 of 1" "$LAP" merge b
FROM=$("$LAP" log -n 1 --json | sed 's/.*"from":"\([0-9a-f]*\)".*/\1/')
[ ${#FROM} -eq 64 ] || fail "the adopted commit has no from link"
expect_grep "branch: b" "$LAP" show "$FROM"
expect_grep '"branch":"b"' "$LAP" show "#$(printf %.7s "$FROM")" --json
expect_not_grep "^branch:" "$LAP" show L5
expect_grep "branch b: chain ok: 8 records" "$LAP" verify
expect_grep '"branches":\[{"branch":"b","records":8,"chain_ok":true}\]' "$LAP" verify --json
BC=$(ls .lap/log/*.000001.jsonl | grep -v '/main\.')
cp "$BC" .lap/branch.bak
# the session start, which the branch's commit chains from
sed 's/branch review work/BRANCH REVIEW WORK/' .lap/branch.bak > "$BC"
expect_grep "branch b: CHAIN BROKEN" "$LAP" verify
expect_fail "$LAP" verify
mv .lap/branch.bak "$BC"
expect_ok "$LAP" verify

t "branch list shows each branch's state, and forget and move tend it"
merge_pair r1; BP="$WORK/r1-p"; BW="$WORK/r1-w"
cd "$BW" && "$LAP" session start "list work" --branch b >/dev/null 2>&1
in_branch g.txt 's/^g1$/listed/' "changes g1 for the list"
for n in c d; do
    cp -R "$BP" "$WORK/r1-$n" && cd "$WORK/r1-$n" &&
        "$LAP" branch start "$n" --from ../r1-p >/dev/null 2>&1 || fail "start $n"
done
cd "$BP" || exit 1
expect_grep "^b  *active  */.*/r1-w\$" "$LAP" branch list
expect_grep "1 commit since its base, 1 since the last merge" "$LAP" branch list
expect_grep '"name":"b","state":"active","present":true' "$LAP" branch list --json
expect_grep '"self":null' "$LAP" branch list --json
cd "$BW" && expect_grep "this folder is branch b (.*) of main" "$LAP" branch list
expect_grep '"self":{"id":"[0-9a-f]*","name":"b","parent":"main"' "$LAP" branch list --json
# an unmerged branch whose folder is deleted is missing, until forgotten
rm -rf "$WORK/r1-c"
cd "$BP" || exit 1
expect_grep "^c  *missing" "$LAP" branch list
expect_grep "lap branch forget c if it is no more" "$LAP" branch list
expect_ok "$LAP" session start "a writer keeps an unmerged gone branch" --branch main
expect_grep "^c  *missing" "$LAP" branch list
expect_grep "forgot branch c" "$LAP" branch forget c
expect_not_grep "^c " "$LAP" branch list
expect_grep "unknown_branch" "$LAP" branch forget c --json
# a moved folder is missing until moved back into the registry
mv "$WORK/r1-d" "$WORK/r1-d-moved"
expect_grep "^d  *missing" "$LAP" branch list
expect_grep "not_that_branch" "$LAP" branch move d "$WORK/r1-w" --json
expect_grep "branch d now at" "$LAP" branch move d "$WORK/r1-d-moved"
expect_grep "^d  *active  */.*/r1-d-moved\$" "$LAP" branch list
# a folder reused for something else is not taken for the branch
mv "$WORK/r1-d-moved" "$WORK/r1-d-old"
mkdir -p "$WORK/r1-d-moved" && (cd "$WORK/r1-d-moved" && "$LAP" init >/dev/null 2>&1)
expect_grep "^d  *missing" "$LAP" branch list
expect_grep '"name":"d","state":"missing","present":false' "$LAP" branch list --json

t "a merged branch whose folder is gone leaves the registry on the next write"
git_merge_b || fail "git merge r1"
cd "$BP" && expect_grep "adopted 1 of 1" "$LAP" merge b
expect_grep "^b  *merged" "$LAP" branch list
rm -rf "$BW"
expect_grep "^b  *merged  *.* (gone)" "$LAP" branch list
OUT=$("$LAP" session end 2>&1)
[ "$OUT" = "session S1 ended" ] || fail "pruning said something: $OUT"
expect_not_grep "^b " "$LAP" branch list
expect_grep "^d  *missing" "$LAP" branch list
expect_grep "0 mismatch" "$LAP" verify --deep

t "sessions are named <branch>/S<n> where ids repeat across folders"
merge_pair q1; BP="$WORK/q1-p"; BW="$WORK/q1-w"
cd "$BW" || exit 1
expect_grep "session b/S1 started" "$LAP" session start "the branch's S1" --branch b
expect_grep '"ref":"b/S1"' "$LAP" session current --json
in_branch g.txt 's/^g2$/branch g2/' "rewrites g2 in the branch's S1"
expect_grep "b/S1 " "$LAP" log -n 1
expect_grep '"session_ref":"b/S1"' "$LAP" log -n 1 --json
expect_grep "^b/S1 " "$LAP" session list
expect_grep "(session b/S1)" "$LAP" show L5
cd "$BP" || exit 1
expect_grep "session S1 started" "$LAP" session start "the parent's S1" --branch main
printf 'p\n' > p.txt
expect_ok "$LAP" commit p.txt --branch main -i "work on the parent" -b "creates p.txt in the parent's S1"
# from the parent, S1 is the parent's and b/S1 the branch's
expect_grep "review S1 — the parent's S1" "$LAP" rr S1
expect_grep "review b/S1 — the branch's S1" "$LAP" rr b/S1
expect_grep "rewrites g2 in the branch's S1" "$LAP" log --session b/S1 --json
expect_not_grep "creates p.txt" "$LAP" log --session b/S1 --json
expect_grep "unknown_session" "$LAP" rr b/S9 --json
expect_grep "unknown_branch" "$LAP" rr nope/S1 --json
"$LAP" session end >/dev/null 2>&1
cd "$BW" && "$LAP" session end >/dev/null 2>&1
git_merge_b || fail "git merge q1"
cd "$BP" && expect_grep "adopted 1 of 1" "$LAP" merge b
# after the merge, b/S1 leads to the session adopted from it, with its from link
expect_grep "review S2 (adopted from b/S1) — the branch's S1" "$LAP" rr b/S1
expect_grep "(from #" "$LAP" rr b/S1 --no-diff
expect_grep '"session":"S2"' "$LAP" log --session b/S1 --json

t "lap merge refuses what it cannot merge"
cd "$BP" && expect_grep "branch_not_found" "$LAP" merge nothing-by-that-name --json
cd "$BW" && expect_grep "merge_in_branch" "$LAP" merge b --json
cd "$WORK"

t "a refused merge leaves both folders' .lap as they were"
merge_pair m15; BP="$WORK/m15-p"; BW="$WORK/m15-w"
rm "$BW/.git" # a plain folder: lap merge would read and copy its chunks
cd "$BW" && "$LAP" session start "work to break" --branch b >/dev/null 2>&1
in_branch g.txt 's/^g1$/broken later/' "a commit after the damaged session start"
ID15=$(cat "$BW/.lap/lineage")
sed '2s/work to break/WORK TO BREAK/' ".lap/log/$ID15.000001.jsonl" > t.new && # the chain breaks after it
    mv t.new ".lap/log/$ID15.000001.jsonl"
both15() { (cd "$BP" && find .lap -type f | sort | xargs cksum; cd "$BW" && find .lap -type f | sort | xargs cksum); }
SUM15=$(both15)
cd "$BP" && expect_grep "log_broken" "$LAP" merge b --json
[ "$(both15)" = "$SUM15" ] || fail "a merge refused as log_broken changed a .lap"
# an unrelated history's branch, its chunk here as a git merge could bring it
mkdir -p "$WORK/m15-u" && cd "$WORK/m15-u" && "$LAP" init >/dev/null 2>&1
printf 'elsewhere\n' > u.txt
for f in u.txt .lapignore; do
    "$LAP" commit "$f" --no-session -i "start another project" -b "records $f there" >/dev/null 2>&1
done
cp -R "$WORK/m15-u" "$WORK/m15-ux" && cd "$WORK/m15-ux" && "$LAP" branch start x --from ../m15-u >/dev/null 2>&1 ||
    fail "unrelated branch start"
cp .lap/log/"$(cat .lap/lineage)".000001.jsonl "$BP/.lap/log/"
cd "$BP" && SUM15=$(both15)
expect_grep "unrelated_history" "$LAP" merge x --json
[ "$(both15)" = "$SUM15" ] || fail "a merge refused as unrelated_history changed a .lap"
cd "$WORK"

t "a branch name stays with one branch, and name/S<n> never means an inherited session"
mkdir -p "$WORK/m18-p" && cd "$WORK/m18-p" || exit 1
git init -q . && git config user.name e2e && git config user.email e2e@lap
printf '.lap/*\n!.lap/log/\n' > .gitignore
"$LAP" init >/dev/null 2>&1
"$LAP" session start "before any branch" >/dev/null 2>&1 # S1, main's own
printf 'f\n' > f.txt
for f in f.txt .lapignore .gitignore; do
    "$LAP" commit "$f" -i "seed the fixture" -b "records $f as the base" >/dev/null 2>&1
done
"$LAP" session end >/dev/null 2>&1
git add -A && git commit -qm base
git worktree add -q "$WORK/m18-w" -b b
cd "$WORK/m18-w" && "$LAP" branch start b --from ../m18-p >/dev/null 2>&1
"$LAP" session start "the branch's work" --branch b >/dev/null 2>&1 # S2
printf 'F\n' > f.txt && "$LAP" commit f.txt --branch b -i "change f in b" -b "rewrites f.txt in the branch" >/dev/null 2>&1
"$LAP" session end --branch b >/dev/null 2>&1
expect_grep "unknown_session" "$LAP" rr b/S1 --json
expect_grep "the branch's work" "$LAP" rr b/S2
git add -A && git commit -qm "b's work" >/dev/null
cd "$WORK/m18-p" && git merge -q --no-edit b >/dev/null 2>&1 || fail "git merge m18"
expect_grep "adopted 1 of 1" "$LAP" merge b
expect_grep "unknown_session" "$LAP" rr b/S1 --json
expect_grep "adopted from b/S2" "$LAP" rr b/S2
git add -A && git commit -qm "took in b" >/dev/null
git worktree remove --force "$WORK/m18-w"
"$LAP" session start "after b" --branch main >/dev/null 2>&1 # a write: b is pruned
grep -q '"name":"b"' .lap/branches.json 2>/dev/null && fail "the merged, gone branch was not pruned"
git worktree add -q "$WORK/m18-w2" -b b-again
cd "$WORK/m18-w2" && expect_grep "name_taken" "$LAP" branch start b --from ../m18-p --json
expect_ok "$LAP" branch start b2 --from ../m18-p
cd "$WORK"

t "folders reached through a symlink work for branch start, branch move and merge"
mkdir -p "$WORK/m19-p" && cd "$WORK/m19-p" && "$LAP" init >/dev/null 2>&1
printf 'f\n' > f.txt
for f in f.txt .lapignore; do
    "$LAP" commit "$f" --no-session -i "seed the fixture" -b "records $f as the base" >/dev/null 2>&1
done
ln -s m19-p "$WORK/m19-link"
cp -R "$WORK/m19-p" "$WORK/m19-w" && cd "$WORK/m19-w" || exit 1
expect_ok "$LAP" branch start x --from ../m19-link
grep -q 'm19-link' "$WORK/m19-p/.lap/branches.json" && fail "the registry kept the symlink's spelling"
cp -R "$WORK/m19-p" "$WORK/m19-w2" && rm -f "$WORK/m19-w2/.lap/branches.json" && cd "$WORK/m19-w2" || exit 1
expect_ok "$LAP" branch start y --from "$WORK/m19-p" # $WORK may itself be under a symlink (/var)
cd "$WORK/m19-w" && "$LAP" session start "x work" --branch x >/dev/null 2>&1
printf 'F\n' > f.txt && "$LAP" commit f.txt --branch x -i "change f in x" -b "rewrites f.txt in x" >/dev/null 2>&1
mv "$WORK/m19-w" "$WORK/m19-w-moved" && ln -s m19-w-moved "$WORK/m19-wlink"
cd "$WORK/m19-p" && expect_ok "$LAP" branch move x ../m19-wlink
cp "$WORK/m19-w-moved/f.txt" f.txt # plain folders: the code comes back by hand
expect_grep "adopted 1 of 1" "$LAP" merge x
expect_grep "clean" "$LAP" status
expect_grep "0 mismatch" "$LAP" verify --deep
cd "$WORK"

t "a branch folder's lineage and parent never reach the parent through git"
mkdir -p "$WORK/m20-p" && cd "$WORK/m20-p" || exit 1
git init -q . && git config user.name e2e && git config user.email e2e@lap
"$LAP" init >/dev/null 2>&1 # no .gitignore of the project's own: lap's guards .lap
[ -f .lap/.gitignore ] || fail "lap init wrote no .lap/.gitignore"
printf 'f\n' > f.txt
for f in f.txt .lapignore; do
    "$LAP" commit "$f" --no-session -i "seed the fixture" -b "records $f as the base" >/dev/null 2>&1
done
git add -A && git commit -qm base
git ls-files .lap | grep -qv '^\.lap/\(\.gitignore\|log/\)' && fail "git tracks more of .lap than log/"
git worktree add -q "$WORK/m20-w" -b b
cd "$WORK/m20-w" && "$LAP" branch start b --from ../m20-p >/dev/null 2>&1
"$LAP" session start "b work" --branch b >/dev/null 2>&1
printf 'F\n' > f.txt && "$LAP" commit f.txt --branch b -i "change f in b" -b "rewrites f.txt in b" >/dev/null 2>&1
git add -A && git commit -qm "b's work" >/dev/null
git ls-files .lap | grep -q 'lineage\|parent' && fail "the branch's identity files went into git"
cd "$WORK/m20-p" && git merge -q --no-edit b >/dev/null 2>&1 || fail "git merge m20"
[ -e .lap/lineage ] && fail "git brought the branch's lineage"
expect_grep '"self":null' "$LAP" branch list --json
expect_grep "adopted 1 of 1" "$LAP" merge b
# the safety net: a lineage and parent that did get here say nothing
cp "$WORK/m20-w/.lap/lineage" "$WORK/m20-w/.lap/parent" .lap/
expect_grep '"self":null' "$LAP" branch list --json
expect_grep "this folder is main" "$LAP" log -n 1
"$LAP" session start "after the leak" --branch main >/dev/null 2>&1
printf 'G\n' >> f.txt
expect_ok "$LAP" commit f.txt --branch main -i "work on main after the leak" -b "appends G to f.txt on main"
expect_grep "0 mismatch" "$LAP" verify --deep
cd "$WORK"

t "an older lap reads a newer history as far as it understands it, and never writes to it"
mkdir -p "$WORK/u24" && cd "$WORK/u24" && "$LAP" init >/dev/null 2>&1
"$LAP" session start "before the newer lap" >/dev/null 2>&1
printf 'a\n' > a.txt
for f in a.txt .lapignore; do
    "$LAP" commit "$f" -i "seed the fixture" -b "records $f as it starts" >/dev/null 2>&1
done
"$LAP" session end >/dev/null 2>&1
CH24=$(ls .lap/log/main.*.jsonl | tail -n 1)
PREV24=$(tail -n 1 "$CH24" | tr -d '\n' | shasum -a 256 | cut -d' ' -f1)
# a record a newer lap would write, chained like any other
printf '{"type":"amend","of":"x","intent":"better","behavior":"clearer","ts":"2026-09-27T00:00:00Z","prev":"%s"}\n' "$PREV24" >> "$CH24"
expect_grep "records a.txt as it starts" "$LAP" log --json
expect_grep 'newer type ("amend")' "$LAP" log
expect_grep "records a.txt as it starts" "$LAP" show L1
expect_grep "before the newer lap" "$LAP" rr S1
expect_grep "clean" "$LAP" status
expect_grep "chain ok: 6 records" "$LAP" verify
expect_grep '1 record of a type this lap does not know ("amend")' "$LAP" verify
printf 'b\n' >> a.txt
SUM24=$(find .lap -type f ! -name lock | sort | xargs cksum)
expect_grep '"error":"newer_history"' "$LAP" commit a.txt --no-session -i "try to write on it" -b "appends b to a.txt" --json
expect_grep "newer_history" "$LAP" session start "try again" --json
[ "$(find .lap -type f ! -name lock | sort | xargs cksum)" = "$SUM24" ] ||
    fail "a refused writer changed .lap"
cd "$WORK"

t "a file tracked before it was ignored shows the same with and without caches"
mkdir -p "$WORK/c23/ign" && cd "$WORK/c23" && "$LAP" init >/dev/null 2>&1
printf 'a\n' > a.txt && printf 'x\n' > ign/x.txt
for f in a.txt ign/x.txt .lapignore; do
    "$LAP" commit "$f" --no-session -i "seed the fixture" -b "records $f" >/dev/null 2>&1
done
printf 'ign/\n' >> .lapignore
"$LAP" commit .lapignore --no-session -i "ignore ign from now on" -b "adds ign/ to .lapignore" >/dev/null 2>&1
nocache23() { # status with every cache gone, then rebuilt
    mkdir -p "$WORK/c23-caches" && for c in index snapshots statcache heads paths; do
        [ -e ".lap/$c" ] && mv ".lap/$c" "$WORK/c23-caches/$c-$1"
    done
    "$LAP" status --json
}
printf 'changed\n' >> ign/x.txt
WITH=$("$LAP" status --json)
expect_grep '"path":"ign/x.txt"' "$LAP" status --json
[ "$(nocache23 m)" = "$WITH" ] || fail "modified: status differs without caches"
"$LAP" rebuild >/dev/null 2>&1
rm ign/x.txt
WITH=$("$LAP" status --json)
expect_grep '"path":"ign/x.txt"' "$LAP" status --json
[ "$(nocache23 d)" = "$WITH" ] || fail "deleted: status differs without caches"
cd "$WORK"

t "readers never see a half-converted history"
mkdir -p "$WORK/v22" && cd "$WORK/v22" && "$LAP" init >/dev/null 2>&1
"$LAP" commit .lapignore --no-session -i "seed the fixture" -b "records .lapignore" >/dev/null 2>&1
i=1
while [ $i -le 150 ]; do
    printf 'line %d\n' $i >> f.txt
    "$LAP" commit f.txt --no-session -i "grow a history to convert" -b "appends line $i to f.txt" >/dev/null 2>&1
    i=$((i + 1))
done
FULL22=$("$LAP" verify | sed -n 's/^chain ok: \([0-9]*\) records.*/\1/p')
[ -n "$FULL22" ] || fail "no record count"
cat .lap/log/main.*.jsonl > .lap/log.jsonl && rm -rf .lap/log # a log from before chunks
legacy22() { # a copy of the old-format folder
    rm -rf "$WORK/v22-$1" && cp -R "$WORK/v22" "$WORK/v22-$1" && cd "$WORK/v22-$1" || exit 1
}
# an older lap's conversion that stopped after two chunks
legacy22 partial && mkdir .lap/log && head -c 700 .lap/log.jsonl | sed '$d' > .lap/log/main.000001.jsonl
expect_grep "chain ok: $FULL22 records" "$LAP" verify
# a crash before this lap published its working folder
legacy22 working && mkdir .lap/log.converting && head -n 3 .lap/log.jsonl > .lap/log.converting/main.000001.jsonl
expect_grep "chain ok: $FULL22 records" "$LAP" verify
"$LAP" session start "after the crash" >/dev/null 2>&1
[ -d .lap/log.converting ] && fail "the working folder of a crashed conversion was left"
[ -e .lap/log.jsonl ] && fail "the conversion did not finish"
expect_grep "chain ok: $((FULL22 + 1)) records" "$LAP" verify
# a crash after publishing, before the old file went
legacy22 published && mkdir .lap/log && cp .lap/log.jsonl .lap/log/main.000001.jsonl
expect_grep "chain ok: $FULL22 records" "$LAP" verify
# live: readers loop while a conversion into many small chunks runs
legacy22 live
( i=0; bad=0; during=0; while [ $i -lt 40 ]; do
    [ -e .lap/log.jsonl ] && during=$((during + 1)) # still converting
    "$LAP" verify >/dev/null 2>&1 || bad=$((bad + 1)); i=$((i + 1))
  done; echo "$bad" > "$WORK/v22-live-bad"; echo "$during" > "$WORK/v22-live-during" ) &
LAP_TEST_CHUNK_BYTES=120 "$LAP" session start "convert under readers" >/dev/null 2>&1 ||
    fail "the conversion under readers failed"
wait
[ "$(cat "$WORK/v22-live-bad")" = 0 ] || fail "$(cat "$WORK/v22-live-bad") of 40 reads during the conversion failed"
[ "$(cat "$WORK/v22-live-during")" -gt 0 ] || fail "no read overlapped the conversion: the loop proved nothing"
expect_grep "chain ok: $((FULL22 + 1)) records" "$LAP" verify
cd "$WORK"

t "a damaged history is named by readers, and no writer builds on it"
mkdir -p "$WORK/d21" && cd "$WORK/d21" && "$LAP" init >/dev/null 2>&1
"$LAP" commit .lapignore --no-session -i "seed the fixture" -b "records .lapignore" >/dev/null 2>&1
for i in 1 2 3 4 5 6; do
    printf 'line %d\n' $i >> f.txt
    LAP_TEST_CHUNK_BYTES=500 "$LAP" commit f.txt --no-session -i "grow the fixture over chunks" \
        -b "appends line $i to f.txt" >/dev/null 2>&1
done
LAST21=$(ls .lap/log | grep '^main' | sort | tail -1)
[ "$LAST21" != "main.000001.jsonl" ] || fail "the fixture is one chunk"
sums21() { find .lap -type f ! -name lock | sort | xargs cksum; }
damaged21() { # $1: what was done; the rest: expected text from verify
    what="$1"; shift
    expect_grep "$1" "$LAP" verify
    printf 'more\n' >> f.txt
    SUM=$(sums21)
    "$LAP" commit f.txt --no-session -i "try to build on damage" -b "appends more to f.txt" >/dev/null 2>&1 &&
        fail "$what: a commit was made on a damaged history"
    [ "$(sums21)" = "$SUM" ] || fail "$what: the refused commit wrote something"
}
cp -R "$WORK/d21" "$WORK/d21-torn" && cd "$WORK/d21-torn" || exit 1
printf '{"type":"commit","partial' >> ".lap/log/$LAST21"
: > ".lap/log/main.$(printf %06d $(( $(echo "$LAST21" | sed 's/main\.0*\([0-9]*\)\.jsonl/\1/') + 1 ))).jsonl"
damaged21 "torn sealed chunk" "$LAST21 ends in a torn line"
cp -R "$WORK/d21" "$WORK/d21-cut" && cd "$WORK/d21-cut" || exit 1
C1=.lap/log/main.000001.jsonl
head -c $(( $(wc -c < "$C1") - 30 )) "$C1" > c1.new && mv c1.new "$C1"
damaged21 "sealed chunk cut short" "main.000001.jsonl ends in a torn line"
cp -R "$WORK/d21" "$WORK/d21-stray" && cd "$WORK/d21-stray" || exit 1
N21=$(ls .lap/log | grep -c '^main')
cp .lap/log/main.000001.jsonl ".lap/log/main.$(printf %06d $((N21 + 1))).jsonl"
damaged21 "stray chunk" "CHAIN BROKEN"
cd "$WORK"

t "an edit to an empty file the parent deleted stops that file"
mkdir -p "$WORK/m17-p" && cd "$WORK/m17-p" || exit 1
git init -q . && git config user.name e2e && git config user.email e2e@lap
printf '.lap/*\n!.lap/log/\n' > .gitignore
"$LAP" init >/dev/null 2>&1
: > e.txt && printf 'f\n' > f.txt
for f in e.txt f.txt .lapignore .gitignore; do
    "$LAP" commit "$f" --no-session -i "seed the merge fixture" -b "records $f as the base" >/dev/null 2>&1
done
git add -A && git commit -qm base
git worktree add -q "$WORK/m17-w" -b b
cd "$WORK/m17-w" && "$LAP" branch start b --from ../m17-p >/dev/null 2>&1
"$LAP" session start "fill e" --branch b >/dev/null 2>&1
printf 'hello\n' > e.txt && "$LAP" commit e.txt --branch b -i "fill the empty file" -b "writes hello into e.txt" >/dev/null 2>&1 ||
    fail "branch edit of e.txt"
git add -A && git commit -qm "fill e" >/dev/null
cd "$WORK/m17-p" && rm e.txt && "$LAP" commit e.txt --branch main --no-session -i "drop the empty file" -b "deletes e.txt on the parent" >/dev/null 2>&1 ||
    fail "parent delete of e.txt"
git add -A && git commit -qm "drop e" >/dev/null
git merge -q --no-edit b >/dev/null 2>&1 # modify/delete: keep the parent's delete
git rm -q e.txt 2>/dev/null; git commit -qm "merged b, e.txt stays deleted" >/dev/null
expect_grep '"stopped":\[{"file":"e.txt","at":"[0-9a-f]*","why":"the parent deleted the file"}\]' "$LAP" merge b --json
[ -e e.txt ] && fail "e.txt came back"
expect_grep "clean" "$LAP" status
expect_grep "0 mismatch" "$LAP" verify --deep
cd "$WORK"

t "a merge stopped after any record is redone exactly by running it again"
merge_pair m16; BP="$WORK/m16-p"; BW="$WORK/m16-w"
cd "$BW" && "$LAP" session start "three edits" --branch b >/dev/null 2>&1
in_branch f.txt 's/^line 2$/TWO/' "edits line 2"
in_branch f.txt 's/^line 5$/FIVE/' "edits line 5"
in_branch f.txt 's/^line 9$/NINE/' "edits line 9"
cd "$BW" && "$LAP" session end --branch b >/dev/null 2>&1
git_merge_b || fail "git merge m16"
# its session, three commits, its end and the merge record: six records
cp -R "$BP" "$WORK/m16-ref" && cd "$WORK/m16-ref" &&
    expect_grep "adopted 3 of 3" "$LAP" merge b
logof() { sed 's/"ts":"[^"]*"//' .lap/log/main.*.jsonl; }
REF16=$(logof)
for n in 0 1 2 3 4 5; do
    cp -R "$BP" "$WORK/m16-c$n" && cd "$WORK/m16-c$n" || exit 1
    LAP_TEST_MERGE_FAIL_AFTER=$n "$LAP" merge b >/dev/null 2>&1 &&
        fail "the merge cut after $n records did not stop"
    expect_grep "adopted 3 of 3" "$LAP" merge b
    [ "$(logof)" = "$REF16" ] || fail "cut after $n records, the rerun's history differs"
    expect_grep "clean" "$LAP" status
    expect_grep "0 mismatch" "$LAP" verify --deep
done
cd "$WORK"

t "lap merge adopts only what git merge brought, so .lap/log never conflicts"
merge_pair m12; BP="$WORK/m12-p"; BW="$WORK/m12-w"
cd "$BW" && "$LAP" session start "staged work" --branch b >/dev/null 2>&1
in_branch f.txt 's/^line 2$/B1/' "B1 edits line 2"
cd "$BW" && git add -A && git commit -qm G1 >/dev/null
cd "$BP" && git merge -q --no-edit b >/dev/null 2>&1 || fail "git merge G1"
in_branch f.txt 's/^line 5$/B2/' "B2 edits line 5"
cd "$BW" && git add -A && git commit -qm G2 >/dev/null
in_branch f.txt 's/^line 9$/B3/' "B3 edits line 9" # not in git yet
cd "$BP" || exit 1
expect_grep "adopted 1 of 1" "$LAP" merge b
expect_grep "clean" "$LAP" status # no B2 or B3 claimed without their code
git add -A && git commit -qm "lap merge of G1" >/dev/null
git merge -q --no-edit b >/dev/null 2>&1 || fail "git merge G2 conflicted"
expect_grep "adopted 1 of 1" "$LAP" merge b
expect_grep "B2 edits line 5" "$LAP" log -n 2 --json
expect_not_grep "B3 edits line 9" "$LAP" log --json
expect_grep "clean" "$LAP" status
expect_grep "0 mismatch" "$LAP" verify --deep
cd "$BW" && expect_grep "0 mismatch" "$LAP" verify --deep
cd "$WORK"

t "identical changes on both sides are already done, and later commits are adopted"
merge_pair m14; BP="$WORK/m14-p"; BW="$WORK/m14-w"
cd "$BW" && "$LAP" session start "same work" --branch b >/dev/null 2>&1
in_branch g.txt 's/^g1$/same/' "makes g1 what the parent makes it"
in_branch g.txt 's/^g3$/later/' "a later branch-only change to g.txt"
cd "$BW" && rm f.txt && "$LAP" commit f.txt --branch b -i "drop f on both sides" \
    -b "deletes f.txt on the branch" >/dev/null 2>&1 || fail "branch delete"
printf 'twin\n' > n.txt && "$LAP" commit n.txt --branch b -i "add n on both sides" \
    -b "creates n.txt on the branch" >/dev/null 2>&1 || fail "branch create"
in_parent g.txt 's/^g1$/same/' "makes g1 what the branch makes it"
cd "$BP" && rm f.txt && "$LAP" commit f.txt --branch main --no-session \
    -i "drop f on both sides" -b "deletes f.txt on the parent" >/dev/null 2>&1 ||
    fail "parent delete"
printf 'twin\n' > n.txt && "$LAP" commit n.txt --branch main --no-session \
    -i "add n on both sides" -b "creates n.txt on the parent" >/dev/null 2>&1 ||
    fail "parent create"
git_merge_b || fail "git merge m14"
expect_grep '"adopted":1,"left":0' "$LAP" merge b --dry-run --json
expect_grep '"stopped":\[\],"already":\["[0-9a-f]*","[0-9a-f]*","[0-9a-f]*"\]' \
    "$LAP" merge b --dry-run --json
expect_grep "already done here: #" "$LAP" merge b
grep -q '"type":"merge".*"already":\["' .lap/log/main.*.jsonl ||
    fail "the merge record does not list the already-done commits"
expect_grep "clean" "$LAP" status
expect_grep "later" cat g.txt
expect_grep "0 mismatch" "$LAP" verify --deep
cd "$WORK"

t "lap merge before git merge is git_merge_first; --copy-from-folder takes the folder's"
merge_pair m13; BP="$WORK/m13-p"; BW="$WORK/m13-w"
cd "$BW" && "$LAP" session start "early work" --branch b >/dev/null 2>&1
in_branch g.txt 's/^g2$/early/' "changes g2 before any git merge"
ID13=$(cat "$BW/.lap/lineage")
cd "$BP" || exit 1
SUM13=$(ls .lap/log; cat .lap/log/*.jsonl | cksum)
expect_grep "git_merge_first" "$LAP" merge b --json
expect_grep "git_merge_first" "$LAP" merge b --dry-run --json
[ "$(ls .lap/log; cat .lap/log/*.jsonl | cksum)" = "$SUM13" ] ||
    fail "git_merge_first wrote something"
expect_grep "would adopt 1 of 1" "$LAP" merge b --dry-run --copy-from-folder
[ -e ".lap/log/$ID13.000001.jsonl" ] && fail "a dry run copied the chunk"
expect_grep "adopted 1 of 1" "$LAP" merge b --copy-from-folder
[ -e ".lap/log/$ID13.000001.jsonl" ] || fail "--copy-from-folder did not copy the chunk"
[ -e "$BW/.lap/log/$ID13.000002.jsonl" ] && fail "lap merge sealed the branch's chunk"
expect_grep "0 mismatch" "$LAP" verify --deep
cd "$WORK"

# main ($NP), lap branch b in the worktree $N1 (git branch b) with one commit
# (line 10 of f.txt), and lap branch b2 started from it in the worktree $N2
# (git branch b2, from b's git head).
nest_trio() {
    merge_pair "$1"; NP="$WORK/$1-p"; N1="$WORK/$1-w"; N2="$WORK/$1-w2"
    cd "$N1" && "$LAP" session start "b work" --branch b >/dev/null 2>&1
    sed 's/^line 10$/B1 early/' f.txt > f.new && mv f.new f.txt &&
        "$LAP" commit f.txt --branch b -i "b1 work before b2" \
            -b "B1 edits line 10 before b2 starts" >/dev/null 2>&1 ||
        fail "b1 early"
    git add -A && git commit -qm "b1 early" >/dev/null
    git worktree add -q "$N2" -b b2
    cd "$N2" && "$LAP" branch start b2 --from "../$1-w" >/dev/null 2>&1 ||
        fail "nested start"
    cd "$N1" && git add -A && git commit -qm "b1 sealed for b2" >/dev/null
}
# edit_commit <folder> <branch> <file> <sed> <behavior>
edit_commit() {
    cd "$1" && sed "$4" "$3" > "$3.new" && mv "$3.new" "$3" &&
        "$LAP" commit "$3" --branch "$2" -i "nested branch work" -b "$5" \
            >/dev/null 2>&1 || fail "commit: $5"
}
once() { # the parent's history holds a behavior exactly once
    n=$("$LAP" log --json | grep -o "$1" | wc -l | tr -d ' ')
    [ "$n" = 1 ] || fail "\"$1\" is in the history $n times, not once"
}

t "a branch of a branch starts from its parent branch and reads its history"
nest_trio n1
cd "$N2" || exit 1
expect_grep "B1 edits line 10" "$LAP" log --json
expect_grep '"branch":"b"' "$LAP" log --json
expect_grep "this folder is branch b2 (.*) of b," "$LAP" branch list
cd "$N1" && expect_grep "^b2 *active" "$LAP" branch list
cd "$NP" && expect_grep "^  b2 .*(from b)" "$LAP" branch list
expect_grep "\"name\":\"b2\".*\"via\":\"[0-9a-f]*\"" "$LAP" branch list --json
cd "$N2" && expect_grep "0 mismatch" "$LAP" verify --deep

t "a branch of a branch merges into its parent branch, and that into main: each commit once"
cd "$N2" && "$LAP" session start "b2 work" --branch b2 >/dev/null 2>&1
edit_commit "$N2" b2 f.txt 's/^line 40$/B2 work/' "B2 edits line 40"
git add -A && git commit -qm "b2 work" >/dev/null
cd "$N1" && git merge -q --no-edit b2 >/dev/null 2>&1 || fail "git merge b2 into b"
expect_grep "adopted 1 of 1" "$LAP" merge b2
expect_grep "clean" "$LAP" status
expect_grep "0 mismatch" "$LAP" verify --deep
git add -A && git commit -qm "b took in b2" >/dev/null
cd "$NP" && git add -A && git commit -qm "main" >/dev/null
git merge -q --no-edit b >/dev/null 2>&1 || fail "git merge b into main"
expect_grep "adopted 2 of 2" "$LAP" merge b
expect_grep "clean" "$LAP" status
once "B1 edits line 10 before b2 starts"
once "B2 edits line 40"
expect_grep "0 mismatch" "$LAP" verify --deep
cd "$WORK"

t "a branch of a branch merges straight into main, bringing its parent branch's work to its base"
nest_trio n2
cd "$N2" && S2REF=$("$LAP" session start "b2 work" --branch b2 2>/dev/null |
    sed -n 's/^session \([^ ]*\) started.*/\1/p')
edit_commit "$N2" b2 f.txt 's/^line 40$/B2 work/' "B2 edits line 40"
git add -A && git commit -qm "b2 work" >/dev/null
edit_commit "$N1" b f.txt 's/^line 50$/B1 later/' "B1 edits line 50 after b2 started"
git add -A && git commit -qm "b1 later" >/dev/null
cd "$NP" && git add -A && git commit -qm "main" >/dev/null
git merge -q --no-edit b2 >/dev/null 2>&1 || fail "git merge b2 into main"
expect_grep '"adopted":2,"left":0' "$LAP" merge b2 --dry-run --json
expect_grep "adopted 2 of 2" "$LAP" merge b2
expect_grep "clean" "$LAP" status
[ "$(grep -c '"type":"merge"' .lap/log/main.*.jsonl | awk -F: '{s+=$NF} END {print s}')" = 2 ] ||
    fail "a merge record for each of b and b2 was expected"
expect_grep "adopted from b2/" "$LAP" rr "$S2REF"
git merge -q --no-edit b >/dev/null 2>&1 || fail "git merge b into main"
expect_grep "adopted 1 of 1" "$LAP" merge b
expect_grep "clean" "$LAP" status
once "B1 edits line 10 before b2 starts"
once "B2 edits line 40"
once "B1 edits line 50 after b2 started"
expect_grep "0 mismatch" "$LAP" verify --deep
cd "$WORK"

t "a conflict in the parent branch's part stops that file for the nested branch too"
nest_trio n3
cd "$N2" && "$LAP" session start "b2 work" --branch b2 >/dev/null 2>&1
edit_commit "$N2" b2 f.txt 's/^line 40$/B2 work/' "B2 edits line 40"
edit_commit "$N2" b2 g.txt 's/^g2$/B2 g/' "B2 edits g2"
git add -A && git commit -qm "b2 work" >/dev/null
cd "$NP" && "$LAP" session start "main work" --branch main >/dev/null 2>&1
edit_commit "$NP" main f.txt 's/^line 10$/MAIN 10/' "main edits line 10 too"
cd "$NP" && git add -A && git commit -qm "main" >/dev/null
git merge -q --no-edit b2 >/dev/null 2>&1 # conflicts on f.txt line 10
[ "$(git diff --name-only --diff-filter=U)" = "f.txt" ] ||
    fail "git should conflict on f.txt alone"
git show :2:f.txt | sed 's/^line 40$/B2 work/' > f.txt
git add f.txt && git commit -qm "merged b2, keeping main's line 10" >/dev/null
expect_grep '"adopted":1,"left":2' "$LAP" merge b2 --json
grep '"type":"merge"' .lap/log/main.*.jsonl | grep -q '"name":"b",.*"stopped":\[{"file":"f.txt"' ||
    fail "b's merge record does not stop f.txt"
grep '"type":"merge"' .lap/log/main.*.jsonl | grep -q '"name":"b2",.*"stopped":\[{"file":"f.txt"' ||
    fail "b2's merge record does not stop f.txt"
expect_grep "0 mismatch" "$LAP" verify --deep
cd "$WORK"

t "a read-only parent refuses the start and nothing is made here"
if [ "$(id -u)" != 0 ]; then
    mkdir -p "$WORK/br" && cp "$WORK/bp/f.txt" "$WORK/bp/g.txt" "$WORK/bp/.lapignore" "$WORK/bp/.gitignore" "$WORK/br/"
    cd "$WORK/bp" && "$LAP" session start "fill the open chunk" --branch main >/dev/null 2>&1 && cd "$WORK/br"
    chmod a-w "$WORK/bp/.lap" "$WORK/bp/.lap/log"
    expect_grep "parent_read_only" "$LAP" branch start --from ../bp --json
    chmod u+w "$WORK/bp/.lap" "$WORK/bp/.lap/log"
    [ -e .lap ] && fail "a refused start made .lap here"
    grep -q '"path":"'"$WORK/br"'"' "$WORK/bp/.lap/branches.json" && fail "a refused start was registered"
else
    echo "skip: running as root, a read-only parent cannot be made"
fi
cd "$WORK"
else
    echo "skip: git not found, the branch scenarios did not run"
fi

# ------------------------------------------------------------ summary
echo "e2e: $TESTS scenarios, $FAILED failure(s)"
[ "$FAILED" -eq 0 ] || exit 1
exit 0
