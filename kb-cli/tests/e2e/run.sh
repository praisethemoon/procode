#!/bin/sh
# End-to-end tests: drive the real kb binary through full workflows in a
# throwaway directory. Requires KB=<absolute path to the kb binary> and
# KB_TESTS=<absolute path to the unit-test binary>, which doubles as a
# lock holder so cross-process locking can be tested for real.
set -u
KB="${KB:?set KB to the kb binary path}"
KB_TESTS="${KB_TESTS:-}"

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
# expect_code <n> <cmd...>: the command must exit with exactly this status
expect_code() {
    want="$1"; shift
    "$@" >/dev/null 2>&1
    got=$?
    if [ "$got" -ne "$want" ]; then
        fail "expected exit $want, got $got: $*"
    fi
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
# in_str <haystack> <pattern>
in_str() { printf '%s\n' "$1" | grep -q "$2"; }
has() {
    if ! in_str "$2" "$3"; then
        fail "$1: expected /$3/ in: $2"
    fi
}
hasnt() {
    if in_str "$2" "$3"; then
        fail "$1: expected NO /$3/ in: $2"
    fi
}
# first string value of a JSON key
jstr() { printf '%s' "$2" | sed -n "s/.*\"$1\":\"\\([^\"]*\\)\".*/\\1/p"; }
sha_of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        echo unavailable
    fi
}

# "ok" when every byte of $1 belongs to a complete UTF-8 sequence. Written
# with od and awk so it needs nothing beyond what the rest of this script
# already assumes.
utf8_state() {
    printf '%s' "$1" | od -An -v -tu1 | awk '
        { for (i = 1; i <= NF; i++) b[n++] = $i }
        END {
            i = 0
            while (i < n) {
                c = b[i]
                if (c < 128) need = 1
                else if (c < 192) { print "bad"; exit }
                else if (c < 224) need = 2
                else if (c < 240) need = 3
                else if (c < 248) need = 4
                else { print "bad"; exit }
                if (i + need > n) { print "bad"; exit }
                for (k = 1; k < need; k++)
                    if (b[i + k] < 128 || b[i + k] > 191) { print "bad"; exit }
                i += need
            }
            print "ok"
        }'
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/kb-e2e.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1
# Keep the global tier inside the throwaway directory: a test run must never
# touch the developer's own ~/.kb.
KB_STORE="$WORK/globalstore"
export KB_STORE

# Neighbours that kb must never touch. lap owns its hash chain and coboard
# owns its log; a knowledge base that wrote into either would corrupt them.
mkdir -p .lap .coboard
echo 'lap log line' > .lap/log.jsonl
echo 'coboard log line' > .coboard/log.jsonl

# ---------------------------------------------------------------- init
t "init creates the §1.6 layout"
expect_ok "$KB" init
[ -d .kb ] || fail ".kb directory missing"
[ -f .kb/sources.jsonl ] || fail "sources.jsonl missing"
[ -f .kb/documents.jsonl ] || fail "documents.jsonl missing"
[ -d .kb/blobs ] || fail "blobs/ missing"
[ -d .kb/index ] || fail "index/ missing"

t "the .gitignore ignores index/ and nothing else"
out=$(cat .kb/.gitignore)
[ "$out" = "index/" ] || fail "unexpected .gitignore: $out"

t "a second init in the same directory fails with exit 1"
expect_code 1 "$KB" init

t "init --json reports the tier and the path"
out=$("$KB" init --store global --json)
has "init global" "$out" '"store":"global"'
has "init global" "$out" '"ok":true'

t "an unknown command exits 1 and says so"
expect_code 1 "$KB" bogus
expect_grep "unknown_command" "$KB" bogus --json

t "an unknown option is rejected rather than ignored"
expect_code 1 "$KB" ls --nope
expect_grep '"error":"usage"' "$KB" ls --nope --json

# ---------------------------------------------------------------- add
cat > page.md <<'MD'
Preamble before any heading.

# Completion ports

CreateIoCompletionPort associates a handle with a port.

## Threading

One thread per core is the usual shape.
MD

t "add files content from a file and reports its identifiers"
out=$("$KB" add --title "Completion ports" --collection win32-iocp \
        --url https://example.test/iocp --file page.md --json)
has "add" "$out" '"ok":true'
has "add" "$out" '"document":"D-1"'
has "add" "$out" '"source":"S-1"'
has "add" "$out" '"created":true'
has "add" "$out" '"reindexed":true'
has "add" "$out" '"blobWritten":true'
has "add" "$out" '"splitter":"markdown"'
has "add" "$out" '"chunkBase":1'
HASH=$(jstr contentHash "$out")

t "the blob is named by the sha256 of its own bytes"
[ -f ".kb/blobs/$HASH" ] || fail "blob $HASH missing"
actual=$(sha_of ".kb/blobs/$HASH")
if [ "$actual" != "unavailable" ] && [ "$actual" != "$HASH" ]; then
    fail "blob name $HASH but content hashes to $actual"
fi

t "exactly one blob exists after one document"
n=$(ls .kb/blobs | wc -l | tr -d ' ')
[ "$n" = "1" ] || fail "expected 1 blob, found $n"

t "add requires a title and a collection"
expect_code 1 sh -c "printf 'x' | '$KB' add --collection c"
expect_code 1 sh -c "printf 'x' | '$KB' add --title t"

t "add refuses empty content"
expect_code 1 sh -c "printf '' | '$KB' add --title t --collection c"

t "a collection name may not look nested"
expect_grep '"error":"usage"' sh -c \
    "printf 'x' | '$KB' add --title t --collection a/b --json"

# ------------------------------------------------------------ idempotency
t "re-filing the same text at the same locator re-indexes nothing"
before=$(wc -c < ".kb/documents.jsonl")
out=$("$KB" add --title "Completion ports" --collection win32-iocp \
        --url https://example.test/iocp --file page.md --json)
has "idempotent" "$out" '"document":"D-1"'
has "idempotent" "$out" '"created":false'
has "idempotent" "$out" '"reindexed":false'
has "idempotent" "$out" '"blobWritten":false'
n=$(ls .kb/blobs | wc -l | tr -d ' ')
[ "$n" = "1" ] || fail "re-ingest wrote a second blob ($n present)"
after=$(wc -c < ".kb/documents.jsonl")
[ "$after" -gt "$before" ] || fail "the touch record was not appended"

t "the touch record carries fetchedAt and nothing else"
expect_grep '"type":"touch","id":"D-1"' tail -n 1 .kb/documents.jsonl
expect_not_grep 'contentHash' tail -n 1 .kb/documents.jsonl

t "every add outcome reports the same set of keys"
# A caller should not have to work out which keys came back before it can
# read the answer.
touch_out=$("$KB" add --title "Completion ports" --collection win32-iocp \
        --url https://example.test/iocp --file page.md --json)
for k in store document source contentHash bytes collection mime splitter \
         chunkCount chunkBase created reindexed blobWritten fetchedAt; do
    has "touch shape" "$touch_out" "\"$k\":"
done

t "the human line says the document was unchanged"
expect_grep 'unchanged; fetchedAt updated' sh -c \
    "'$KB' add --title 'Completion ports' --collection win32-iocp \
        --url https://example.test/iocp --file page.md"

t "changed content keeps the document id and takes a fresh chunk range"
printf '# Completion ports\n\nRewritten body.\n' > page.md
out=$("$KB" add --title "Completion ports" --collection win32-iocp \
        --url https://example.test/iocp --file page.md --json)
has "update" "$out" '"document":"D-1"'
has "update" "$out" '"created":false'
has "update" "$out" '"reindexed":true'
has "update" "$out" '"blobWritten":true'
hasnt "update" "$out" '"chunkBase":1'
n=$(ls .kb/blobs | wc -l | tr -d ' ')
[ "$n" = "2" ] || fail "expected 2 blobs after a content change, found $n"

t "the same locator in another collection is a separate filing"
out=$("$KB" add --title "Completion ports" --collection notes \
        --url https://example.test/iocp --file page.md --json)
has "collection split" "$out" '"document":"D-2"'
has "collection split" "$out" '"source":"S-2"'

t "content with no locator is identified by its own hash"
out=$(printf 'a standalone note about kqueue.\n' | \
        "$KB" add --title kqueue --collection bsd --json)
D_NOTE=$(jstr document "$out")
has "inline" "$out" '"created":true'
out2=$(printf 'a standalone note about kqueue.\n' | \
        "$KB" add --title kqueue --collection bsd --json)
has "inline idempotent" "$out2" "\"document\":\"$D_NOTE\""
has "inline idempotent" "$out2" '"created":false'

t "a --file with no url is located by the file's own path"
printf 'a standalone note about kqueue.\n' > note.txt
out=$("$KB" add --title kqueue --collection bsd --file note.txt --json)
has "file locator" "$out" '"created":true'
# The same bytes are already stored, so no second blob is written even
# though this is a different document.
has "file locator" "$out" '"blobWritten":false'

# ------------------------------------------------------------- metadata
t "metadata is stored verbatim and returned unchanged"
printf 'paper text\n' | "$KB" add --title Paper --collection papers \
    --meta '{"year":2003,"authors":["Vyukov","Dice"]}' --json > /dev/null
out=$("$KB" ls --collection papers --json)
has "meta" "$out" '"meta":{"year":2003,"authors":\["Vyukov","Dice"\]}'

t "metadata that is not a JSON object is refused"
expect_grep '"error":"usage"' sh -c \
    "printf 'x' | '$KB' add --title t --collection c --meta '[1,2]' --json"
expect_grep '"error":"usage"' sh -c \
    "printf 'x' | '$KB' add --title t --collection c --meta 'nope' --json"

# ------------------------------------------------------------------- ls
t "ls lists documents with their tier and collection"
out=$("$KB" ls --json)
has "ls" "$out" '"id":"D-1"'
has "ls" "$out" '"store":"project"'
has "ls" "$out" '"collection":"win32-iocp"'

t "ls filters by collection, source and mime"
out=$("$KB" ls --collection notes --json)
has "ls collection" "$out" '"count":1'
out=$("$KB" ls --source S-1 --json)
has "ls source" "$out" '"count":1'
out=$("$KB" ls --mime text/markdown --json)
hasnt "ls mime" "$out" '"count":0'
out=$("$KB" ls --mime application/pdf --json)
has "ls mime none" "$out" '"count":0'

t "ls --since is a lower bound on fetchedAt"
out=$("$KB" ls --since 2000-01-01T00:00:00Z --json)
hasnt "ls since old" "$out" '"count":0'
out=$("$KB" ls --since 2999-01-01T00:00:00Z --json)
has "ls since future" "$out" '"count":0'

t "ls --limit caps the rows"
out=$("$KB" ls --limit 1 --json)
has "ls limit" "$out" '"count":1'

t "the query-string spelling of a flag also works"
out=$("$KB" ls --collection=notes --json)
has "ls eq form" "$out" '"count":1'

# ------------------------------------------------------------------ get
t "get returns one document"
out=$("$KB" get D-1 --json)
has "get" "$out" '"id":"D-1"'

t "get ?include=text returns the stored text"
out=$("$KB" get D-1 --include text --json)
has "get text" "$out" '"text":"# Completion ports'

t "get ?include=chunks returns spans and headings that tile the document"
out=$("$KB" get D-1 --include chunks --json)
has "get chunks" "$out" '"chunks":\['
has "get chunks" "$out" '"span":{"start":0'
has "get chunks" "$out" '"heading":"Completion ports"'
# the last span must reach the document's byte count
bytes=$(printf '%s' "$out" | sed -n 's/.*"bytes":\([0-9]*\).*/\1/p')
has "get chunks tail" "$out" "\"end\":$bytes}"

t "get rejects an id that is not a document id"
expect_code 1 "$KB" get S-1
expect_grep '"error":"usage"' "$KB" get C-4 --json

t "get on a missing document is not_found and exits 1"
expect_code 1 "$KB" get D-9999
expect_grep '"error":"not_found"' "$KB" get D-9999 --json

# ---------------------------------------------------------- collections
t "collections lists names with counts"
out=$("$KB" collections --json)
has "collections" "$out" '"name":"win32-iocp"'
has "collections" "$out" '"name":"notes"'
has "collections" "$out" '"documents":1'

# --------------------------------------------------------------- status
t "status reports both tiers and which one takes writes"
out=$("$KB" status --json)
has "status" "$out" '"store":"project"'
has "status" "$out" '"store":"global"'
has "status" "$out" '"defaultWrite":"project"'
has "status" "$out" '"chunkTokens":400'
has "status" "$out" '"chunkOverlap":60'
has "status" "$out" '"model":null'
has "status" "$out" '"nextIds"'

t "the chunking parameters are recorded in index/model.json"
[ -f .kb/index/model.json ] || fail "index/model.json missing"
expect_grep '"chunkTokens":400' cat .kb/index/model.json
expect_grep '"chunker":"structural-1"' cat .kb/index/model.json

t "a store built with other chunking parameters says a reindex is owed"
cp .kb/index/model.json model.json.bak
printf '{"chunker":"old","chunkTokens":128,"chunkOverlap":16}\n' \
    > .kb/index/model.json
expect_grep '"current":false' "$KB" status --json
cp model.json.bak .kb/index/model.json

# -------------------------------------------------------- store discovery
t "a write from a subdirectory finds the store above it"
mkdir -p deep/er/still
( cd deep/er/still && printf 'nested note\n' | \
    "$KB" add --title nested --collection deepdive --json ) > /dev/null
expect_grep '"name":"deepdive"' "$KB" collections --json

t "a .kb FILE in a parent does not shadow the real store"
mkdir -p decoy/inner
printf 'not a store' > decoy/.kb
( cd decoy/inner && "$KB" ls --json ) > lsout.txt 2>&1
expect_grep '"id":"D-1"' cat lsout.txt
rm -f decoy/.kb

t "outside any store, a read says so and exits 1"
# A sibling of $WORK, so no .kb exists anywhere above it.
AWAY=$(mktemp -d "${TMPDIR:-/tmp}/kb-away.XXXXXX")
( cd "$AWAY" && KB_STORE="$AWAY/nostore" "$KB" ls --json ) > away.txt 2>&1
expect_grep '"error":"not_found"' cat away.txt
( cd "$AWAY" && KB_STORE="$AWAY/nostore" "$KB" ls >/dev/null 2>&1 )
[ $? -eq 1 ] || fail "expected exit 1 outside any store"
rm -rf "$AWAY"

# ------------------------------------------------------------ both tiers
t "--store global writes to the global tier"
out=$(printf 'global note\n' | "$KB" add --title global --collection shared \
        --store global --json)
has "global add" "$out" '"store":"global"'
has "global add" "$out" '"document":"D-1"'
[ -f "$KB_STORE/documents.jsonl" ] || fail "global log missing"

t "identifiers are per store, so D-1 exists in both tiers"
out=$("$KB" ls --store all --json)
has "both tiers" "$out" '"store":"project"'
has "both tiers" "$out" '"store":"global"'

t "get resolves the project tier first when both hold the id"
out=$("$KB" get D-1 --json)
has "get precedence" "$out" '"store":"project"'
out=$("$KB" get D-1 --store global --json)
has "get global" "$out" '"store":"global"'
has "get global" "$out" '"collection":"shared"'

t "--store all is refused as a write target"
expect_grep '"error":"usage"' sh -c \
    "printf 'x' | '$KB' add --title t --collection c --store all --json"

# ------------------------------------------------------------- the lock
if [ -n "$KB_TESTS" ]; then
    t "a second writer is refused with store_locked and names the holder"
    rm -f lockctl
    mkfifo lockctl
    "$KB_TESTS" --hold-lock "$WORK/.kb" < lockctl > lockout.txt 2>&1 &
    holder=$!
    exec 9> lockctl
    i=0
    while [ $i -lt 200 ]; do
        if grep -q locked lockout.txt 2>/dev/null; then break; fi
        sleep 0.05
        i=$((i + 1))
    done
    if ! grep -q locked lockout.txt 2>/dev/null; then
        fail "the lock holder never started: $(cat lockout.txt)"
    else
        pid=$(awk '{print $2}' lockout.txt)
        before=$(wc -c < .kb/documents.jsonl)
        out=$(printf 'blocked\n' | "$KB" add --title blocked \
                --collection locked --json 2>&1)
        rc=$?
        [ "$rc" -eq 1 ] || fail "expected exit 1 while locked, got $rc"
        has "locked" "$out" '"error":"store_locked"'
        has "locked" "$out" "process $pid"
        after=$(wc -c < .kb/documents.jsonl)
        [ "$before" = "$after" ] || fail "a locked-out writer still appended"
    fi
    exec 9>&-
    wait $holder
    rm -f lockctl

    t "the store is writable again once the holder exits"
    expect_ok sh -c "printf 'after\n' | '$KB' add --title after \
        --collection locked --json"
fi

# ------------------------------------------------- crash mid-append repair
t "a torn final line does not break a read"
docs_before=$("$KB" ls --json)
count_before=$(printf '%s' "$docs_before" | \
    sed -n 's/.*"count":\([0-9]*\).*/\1/p')
printf '{"type":"document","id":"D-777","sou' >> .kb/documents.jsonl
out=$("$KB" ls --json)
has "torn read" "$out" "\"count\":$count_before"
expect_grep '"torn":true' "$KB" status --json

t "the next write repairs the torn line and does not reuse its id"
out=$(printf 'after the crash\n' | "$KB" add --title recovered \
        --collection recovery --json 2>/dev/null)
has "repair" "$out" '"ok":true'
new_id=$(jstr document "$out")
[ "$new_id" != "D-777" ] || fail "the interrupted record's id came back"
expect_not_grep 'D-777' cat .kb/documents.jsonl

t "the log still reads cleanly after the repaired append"
out=$("$KB" ls --json)
has "post repair" "$out" '"ok":true'
has "post repair" "$out" "\"id\":\"$new_id\""
expect_grep '"torn":false' "$KB" status --json

t "an id is never reused after a record is removed from the log"
# The harshest form of a delete: the newest record vanishes entirely. The
# counter is durable, so the id it held is gone for good.
last=$(tail -n 1 .kb/documents.jsonl | \
    sed -n 's/.*"id":"\(D-[0-9]*\)".*/\1/p')
[ -n "$last" ] || fail "could not read the newest document id"
sed '$d' .kb/documents.jsonl > docs.trimmed && mv docs.trimmed .kb/documents.jsonl
expect_not_grep "\"id\":\"$last\"" cat .kb/documents.jsonl
out=$(printf 'fresh\n' | "$KB" add --title fresh --collection recovery --json)
new=$(jstr document "$out")
[ "$new" != "$last" ] || fail "id $last was handed out a second time"
want="D-$(( ${last#D-} + 1 ))"
[ "$new" = "$want" ] || fail "expected $want after $last, got $new"

# --------------------------------------------------- neighbours untouched
t "kb never writes outside its own store"
[ "$(cat .lap/log.jsonl)" = "lap log line" ] || fail ".lap was modified"
[ "$(cat .coboard/log.jsonl)" = "coboard log line" ] || fail ".coboard was modified"
n=$(ls -a .lap | wc -l | tr -d ' ')
[ "$n" = "3" ] || fail ".lap gained entries"
n=$(ls -a .coboard | wc -l | tr -d ' ')
[ "$n" = "3" ] || fail ".coboard gained entries"

# ------------------------------------------------------------ chunk shapes
t "source code splits on top-level declarations"
cat > sample.c <<'EOC'
#include <stdio.h>

static int32_t helper(int32_t x) {
    return x + 1;
}

int32_t main(void) {
    return helper(1);
}
EOC
out=$("$KB" add --title sample --collection code --file sample.c --json)
has "code" "$out" '"splitter":"code"'
has "code" "$out" '"chunkCount":2'
D_CODE=$(jstr document "$out")
out=$("$KB" get "$D_CODE" --include chunks --json)
has "code heading" "$out" '"heading":"int32_t main(void) {"'

t "html splits on heading elements"
printf '<h1>io_uring</h1><p>a</p><h2>prep_recv</h2><p>b</p>' > page.html
out=$("$KB" add --title html --collection code --file page.html --json)
has "html" "$out" '"splitter":"html"'
D_HTML=$(jstr document "$out")
out=$("$KB" get "$D_HTML" --include chunks --json)
has "html heading" "$out" '"heading":"io_uring"'
has "html heading" "$out" '"heading":"prep_recv"'

t "plain text falls back to a sliding window with overlap"
i=0
: > big.txt
while [ $i -lt 400 ]; do
    echo "a line of prose about completion ports and their queues." >> big.txt
    i=$((i + 1))
done
out=$("$KB" add --title big --collection code --file big.txt --json)
has "text" "$out" '"splitter":"text"'
D_BIG=$(jstr document "$out")
bytes=$(printf '%s' "$out" | sed -n 's/.*"bytes":\([0-9]*\).*/\1/p')
out=$("$KB" get "$D_BIG" --include chunks --json)
has "text spans" "$out" '"span":{"start":0'
has "text tail" "$out" "\"end\":$bytes}"

# =========================================================== keyword search
# A store of its own, so the corpus these assertions are about is exactly
# the three pages below and nothing the tests above happened to leave.
mkdir -p iso
kbi() { ( cd "$WORK/iso" && "$KB" "$@" ); }
kbi init > /dev/null

cat > iso/iocp.md <<'MD'
# CreateIoCompletionPort

CreateIoCompletionPort associates a file handle with an I/O completion port.
The port is drained with GetQueuedCompletionStatus.

## Threading

One thread per core is the usual shape for a completion port pool.
MD
cat > iso/uring.md <<'MD'
# io_uring_prep_recv

io_uring_prep_recv queues a receive on the submission ring.

## io_uring_prep_send

io_uring_prep_send queues a send on the submission ring.
MD
cat > iso/kqueue.md <<'MD'
# EVFILT_READ

EVFILT_READ fires when a descriptor becomes readable. kevent registers it.
MD
kbi add --title "Completion ports" --collection win32-iocp --file iocp.md \
    --json > /dev/null
kbi add --title "io_uring" --collection io-uring --file uring.md \
    --json > /dev/null
kbi add --title "kqueue" --collection bsd --file kqueue.md --json > /dev/null

t "an add leaves a keyword index that search can use immediately"
[ -f iso/.kb/index/fts.db ] || fail "index/fts.db was not written by add"
expect_grep '"keyword":{"current":true' kbi status --store project --json

t "search resolves an exact identifier, and its opposite ranks below it"
out=$(kbi search io_uring_prep_recv --store project --json)
first=$(printf '%s' "$out" | sed -n 's/.*"hits":\[{"chunk":"\(C-[0-9]*\)".*/\1/p')
has "exact" "$out" '"document":"D-2"'
# The recv chunk must come first; the send chunk shares io, uring and prep
# and is the one §4 says a vector index cannot separate from it.
recv_line=$(kbi search io_uring_prep_recv --store project | head -n 1)
in_str "$recv_line" "$first" || fail "human and json disagree on the top hit"
n=$(printf '%s' "$out" | sed -n 's/.*"count":\([0-9]*\).*/\1/p')
[ "$n" -ge 2 ] || fail "expected the send chunk to be a weaker hit too"
# One score per hit, in rank order; the first must beat the second.
scores=$(printf '%s' "$out" | tr '}' '\n' | \
    sed -n 's/.*"bm25":\([0-9.]*\).*/\1/p')
top_score=$(printf '%s\n' "$scores" | sed -n 1p)
second=$(printf '%s\n' "$scores" | sed -n 2p)
awk -v a="$top_score" -v b="$second" 'BEGIN{exit !(a>b)}' || \
    fail "the exact identifier did not outrank the one sharing its words ($top_score vs $second)"

t "a query may be typed in any case"
expect_grep '"document":"D-1"' kbi search createiocompletionport \
    --store project --json
out=$(kbi search CREATEIOCOMPLETIONPORT --store project --json)
has "case" "$out" '"document":"D-1"'

t "a camelCase name is also found by the words inside it"
out=$(kbi search "completion port" --store project --json)
has "camel" "$out" '"document":"D-1"'

t "an underscored identifier is not found by an unrelated document"
out=$(kbi search EVFILT_READ --store project --json)
has "evfilt" "$out" '"document":"D-3"'
first=$(printf '%s' "$out" | sed -n 's/.*"hits":\[{"chunk":"[^"]*","document":"\([^"]*\)".*/\1/p')
[ "$first" = "D-3" ] || fail "EVFILT_READ ranked $first first"

t "the hit shape is §4's, as far as this slice can fill it"
out=$(kbi search io_uring_prep_recv --store project --json)
for k in chunk document source title heading snippet collection store \
         alsoGlobal matched scores fetchedAt stale; do
    has "hit shape" "$out" "\"$k\":"
done
has "hit shape" "$out" '"matched":\["keyword"\]'
has "hit shape" "$out" '"mode":"keyword"'

t "an absent score is absent, not zero"
# §4's vector and fused belong to a path that did not run. A caller must be
# able to tell that from a path that ran and found nothing. `stale` is a
# different case and is always present: §5 puts it on every hit, and a
# freshly filed document answers "false" rather than saying nothing.
hasnt "scores" "$out" '"vector"'
hasnt "scores" "$out" '"fused"'
has "scores" "$out" '"stale":false'

t "search returns snippets, never whole chunks"
# The snippet is capped; the full passage is what kb chunk is for.
snip=$(printf '%s' "$out" | sed -n 's/.*"snippet":"\([^"]*\)".*/\1/p')
[ -n "$snip" ] || fail "no snippet returned"
[ "${#snip}" -le 260 ] || fail "snippet is ${#snip} bytes; the cap is 240"
hasnt "snippet" "$snip" '\\n'

t "hybrid and semantic refuse rather than quietly answering with keyword"
expect_code 1 kbi search anything --mode hybrid
expect_grep '"error":"model_missing"' kbi search anything --mode hybrid --json
expect_grep '"error":"model_missing"' kbi search anything --mode semantic --json
expect_grep 'embedding model' kbi search anything --mode hybrid
expect_grep '"error":"usage"' kbi search anything --mode telepathy --json

t "search needs a query"
expect_code 1 kbi search
expect_grep '"error":"usage"' kbi search --json

t "the filters in §4 narrow the scope"
out=$(kbi search ring --store project --collection io-uring --json)
has "collection" "$out" '"collection":"io-uring"'
hasnt "collection" "$out" '"collection":"win32-iocp"'
out=$(kbi search port --store project --collection win32-iocp,io-uring --json)
has "collection list" "$out" '"collection":"win32-iocp"'
out=$(kbi search port --store project --collection bsd --json)
has "collection miss" "$out" '"count":0'
out=$(kbi search port --store project --source S-1 --json)
has "source" "$out" '"document":"D-1"'
out=$(kbi search port --store project --source S-3 --json)
has "source miss" "$out" '"count":0'
out=$(kbi search port --store project --mime text/markdown --json)
hasnt "mime" "$out" '"count":0'
out=$(kbi search port --store project --mime application/pdf --json)
has "mime miss" "$out" '"count":0'
out=$(kbi search port --store project --since 2999-01-01T00:00:00Z --json)
has "since" "$out" '"count":0'

t "minScore is a floor on the bm25 score"
out=$(kbi search port --store project --min-score 0 --json)
lo=$(printf '%s' "$out" | sed -n 's/.*"count":\([0-9]*\).*/\1/p')
out=$(kbi search port --store project --min-score 1000 --json)
has "minScore" "$out" '"count":0'
[ "$lo" -gt 0 ] || fail "expected hits without a minScore floor"

t "k defaults to 10 and expand returns neighbouring snippets"
out=$(kbi search port --store project --expand 1 --json)
has "expand" "$out" '"neighbours":\['
hasnt "expand" "$out" '"text":'
out=$(kbi search port --store project --json)
hasnt "no expand" "$out" '"neighbours"'

t "a bad k or expand is refused"
expect_grep '"error":"usage"' kbi search port --k 0 --json
expect_grep '"error":"usage"' kbi search port --k -1 --json
expect_grep '"error":"usage"' kbi search port --expand -1 --json
expect_grep '"error":"usage"' kbi search port --min-score nope --json

# --------------------------------------------------------- k is capped at 100
mkdir -p bigk
printf 'kwzz a line about completion queues and their ports\n' > bigk/k.txt
i=0
while [ $i -lt 13 ]; do
    cat bigk/k.txt bigk/k.txt > bigk/k2.txt && mv bigk/k2.txt bigk/k.txt
    i=$((i + 1))
done
kbb() { ( cd "$WORK/bigk" && "$KB" "$@" ); }
kbb init > /dev/null
kbb add --title big --collection bulk --file k.txt --json > /dev/null

t "k is clamped to 100 however many chunks match"
out=$(kbb search kwzz --store project --k 5 --json)
has "k 5" "$out" '"count":5'
out=$(kbb search kwzz --store project --k 100 --json)
has "k 100" "$out" '"count":100'
out=$(kbb search kwzz --store project --k 5000 --json)
has "k clamp" "$out" '"count":100'
# and there really are more than 100 chunks to be had, so the clamp binds
chunks=$(kbb rebuild --store project --json | \
    sed -n 's/.*"chunks":\([0-9]*\).*/\1/p')
[ "$chunks" -gt 100 ] || fail "only $chunks chunks; the clamp proves nothing"

# ------------------------------------------------------------ kb chunk (§4)
t "kb chunk returns the whole passage and its neighbours"
out=$(kbi chunk C-2 --store project --json)
has "chunk" "$out" '"id":"C-2"'
has "chunk" "$out" '"document":"D-1"'
has "chunk" "$out" '"text":'
has "chunk" "$out" '"neighbours":\['
has "chunk" "$out" '"id":"C-1"'
has "chunk" "$out" '"span":{"start":'

t "kb chunk --expand 0 returns the passage alone"
out=$(kbi chunk C-2 --store project --expand 0 --json)
has "chunk alone" "$out" '"neighbours":\[\]'

t "kb chunk rejects anything that is not a chunk id"
expect_code 1 kbi chunk D-1
expect_grep '"error":"usage"' kbi chunk D-1 --json
expect_grep '"error":"not_found"' kbi chunk C-99999 --json

t "a chunk id names the same passage as the document's own chunk list"
want=$(kbi get D-1 --include chunks --json | \
    sed -n 's/.*{"id":"C-2","document":"D-1","ordinal":1,"heading":"\([^"]*\)".*/\1/p')
got=$(kbi chunk C-2 --store project --expand 0 --json | \
    sed -n 's/.*"heading":"\([^"]*\)".*/\1/p')
[ -n "$want" ] && [ "$want" = "$got" ] || \
    fail "get says heading '$want', chunk says '$got'"

# ------------------------------------------------------------- both tiers
t "a document in both tiers is returned once, from the project tier"
"$KB" add --title "io_uring" --collection io-uring --file iso/uring.md \
    --store global --json > /dev/null
out=$(kbi search io_uring_prep_recv --json)
has "dedup" "$out" '"store":"project"'
has "dedup" "$out" '"alsoGlobal":true'
hasnt "dedup" "$out" '"store":"global"'

t "a document only the global tier has is still found"
printf 'zzunique a note that only the global store holds\n' | \
    "$KB" add --title globalonly --collection shared --store global --json \
    > /dev/null
out=$(kbi search zzunique --json)
has "global only" "$out" '"store":"global"'
has "global only" "$out" '"alsoGlobal":false'

t "--store narrows to one tier"
out=$(kbi search zzunique --store project --json)
has "project only" "$out" '"count":0'
out=$(kbi search zzunique --store global --json)
hasnt "global only" "$out" '"count":0'

t "the tiers fuse by rank, not by score"
# Rigged so the two answers disagree. The global tier's hit is a four-word
# passage that is nothing but the query term, which scores high; the
# project's is the same term once inside a long page, which scores low. A
# merge that compared scores would lead with global. A merge that compares
# ranks leads with project, because each is its own tier's best answer and
# §1.4 gives the tie to the project.
i=0
: > iso/rankbait.md
while [ $i -lt 60 ]; do
    echo "padding words that mean nothing at all here." >> iso/rankbait.md
    i=$((i + 1))
done
echo "a single zzrank mention buried in all of that." >> iso/rankbait.md
kbi add --title bait --collection bait --file rankbait.md --json > /dev/null
printf 'zzrank zzrank zzrank zzrank\n' | \
    "$KB" add --title rank --collection shared --store global --json > /dev/null
out=$(kbi search zzrank --json)
lead=$(printf '%s' "$out" | \
    sed -n 's/.*"hits":\[{[^}]*"store":"\([a-z]*\)".*/\1/p')
[ "$lead" = "project" ] || fail "the fused list led with the $lead tier"
scores=$(printf '%s' "$out" | tr '}' '\n' | \
    sed -n 's/.*"bm25":\([0-9.]*\).*/\1/p')
lead_score=$(printf '%s\n' "$scores" | sed -n 1p)
next_score=$(printf '%s\n' "$scores" | sed -n 2p)
[ -n "$next_score" ] || fail "expected a hit from the other tier as well"
# The leading hit scores LOWER than the one behind it. That is only
# possible because the two lists were merged by rank.
awk -v a="$lead_score" -v b="$next_score" 'BEGIN{exit !(a<b)}' || \
    fail "the leading hit scored $lead_score against $next_score behind it; \
the merge looks like a sort by score"

# --------------------------------------------------------- index staleness
t "search refuses a stale index rather than answering without it"
cp iso/.kb/index/fts.db iso/fts.db.bak
head -c 64 iso/fts.db.bak > iso/.kb/index/fts.db
expect_code 1 kbi search port --store project
expect_grep '"error":"index_stale"' kbi search port --store project --json
expect_grep 'rebuild' kbi search port --store project --json

t "an index from another format version is refused, not misread"
cp iso/fts.db.bak iso/.kb/index/fts.db
printf '\071' | dd of=iso/.kb/index/fts.db bs=1 seek=8 conv=notrunc \
    > /dev/null 2>&1
expect_grep '"error":"index_stale"' kbi search port --store project --json
expect_grep 'format version' kbi search port --store project --json

t "an index that no longer describes the logs is refused"
cp iso/fts.db.bak iso/.kb/index/fts.db
printf 'a late note about ports\n' >> iso/.kb/blobs/scratch 2>/dev/null
cp iso/.kb/documents.jsonl iso/docs.bak
tail -n 1 iso/.kb/documents.jsonl | sed 's/"contentHash":"[0-9a-f]*"/"contentHash":"00000000000000000000000000000000000000000000000000000000000000ff"/' \
    >> iso/.kb/documents.jsonl
expect_grep '"error":"index_stale"' kbi search port --store project --json
expect_grep 'no longer describes' kbi search port --store project --json
cp iso/docs.bak iso/.kb/documents.jsonl
rm -f iso/.kb/blobs/scratch

t "status reports the same staleness search refuses on"
cp iso/fts.db.bak iso/.kb/index/fts.db
expect_grep '"current":true' kbi status --store project --json
head -c 64 iso/fts.db.bak > iso/.kb/index/fts.db
expect_grep '"keyword":{"current":false' kbi status --store project --json
cp iso/fts.db.bak iso/.kb/index/fts.db

# --------------------------------------------------------------- rebuild
t "rebuild reconstructs the index from the logs and blobs alone"
before_json=$(kbi search port --store project --json)
before_sha=$(sha_of iso/.kb/index/fts.db)
rm -rf iso/.kb/index
[ -d iso/.kb/index ] && fail "index/ was not removed"
expect_code 1 kbi search port --store project
expect_ok kbi rebuild --store project
[ -f iso/.kb/index/fts.db ] || fail "rebuild wrote no fts.db"
after_sha=$(sha_of iso/.kb/index/fts.db)
[ "$before_sha" = "$after_sha" ] || \
    fail "rebuild produced different bytes: $before_sha vs $after_sha"

t "search returns byte-identical results after a rebuild from nothing"
after_json=$(kbi search port --store project --json)
[ "$before_json" = "$after_json" ] || fail "results changed across a rebuild
--- before ---
$before_json
--- after ---
$after_json"

t "rebuild replaces a truncated index rather than reusing it"
head -c 40 iso/.kb/index/fts.db > iso/trunc && mv iso/trunc iso/.kb/index/fts.db
expect_ok kbi rebuild --store project
[ "$(sha_of iso/.kb/index/fts.db)" = "$before_sha" ] || \
    fail "rebuild did not replace the truncated index"
expect_ok kbi search port --store project

t "rebuild replaces an index from another version"
printf '\071' | dd of=iso/.kb/index/fts.db bs=1 seek=8 conv=notrunc \
    > /dev/null 2>&1
expect_ok kbi rebuild --store project
[ "$(sha_of iso/.kb/index/fts.db)" = "$before_sha" ] || \
    fail "rebuild did not replace the foreign index"

t "rebuild reports what it wrote, per tier"
out=$(kbi rebuild --store project --json)
has "rebuild" "$out" '"store":"project"'
has "rebuild" "$out" '"documents":4'
has "rebuild" "$out" '"missingBlobs":0'
has "rebuild" "$out" '"mismatched":0'
has "rebuild" "$out" '"indexBytes":'

t "rebuild spans every tier by default"
out=$(kbi rebuild --json)
has "rebuild all" "$out" '"store":"project"'
has "rebuild all" "$out" '"store":"global"'

t "a store with a missing blob still rebuilds, and says so"
mv iso/.kb/blobs "$WORK/blobs.hidden"
mkdir iso/.kb/blobs
out=$(kbi rebuild --store project --json)
has "missing blob" "$out" '"ok":true'
hasnt "missing blob" "$out" '"missingBlobs":0'
rmdir iso/.kb/blobs
mv "$WORK/blobs.hidden" iso/.kb/blobs
expect_ok kbi rebuild --store project

t "re-filing unchanged content does not make the index stale"
# §2: a touch updates fetchedAt and re-indexes nothing, so it must not cost
# a rebuild either.
sha_before=$(sha_of iso/.kb/index/fts.db)
kbi add --title "kqueue" --collection bsd --file kqueue.md --json | \
    grep -q '"reindexed":false' || fail "expected a touch"
expect_grep '"current":true' kbi status --store project --json
[ "$(sha_of iso/.kb/index/fts.db)" = "$sha_before" ] || \
    fail "a touch rewrote the index"

t "changed content updates the index in the same locked write"
printf '# EVFILT_READ\n\nzzfresh text about kevent filters.\n' > iso/kqueue.md
kbi add --title "kqueue" --collection bsd --file kqueue.md --json > /dev/null
expect_grep '"current":true' kbi status --store project --json
out=$(kbi search zzfresh --store project --json)
has "reindex" "$out" '"document":"D-3"'

t "a search writes nothing"
sha_before=$(sha_of iso/.kb/index/fts.db)
docs_before=$(wc -c < iso/.kb/documents.jsonl)
kbi search port --store project --json > /dev/null
kbi search port --json > /dev/null
[ "$(sha_of iso/.kb/index/fts.db)" = "$sha_before" ] || \
    fail "a search rewrote the index"
[ "$(wc -c < iso/.kb/documents.jsonl)" = "$docs_before" ] || \
    fail "a search appended to the log"

t "a search is not blocked by a writer"
if [ -n "$KB_TESTS" ]; then
    rm -f searchctl
    mkfifo searchctl
    "$KB_TESTS" --hold-lock "$WORK/iso/.kb" < searchctl > searchout.txt 2>&1 &
    holder=$!
    exec 9> searchctl
    i=0
    while [ $i -lt 200 ]; do
        if grep -q locked searchout.txt 2>/dev/null; then break; fi
        sleep 0.05
        i=$((i + 1))
    done
    if grep -q locked searchout.txt 2>/dev/null; then
        expect_ok kbi search port --store project --json
    else
        fail "the lock holder never started: $(cat searchout.txt)"
    fi
    exec 9>&-
    wait $holder
    rm -f searchctl
fi

t "an empty store answers a search instead of demanding a rebuild"
mkdir -p fresh
( cd "$WORK/fresh" && "$KB" init > /dev/null )
out=$( cd "$WORK/fresh" && "$KB" search anything --store project --json )
has "empty" "$out" '"ok":true'
has "empty" "$out" '"count":0'

t "every JSON route stays valid UTF-8 on text that is cut mid-character"
# Spans, snippet windows and the heading clamp are all byte offsets, and any
# of them can land inside a character. A JSON string is text, not bytes: half
# a character in one field costs the caller the whole response.
mkdir -p u8
kbu() { ( cd "$WORK/u8" && "$KB" "$@" ); }
kbu init > /dev/null
# A heading of 100 three-byte characters (the clamp is 200 bytes, which is
# not a multiple of three) and a body long enough that a snippet window
# lands mid-character too.
awk 'BEGIN{
    printf "# ";
    for (i = 0; i < 100; i++) printf "\344\270\200";
    printf " zzutf\n\nzzutf ";
    for (i = 0; i < 400; i++) printf "\344\270\200";
    printf "\n";
}' > u8/wide.md
kbu add --title wide --collection utf8 --file wide.md --json > /dev/null
for c in "search zzutf --store project" "chunk C-1 --store project" \
         "get D-1 --include chunks --store project"; do
    # shellcheck disable=SC2086
    if [ "$(utf8_state "$(kbu $c --json)")" != "ok" ]; then
        fail "kb $c emitted invalid UTF-8"
    fi
done

t "kb still writes nothing outside its own stores"
[ "$(cat .lap/log.jsonl)" = "lap log line" ] || fail ".lap was modified"
[ "$(cat .coboard/log.jsonl)" = "coboard log line" ] || fail ".coboard was modified"
n=$(ls -a .lap | wc -l | tr -d ' ')
[ "$n" = "3" ] || fail ".lap gained entries"

# ------------------------------------------------------------------- done
echo "e2e: $TESTS tests, $FAILED failed"
[ "$FAILED" -eq 0 ] || exit 1
exit 0
