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

WORK=$(mktemp -d "${TMPDIR:-/tmp}/kb-e2e.XXXXXX") || exit 1
# Without a directory of its own the run would work in, and the trap would
# delete, whatever the caller's current directory is.
[ -n "$WORK" ] && [ -d "$WORK" ] || exit 1
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1
# kb reads nothing from the home directory. HOME points into the throwaway
# directory anyway, so a regression that started reading it again would find
# the store planted there below, and never the developer's own.
HOME="$WORK/home"
export HOME
mkdir -p "$HOME"
unset KB_STORE

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

t "init --json reports the path"
mkdir -p initjson
out=$( cd initjson && "$KB" init --json )
has "init json" "$out" '"ok":true'
has "init json" "$out" '/initjson/.kb"'
hasnt "init json" "$out" '"store":'

t "init takes no --store"
expect_grep '"error":"usage"' "$KB" init --store global --json

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
for k in document source contentHash bytes collection mime splitter \
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
t "ls lists documents with their collection"
out=$("$KB" ls --json)
has "ls" "$out" '"id":"D-1"'
has "ls" "$out" '"collection":"win32-iocp"'
hasnt "ls" "$out" '"store":'

t "a type the chunker cannot split is refused, and nothing is filed"
before=$(wc -c < .kb/documents.jsonl)
out=$(printf '%%PDF-1.7' | "$KB" add --title paper --collection papers \
        --mime application/pdf --json)
has "pdf" "$out" '"error":"unsupported_mime"'
has "pdf" "$out" '"details":{"mime":"application/pdf"}'
out=$(printf '{"title":"t","collection":"c","content":"x","mime":"image/png"}\n' |
        "$KB" add --batch --json)
has "batch png" "$out" '"error":"unsupported_mime"'
[ "$before" = "$(wc -c < .kb/documents.jsonl)" ] || fail "a refused type was filed"
expect_ok sh -c "printf 'a: 1' | '$KB' add --title cfg --collection formats --mime application/yaml"
expect_ok sh -c "printf 'a,b' | '$KB' add --title csv --collection formats --mime text/csv"

t "an error without details has no details field"
expect_not_grep '"details"' "$KB" ls --nope --json

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

# -------------------------------------------------------------- filters
mkdir -p filt
kbf() { ( cd "$WORK/filt" && "$KB" "$@" ); }
kbf init > /dev/null
printf 'zzfilta\n' | kbf add --title "Completion Ports" --collection win \
    --meta '{"year":2024,"tags":["iocp","win32"]}' > /dev/null
printf 'zzfiltb\n' | kbf add --title "io_uring intro" --collection linux \
    --url https://kernel.test/uring --meta '{"year":2023}' > /dev/null
printf 'zzfiltc\n' | kbf add --title kqueue --collection bsd > /dev/null

t "ls takes a list of collections and a text filter over title and locator"
has "two collections" "$(kbf ls --collection win,linux --json)" '"count":2'
has "q title" "$(kbf ls --q completion --json)" '"count":1'
has "q locator" "$(kbf ls --q kernel.test --json)" '"count":1'

t "meta filters: every key matches, a value in an array counts"
has "meta number" "$(kbf ls --meta '{"year":2024}' --json)" '"count":1'
has "meta in array" "$(kbf ls --meta '{"tags":"iocp"}' --json)" '"count":1'
has "meta both keys" "$(kbf ls --meta '{"year":2023,"tags":"iocp"}' --json)" '"count":0'
has "meta absent key" "$(kbf ls --meta '{"lang":"c"}' --json)" '"count":0'
has "search meta" "$(kbf search zzfilta --meta '{"year":2024}' --json)" '"count":1'
has "search meta miss" "$(kbf search zzfilta --meta '{"year":2023}' --json)" '"count":0'
expect_grep '"error":"usage"' kbf ls --meta '[1]' --json

t "--since takes a date or a timestamp and refuses anything else"
has "since date" "$(kbf ls --since 2000-01-01 --json)" '"count":3'
has "since future" "$(kbf ls --since 2999-01-01 --json)" '"count":0'
expect_grep '"error":"usage"' kbf ls --since 2026-9-1 --json
expect_grep '"error":"usage"' kbf search zzfilta --since yesterday --json

t "the same text with a new title or meta records them and re-indexes nothing"
out=$(printf 'zzfiltb\n' | kbf add --title "io_uring, an introduction" \
        --collection linux --url https://kernel.test/uring \
        --meta '{"year":2025}' --json)
has "refile" "$out" '"reindexed":false'
out=$(kbf get D-2 --json)
has "new title" "$out" '"title":"io_uring, an introduction"'
has "new meta" "$out" '"year":2025'
printf 'zzfiltb\n' | kbf add --title "io_uring, an introduction" \
    --collection linux --url https://kernel.test/uring > /dev/null
has "meta kept" "$(kbf get D-2 --json)" '"year":2025'

t "a source's etag is the one its latest fetch was filed with"
printf 'zzfiltb\n' | kbf add --title "io_uring, an introduction" \
    --collection linux --url https://kernel.test/uring --etag abc123 > /dev/null
has "etag" "$(kbf sources show S-2 --json)" '"etag":"abc123"'
printf 'zzfiltb rewritten\n' | kbf add --title "io_uring, an introduction" \
    --collection linux --url https://kernel.test/uring > /dev/null
has "etag cleared" "$(kbf sources show S-2 --json)" '"etag":null'
has "status ok" "$(kbf sources --q kernel --json)" '"status":"ok"'

t "Markdown or HTML piped with no type is recognised; a comment line is not"
has "md" "$(printf '# Title\n\nbody\n' | kbf add --title md --collection sniff --json)" \
    '"mime":"text/markdown"'
has "html" "$(printf '<!doctype html><p>x' | kbf add --title h --collection sniff --json)" \
    '"mime":"text/html"'
has "cfg" "$(printf 'x=1\n# comment\n' | kbf add --title cfg --collection sniff --json)" \
    '"mime":"text/plain"'

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
t "status reports the one store it found"
out=$("$KB" status --json)
has "status" "$out" "\"path\":\"$(pwd -P)/.kb\""
has "status" "$out" '"present":true'
hasnt "status" "$out" '"tiers"'
hasnt "status" "$out" '"defaultWrite"'
has "status" "$out" '"chunkTokens":400'
has "status" "$out" '"chunkOverlap":60'
has "status" "$out" '"model":{"recorded":null,"available":null,"missing":"no embedding model in '
has "status" "$out" 'curl -fL'
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

# --------------------------------------------------------- no global tier
# There is one store, found by walking up from the current directory. A
# `.kb` in the home directory is an ordinary store that only a command run
# under the home directory can find, and nothing names a store any other way.
( cd "$HOME" && "$KB" init --json ) > /dev/null
# A sibling of $WORK, so no .kb exists anywhere above it, and outside $HOME.
AWAY=$(mktemp -d "${TMPDIR:-/tmp}/kb-away.XXXXXX") || exit 1
[ -n "$AWAY" ] && [ -d "$AWAY" ] || exit 1

t "outside any store, a read says so and exits 1"
( cd "$AWAY" && "$KB" ls --json ) > away.txt 2>&1
expect_grep '"error":"not_found"' cat away.txt
expect_grep 'kb init' cat away.txt
( cd "$AWAY" && "$KB" ls >/dev/null 2>&1 )
[ $? -eq 1 ] || fail "expected exit 1 outside any store"

t "a store in the home directory is not a fallback"
( cd "$AWAY" && "$KB" search anything --json ) > away.txt 2>&1
expect_grep '"error":"not_found"' cat away.txt
( cd "$AWAY" && "$KB" status --json ) > away.txt 2>&1
expect_grep '"path":null' cat away.txt

t "KB_STORE is not read"
( cd "$AWAY" && KB_STORE="$HOME/.kb" "$KB" ls --json ) > away.txt 2>&1
expect_grep '"error":"not_found"' cat away.txt

t "an add outside any store is refused, not filed somewhere else"
( cd "$AWAY" && printf 'x\n' | \
    "$KB" add --title t --collection c --json ) > away.txt 2>&1
expect_grep '"error":"not_found"' cat away.txt
[ ! -e "$AWAY/.kb" ] || fail "add created a store"
[ ! -s "$HOME/.kb/documents.jsonl" ] || fail "add wrote to the home store"
rm -rf "$AWAY"

t "--store is not an option on any command"
for c in "ls" "get D-1" "search port" "chunk C-1" "collections" "stats" \
         "stale" "refresh" "links D-1" "rebuild" "reindex" "compact" \
         "status"; do
    # shellcheck disable=SC2086
    expect_grep '"error":"usage"' "$KB" $c --store project --json
done
expect_grep '"error":"usage"' sh -c \
    "printf 'x' | '$KB' add --title t --collection c --store project --json"

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
        has "locked" "$out" "\"details\":{\"store\":\"[^\"]*\",\"pid\":$pid}"
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
expect_grep '"keyword":{"current":true' kbi status --json

t "search resolves an exact identifier, and its opposite ranks below it"
out=$(kbi search io_uring_prep_recv --json)
first=$(printf '%s' "$out" | sed -n 's/.*"hits":\[{"chunk":"\(C-[0-9]*\)".*/\1/p')
has "exact" "$out" '"document":"D-2"'
# The recv chunk must come first; the send chunk shares io, uring and prep
# and is the one §4 says a vector index cannot separate from it.
recv_line=$(kbi search io_uring_prep_recv | head -n 1)
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
    --json
out=$(kbi search CREATEIOCOMPLETIONPORT --json)
has "case" "$out" '"document":"D-1"'

t "a camelCase name is also found by the words inside it"
out=$(kbi search "completion port" --json)
has "camel" "$out" '"document":"D-1"'

t "an underscored identifier is not found by an unrelated document"
out=$(kbi search EVFILT_READ --json)
has "evfilt" "$out" '"document":"D-3"'
first=$(printf '%s' "$out" | sed -n 's/.*"hits":\[{"chunk":"[^"]*","document":"\([^"]*\)".*/\1/p')
[ "$first" = "D-3" ] || fail "EVFILT_READ ranked $first first"

t "the hit shape is §4's, as far as this slice can fill it"
out=$(kbi search io_uring_prep_recv --json)
for k in chunk document source title heading snippet collection matched \
         scores fetchedAt stale; do
    has "hit shape" "$out" "\"$k\":"
done
hasnt "hit shape" "$out" '"store":'
hasnt "hit shape" "$out" '"alsoGlobal":'

has "hit shape" "$out" '"matched":\["keyword"\]'
has "hit shape" "$out" '"mode":"keyword"'

t "an absent score is absent, not zero"
# §4's vector belongs to a path that did not run. A caller must be able to
# tell that from a path that ran and found nothing. `fused` did run — the
# keyword list is fused on its own — so it is there. `stale` is a different
# case and is always present: §5 puts it on every hit, and a freshly filed
# document answers "false" rather than saying nothing.
hasnt "scores" "$out" '"vector"'
has "scores" "$out" '"fused":'
has "scores" "$out" '"stale":false'

t "every hit carries its fused score, and the list is ordered by it"
fused=$(kbi search port --json | tr '}' '\n' | sed -n 's/.*"fused":\([0-9.]*\).*/\1/p')
[ "$(printf '%s\n' "$fused" | wc -l | tr -d ' ')" -gt 1 ] || fail "expected several hits for port"
# The best hit of one ranked list is 1/(60 + 1).
[ "$(printf '%s\n' "$fused" | sed -n 1p)" = "0.016393" ] || \
    fail "the top fused score is $(printf '%s\n' "$fused" | sed -n 1p), not 1/61"
printf '%s\n' "$fused" | awk 'NR > 1 && $1 > prev { bad = 1 } { prev = $1 } END { exit bad }' || \
    fail "hits are not in fused order: $fused"

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
out=$(kbi search ring --collection io-uring --json)
has "collection" "$out" '"collection":"io-uring"'
hasnt "collection" "$out" '"collection":"win32-iocp"'
out=$(kbi search port --collection win32-iocp,io-uring --json)
has "collection list" "$out" '"collection":"win32-iocp"'
out=$(kbi search port --collection bsd --json)
has "collection miss" "$out" '"count":0'
out=$(kbi search port --source S-1 --json)
has "source" "$out" '"document":"D-1"'
out=$(kbi search port --source S-3 --json)
has "source miss" "$out" '"count":0'
out=$(kbi search port --mime text/markdown --json)
hasnt "mime" "$out" '"count":0'
out=$(kbi search port --mime application/pdf --json)
has "mime miss" "$out" '"count":0'
out=$(kbi search port --since 2999-01-01T00:00:00Z --json)
has "since" "$out" '"count":0'

t "minScore is a floor on the bm25 score, applied before fusion"
out=$(kbi search port --min-score 0 --json)
lo=$(printf '%s' "$out" | sed -n 's/.*"count":\([0-9]*\).*/\1/p')
out=$(kbi search port --min-score 1000 --json)
has "minScore" "$out" '"count":0'
[ "$lo" -gt 0 ] || fail "expected hits without a minScore floor"
# A floor between the strongest and the weakest bm25 drops the weak hits and
# keeps the strong ones, and fusion then ranks what is left from the top: a
# floor on the fused score could not do the first, and one applied after
# fusion would not reset the second.
bm=$(kbi search port --json | tr '}' '\n' | sed -n 's/.*"bm25":\([0-9.]*\).*/\1/p')
hi=$(printf '%s\n' "$bm" | sed -n 1p)
low=$(printf '%s\n' "$bm" | tail -n 1)
floor=$(awk -v a="$hi" -v b="$low" 'BEGIN { printf "%.6f", (a + b) / 2 }')
awk -v a="$hi" -v b="$low" 'BEGIN { exit !(a > b) }' || fail "expected port's hits to differ in bm25"
out=$(kbi search port --min-score "$floor" --json)
kept=$(printf '%s' "$out" | tr '}' '\n' | sed -n 's/.*"bm25":\([0-9.]*\).*/\1/p')
[ -n "$kept" ] || fail "the floor $floor dropped every hit, including $hi"
printf '%s\n' "$kept" | awk -v f="$floor" '$1 < f { bad = 1 } END { exit bad }' || \
    fail "a hit below the floor $floor survived: $kept"
[ "$(printf '%s\n' "$kept" | wc -l | tr -d ' ')" -lt "$(printf '%s\n' "$bm" | wc -l | tr -d ' ')" ] || \
    fail "the floor $floor dropped nothing"
has "minScore" "$out" '"fused":0.016393'

t "k defaults to 10 and expand returns neighbouring snippets"
out=$(kbi search port --expand 1 --json)
has "expand" "$out" '"neighbours":\['
hasnt "expand" "$out" '"text":'
out=$(kbi search port --json)
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
out=$(kbb search kwzz --k 5 --json)
has "k 5" "$out" '"count":5'
out=$(kbb search kwzz --k 100 --json)
has "k 100" "$out" '"count":100'
out=$(kbb search kwzz --k 5000 --json)
has "k clamp" "$out" '"count":100'
# and there really are more than 100 chunks to be had, so the clamp binds
chunks=$(kbb rebuild --json | \
    sed -n 's/.*"chunks":\([0-9]*\).*/\1/p')
[ "$chunks" -gt 100 ] || fail "only $chunks chunks; the clamp proves nothing"

# ------------------------------------------------------------ kb chunk (§4)
t "kb chunk returns the whole passage and its neighbours"
out=$(kbi chunk C-2 --json)
has "chunk" "$out" '"id":"C-2"'
has "chunk" "$out" '"document":"D-1"'
has "chunk" "$out" '"text":'
has "chunk" "$out" '"neighbours":\['
has "chunk" "$out" '"id":"C-1"'
has "chunk" "$out" '"span":{"start":'

t "kb chunk --expand 0 returns the passage alone"
out=$(kbi chunk C-2 --expand 0 --json)
has "chunk alone" "$out" '"neighbours":\[\]'

t "kb chunk rejects anything that is not a chunk id"
expect_code 1 kbi chunk D-1
expect_grep '"error":"usage"' kbi chunk D-1 --json
expect_grep '"error":"not_found"' kbi chunk C-99999 --json

t "a chunk id names the same passage as the document's own chunk list"
want=$(kbi get D-1 --include chunks --json | \
    sed -n 's/.*{"id":"C-2","document":"D-1","ordinal":1,"heading":"\([^"]*\)".*/\1/p')
got=$(kbi chunk C-2 --expand 0 --json | \
    sed -n 's/.*"heading":"\([^"]*\)".*/\1/p')
[ -n "$want" ] && [ "$want" = "$got" ] || \
    fail "get says heading '$want', chunk says '$got'"


# --------------------------------------------------------- index staleness
t "search refuses a stale index rather than answering without it"
cp iso/.kb/index/fts.db iso/fts.db.bak
head -c 64 iso/fts.db.bak > iso/.kb/index/fts.db
expect_code 1 kbi search port
expect_grep '"error":"index_stale"' kbi search port --json
expect_grep '"details":{"structures":\["keyword"\]' kbi search port --json
expect_grep 'rebuild' kbi search port --json

t "an index from another format version is refused, not misread"
cp iso/fts.db.bak iso/.kb/index/fts.db
printf '\071' | dd of=iso/.kb/index/fts.db bs=1 seek=8 conv=notrunc \
    > /dev/null 2>&1
expect_grep '"error":"index_stale"' kbi search port --json
expect_grep 'format version' kbi search port --json

t "an index that no longer describes the logs is refused"
cp iso/fts.db.bak iso/.kb/index/fts.db
printf 'a late note about ports\n' >> iso/.kb/blobs/scratch 2>/dev/null
cp iso/.kb/documents.jsonl iso/docs.bak
tail -n 1 iso/.kb/documents.jsonl | sed 's/"contentHash":"[0-9a-f]*"/"contentHash":"00000000000000000000000000000000000000000000000000000000000000ff"/' \
    >> iso/.kb/documents.jsonl
expect_grep '"error":"index_stale"' kbi search port --json
expect_grep 'no longer describes' kbi search port --json
cp iso/docs.bak iso/.kb/documents.jsonl
rm -f iso/.kb/blobs/scratch

t "status reports the same staleness search refuses on"
cp iso/fts.db.bak iso/.kb/index/fts.db
expect_grep '"current":true' kbi status --json
head -c 64 iso/fts.db.bak > iso/.kb/index/fts.db
expect_grep '"keyword":{"current":false' kbi status --json
cp iso/fts.db.bak iso/.kb/index/fts.db

# --------------------------------------------------------------- rebuild
t "rebuild reconstructs the index from the logs and blobs alone"
before_json=$(kbi search port --json)
before_sha=$(sha_of iso/.kb/index/fts.db)
rm -rf iso/.kb/index
[ -d iso/.kb/index ] && fail "index/ was not removed"
expect_code 1 kbi search port
expect_ok kbi rebuild
[ -f iso/.kb/index/fts.db ] || fail "rebuild wrote no fts.db"
after_sha=$(sha_of iso/.kb/index/fts.db)
[ "$before_sha" = "$after_sha" ] || \
    fail "rebuild produced different bytes: $before_sha vs $after_sha"

t "search returns byte-identical results after a rebuild from nothing"
after_json=$(kbi search port --json)
[ "$before_json" = "$after_json" ] || fail "results changed across a rebuild
--- before ---
$before_json
--- after ---
$after_json"

t "rebuild replaces a truncated index rather than reusing it"
head -c 40 iso/.kb/index/fts.db > iso/trunc && mv iso/trunc iso/.kb/index/fts.db
expect_ok kbi rebuild
[ "$(sha_of iso/.kb/index/fts.db)" = "$before_sha" ] || \
    fail "rebuild did not replace the truncated index"
expect_ok kbi search port

t "rebuild replaces an index from another version"
printf '\071' | dd of=iso/.kb/index/fts.db bs=1 seek=8 conv=notrunc \
    > /dev/null 2>&1
expect_ok kbi rebuild
[ "$(sha_of iso/.kb/index/fts.db)" = "$before_sha" ] || \
    fail "rebuild did not replace the foreign index"

t "rebuild reports what it wrote"
out=$(kbi rebuild --json)
has "rebuild" "$out" '"path":'
hasnt "rebuild" "$out" '"stores":'
has "rebuild" "$out" '"documents":3'
has "rebuild" "$out" '"missingBlobs":0'
has "rebuild" "$out" '"mismatched":0'
has "rebuild" "$out" '"indexBytes":'

t "a store with a missing blob still rebuilds, and says so"
mv iso/.kb/blobs "$WORK/blobs.hidden"
mkdir iso/.kb/blobs
out=$(kbi rebuild --json)
has "missing blob" "$out" '"ok":true'
hasnt "missing blob" "$out" '"missingBlobs":0'
rmdir iso/.kb/blobs
mv "$WORK/blobs.hidden" iso/.kb/blobs
expect_ok kbi rebuild

t "re-filing unchanged content does not make the index stale"
# §2: a touch updates fetchedAt and re-indexes nothing, so it must not cost
# a rebuild either.
sha_before=$(sha_of iso/.kb/index/fts.db)
kbi add --title "kqueue" --collection bsd --file kqueue.md --json | \
    grep -q '"reindexed":false' || fail "expected a touch"
expect_grep '"current":true' kbi status --json
[ "$(sha_of iso/.kb/index/fts.db)" = "$sha_before" ] || \
    fail "a touch rewrote the index"

t "changed content updates the index in the same locked write"
printf '# EVFILT_READ\n\nzzfresh text about kevent filters.\n' > iso/kqueue.md
kbi add --title "kqueue" --collection bsd --file kqueue.md --json > /dev/null
expect_grep '"current":true' kbi status --json
out=$(kbi search zzfresh --json)
has "reindex" "$out" '"document":"D-3"'

t "a search writes nothing"
sha_before=$(sha_of iso/.kb/index/fts.db)
docs_before=$(wc -c < iso/.kb/documents.jsonl)
kbi search port --json > /dev/null
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
        expect_ok kbi search port --json
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
out=$( cd "$WORK/fresh" && "$KB" search anything --json )
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
for c in "search zzutf" "chunk C-1" "get D-1 --include chunks"; do
    # shellcheck disable=SC2086
    if [ "$(utf8_state "$(kbu $c --json)")" != "ok" ]; then
        fail "kb $c emitted invalid UTF-8"
    fi
done

# ================================================ §5 provenance and staleness
# A store of its own with three sources whose ages are planted rather than
# waited for. A `touch` record carries a new fetchedAt and nothing else (§2),
# which is exactly how a document is made old without disturbing anything the
# index is built from.
mkdir -p prov
kbv() { ( cd "$WORK/prov" && "$KB" "$@" ); }
kbv init > /dev/null
printf 'zzprov iocp notes about completion ports\n' > prov/a.md
printf 'zzprov uring notes about submission rings\n' > prov/b.md
printf 'zzprov kqueue notes about kevent filters\n' > prov/c.md
kbv add --title A --collection old-topic --file a.md --json > /dev/null
kbv add --title B --collection old-topic --file b.md --json > /dev/null
kbv add --title C --collection fresh-topic --file c.md --json > /dev/null
# Absolute dates, far enough apart that the thresholds below keep separating
# them for decades: D-1's source is the oldest, D-2's is in the middle, and
# D-3 keeps whatever the clock said when it was filed.
for r in 'D-1 2005-01-01T00:00:00Z' 'D-2 2023-06-15T12:00:00Z'; do
    set -- $r
    printf '{"type":"touch","id":"%s","fetchedAt":"%s"}\n' "$1" "$2" \
        >> prov/.kb/documents.jsonl
done

t "a touch carrying an old date does not stale the index"
# §2: re-filing unchanged content re-indexes nothing, so fetchedAt is outside
# the digest — which is what lets the ages above be planted at all.
expect_grep '"current":true' kbv status --json

t "the default threshold is §5's own 90d and is reported with the answer"
out=$(kbv ls --json)
has "default" "$out" '"olderThan":"90d"'
out=$(kbv status --json)
has "default" "$out" '"olderThan":"90d"'

# A document's verdict is matched through its own fetchedAt, which is unique
# and sits immediately beside `stale` in every shape that carries both. A
# pattern anchored on the id would let a greedy match read a later row's
# verdict as this one's, which is exactly the confusion these tests exist to
# rule out.
OLD_A='"fetchedAt":"2005-01-01T00:00:00Z","indexedAt":"[^"]*"'
OLD_B='"fetchedAt":"2023-06-15T12:00:00Z","indexedAt":"[^"]*"'

t "ls and get agree with search about which documents are stale"
# One definition of stale (§5): the same three documents, the same three
# verdicts, whichever route asked.
ls_out=$(kbv ls --json)
has "ls old" "$ls_out" "$OLD_A,\"stale\":true"
has "ls old" "$ls_out" "$OLD_B,\"stale\":true"
has "get old" "$(kbv get D-1 --json)" "$OLD_A,\"stale\":true"
has "get old" "$(kbv get D-2 --json)" "$OLD_B,\"stale\":true"
has "get fresh" "$(kbv get D-3 --json)" '"stale":false'
srch=$(kbv search zzprov --json)
has "search old" "$srch" '"fetchedAt":"2005-01-01T00:00:00Z","stale":true'
has "search old" "$srch" '"fetchedAt":"2023-06-15T12:00:00Z","stale":true'
has "search fresh" "$srch" '"stale":false'
expect_grep '(stale)' kbv search zzprov
expect_grep '(stale)' kbv get D-1

t "the threshold moves the verdict, and the unit is part of it"
# The same three documents under three thresholds. 5000 days is about
# thirteen years and 20000 about fifty-five, so a parser that read the number
# and dropped the unit would be comparing against seconds and would answer
# "stale" to every one of them.
out=$(kbv ls --older-than 20000d --json)
has "very wide" "$out" '"olderThan":"20000d"'
hasnt "very wide" "$out" '"stale":true'
out=$(kbv ls --older-than 5000d --json)
has "middle" "$out" "$OLD_A,\"stale\":true"
has "middle" "$out" "$OLD_B,\"stale\":false"
out=$(kbv ls --json)
has "default" "$out" "$OLD_B,\"stale\":true"

t "olderThan accepts the query-string spelling too"
out=$(kbv ls --olderThan=20000d --json)
has "eq form" "$out" '"olderThan":"20000d"'

t "a list route's answer is a function of the store, not of the clock"
# `stale` is on every row, but the cutoff INSTANT is not in a list payload:
# a search answer has to be reproducible, which is what makes "rebuild gives
# the same answer" checkable at all.
hasnt "no clock" "$(kbv ls --json)" '"staleBefore"'
hasnt "no clock" "$(kbv search zzprov --json)" '"staleBefore"'

t "a duration without a unit is refused rather than guessed at"
expect_code 1 kbv stale --older-than 90
expect_grep '"error":"usage"' kbv stale --older-than 90 --json
expect_grep 's m h d w' kbv stale --older-than 90 --json
expect_grep '"error":"usage"' kbv stale --older-than 1y --json
expect_grep '"error":"usage"' kbv search zzprov --older-than nope --json

t "kb stale returns the documents past the threshold and nothing else"
out=$(kbv stale --json)
has "stale" "$out" '"id":"D-1"'
has "stale" "$out" '"id":"D-2"'
hasnt "stale" "$out" '"id":"D-3"'
has "stale" "$out" '"count":2'
has "stale" "$out" '"olderThan":"90d"'
has "stale" "$out" '"staleBefore":'
# Every row is one §5 would return: it carries its age and its verdict.
hasnt "stale rows" "$out" '"stale":false'

t "kb stale orders newest sources first"
# S-2 was touched to 2023 and S-1 to 2020, so S-2's documents come first.
order=$(printf '%s' "$out" | grep -o '"id":"D-[0-9]*"' | \
    sed 's/.*"\(D-[0-9]*\)"/\1/' | tr '\n' ' ')
[ "$order" = "D-2 D-1 " ] || fail "expected D-2 then D-1, got: $order"
# And the sort key is shown rather than implied.
has "stale key" "$out" '"sourceFetchedAt":"2023-06-15T12:00:00Z"'

t "kb stale narrows by collection and by limit"
out=$(kbv stale --collection old-topic --json)
has "stale collection" "$out" '"count":2'
out=$(kbv stale --collection fresh-topic --json)
has "stale collection" "$out" '"count":0'
out=$(kbv stale --limit 1 --json)
has "stale limit" "$out" '"count":1'
has "stale limit" "$out" '"id":"D-2"'
out=$(kbv stale --older-than 20000d --json)
has "stale wide" "$out" '"count":0'
expect_grep 'nothing older than' kbv stale --older-than 20000d

t "kb refresh says plainly that it is a report and not an action"
out=$(kbv refresh --json)
has "refresh" "$out" '"action":"report"'
has "refresh" "$out" '"refetched":0'
has "refresh" "$out" '"reembedded":0'
has "refresh" "$out" 'no HTTP client'
has "refresh" "$out" '"count":2'
has "refresh" "$out" '"staleDocuments":2'
has "refresh" "$out" '"id":"S-1"'
has "refresh" "$out" '"id":"S-2"'
hasnt "refresh" "$out" '"id":"S-3"'
expect_grep 'fetched nothing' kbv refresh

t "refresh refuses a flag it would only ignore"
# --limit narrows a list of documents and means nothing to a report about
# sources. Accepting it silently would let a caller believe it was heard.
expect_grep '"error":"usage"' kbv refresh --limit 1 --json
expect_ok kbv stale --limit 1 --json

t "kb refresh writes nothing at all"
# A report is a read. It must not take the write lock, must not append, and
# must not touch the index.
before_docs=$(wc -c < prov/.kb/documents.jsonl)
before_srcs=$(wc -c < prov/.kb/sources.jsonl)
before_idx=$(sha_of prov/.kb/index/fts.db)
kbv refresh --json > /dev/null
kbv refresh --older-than 1s --json > /dev/null
[ "$(wc -c < prov/.kb/documents.jsonl)" = "$before_docs" ] || \
    fail "refresh appended to documents.jsonl"
[ "$(wc -c < prov/.kb/sources.jsonl)" = "$before_srcs" ] || \
    fail "refresh appended to sources.jsonl"
[ "$(sha_of prov/.kb/index/fts.db)" = "$before_idx" ] || \
    fail "refresh rewrote the index"

t "refresh names how each source could be read again, or that it cannot"
# §12.2's resolution made visible: a url is the route that cannot be walked
# from here, a file is still on disk, and inline content has nowhere to go
# back to at all.
printf 'zzprov a filed page about ports\n' > prov/page.md
kbv add --title Page --collection routes --url https://example.test/p \
    --file page.md --json > /dev/null
kbv add --title Local --collection routes --file page.md --json > /dev/null
printf 'zzprov a note handed straight over\n' | \
    kbv add --title Inline --collection routes --json > /dev/null
for d in D-4 D-5 D-6; do
    printf '{"type":"touch","id":"%s","fetchedAt":"2009-05-05T00:00:00Z"}\n' \
        "$d" >> prov/.kb/documents.jsonl
done
out=$(kbv refresh --json)
has "route url" "$out" '"kind":"url","locator":"https://example.test/p","collection":"routes","staleDocuments":1,"fetchedAt":"2009-05-05T00:00:00Z","refetchBy":"post-documents"'
has "route file" "$out" '"kind":"file","locator":"[^"]*page.md","collection":"routes","staleDocuments":1,"fetchedAt":"2009-05-05T00:00:00Z","refetchBy":"re-read"'
has "route inline" "$out" '"kind":"inline".*"refetchBy":"none"'
has "route count" "$out" '"count":5'

t "status counts what is stale and says against which threshold"
out=$(kbv status --json)
has "status stale" "$out" '"stale":{"documents":5,"olderThan":"90d","before":"'
out=$(kbv status --older-than 20000d --json)
has "status wide" "$out" '"stale":{"documents":0'

# ============================================================== §6 links
t "a link is refused unless it is one of §6's five types"
expect_code 1 kbv links add D-1 relates_to D-2
expect_grep '"error":"usage"' kbv links add D-1 relates_to D-2 --json
expect_grep 'supersedes, cites, analogue_of, implements or see_also' \
    kbv links add D-1 relates_to D-2 --json
expect_grep '"error":"usage"' kbv links add D-1 Supersedes D-2 --json
expect_grep '"error":"usage"' kbv links add D-1 '' D-2 --json
for ty in supersedes cites analogue_of implements see_also; do
    expect_ok kbv links add D-1 "$ty" D-2 --json
    expect_ok kbv links delete D-1 "$ty" D-2 --json
done

t "a link to a document that does not exist is refused at write time"
expect_code 1 kbv links add D-1 cites D-9999
expect_grep '"error":"not_found"' kbv links add D-1 cites D-9999 --json
expect_grep '"error":"not_found"' kbv links add D-9999 cites D-1 --json
# and nothing was written
expect_grep '"outgoing":\[\]' kbv links D-1 --json

t "both ends must be document ids, and they must differ"
expect_grep '"error":"usage"' kbv links add S-1 cites D-2 --json
expect_grep '"error":"usage"' kbv links add D-1 cites C-2 --json
expect_grep '"error":"usage"' kbv links add D-1 cites D-1 --json
expect_grep '"error":"usage"' kbv links add D-1 cites --json
expect_grep '"error":"usage"' kbv links --json

t "a link is written once and reads from both ends"
out=$(kbv links add D-1 analogue_of D-2 --json)
has "link" "$out" '"action":"add"'
has "link" "$out" '"changed":true'
has "link" "$out" '"from":"D-1","type":"analogue_of","to":"D-2"'
out=$(kbv links add D-1 analogue_of D-2 --json)
has "link idempotent" "$out" '"changed":false'
out=$(kbv links D-1 --json)
has "outgoing" "$out" '"outgoing":\[{"type":"analogue_of","from":"D-1","to":"D-2","resolved":true'
has "outgoing" "$out" '"incoming":\[\]'
out=$(kbv links D-2 --json)
has "incoming" "$out" '"incoming":\[{"type":"analogue_of","from":"D-1","to":"D-2","resolved":true'
has "incoming" "$out" '"outgoing":\[\]'

t "a link resolves to a row, not to an identifier"
# §6: "outgoing and incoming, resolved to rows". A caller that had to fetch
# each neighbour to learn its title would make one call per edge.
out=$(kbv links D-1 --json)
has "resolved" "$out" '"document":{"id":"D-2"'
has "resolved" "$out" '"title":"B"'
has "resolved" "$out" '"collection":"old-topic"'
has "resolved" "$out" '"stale":true'

t "the triple is the identity: type and direction both count"
kbv links add D-1 cites D-2 --json > /dev/null
kbv links add D-2 cites D-1 --json > /dev/null
out=$(kbv links D-1 --json)
has "triple" "$out" '"type":"analogue_of"'
has "triple" "$out" '"type":"cites","from":"D-1","to":"D-2"'
has "triple" "$out" '"type":"cites","from":"D-2","to":"D-1"'
expect_grep '"links":3' kbv status --json

t "get ?include=links returns both directions"
out=$(kbv get D-1 --include links --json)
has "include links" "$out" '"links":{"outgoing":\['
has "include links" "$out" '"incoming":\['
out=$(kbv get D-1 --json)
hasnt "no include" "$out" '"links"'

t "removing a link removes exactly that edge"
out=$(kbv links delete D-1 cites D-2 --json)
has "unlink" "$out" '"action":"delete"'
has "unlink" "$out" '"changed":true'
out=$(kbv links D-1 --json)
hasnt "unlink" "$out" '"type":"cites","from":"D-1"'
has "unlink" "$out" '"type":"analogue_of"'
has "unlink" "$out" '"type":"cites","from":"D-2"'
expect_grep '"links":2' kbv status --json

t "removing a link that is not there is not_found"
expect_code 1 kbv links delete D-1 cites D-2
expect_grep '"error":"not_found"' kbv links delete D-1 cites D-2 --json

t "a link whose target is later forgotten does not break a read"
# The edge was legal when written. Losing the far end must leave a row that
# says so, not an error and not a silence.
# Backed up BEFORE the edge is added, so restoring puts the store back to
# exactly the two links it had.
cp prov/.kb/documents.jsonl prov/docs.bak
kbv links add D-1 see_also D-3 --json > /dev/null
grep -v '"type":"document","id":"D-3"' prov/.kb/documents.jsonl > prov/d.tmp
mv prov/d.tmp prov/.kb/documents.jsonl
out=$(kbv links D-1 --json)
has "dangling" "$out" '"to":"D-3","resolved":false,"document":null'
has "dangling" "$out" '"ok":true'
expect_grep '(forgotten)' kbv links D-1
# and the dangling edge can still be removed
expect_ok kbv links delete D-1 see_also D-3 --json
cp prov/docs.bak prov/.kb/documents.jsonl

t "a link writes no index and costs no rebuild"
sha_before=$(sha_of prov/.kb/index/fts.db)
kbv links add D-1 implements D-2 --json > /dev/null
kbv links delete D-1 implements D-2 --json > /dev/null
[ "$(sha_of prov/.kb/index/fts.db)" = "$sha_before" ] || \
    fail "a link rewrote the keyword index"
expect_grep '"current":true' kbv status --json

t "links survive losing index/ entirely"
# §1.6: everything under index/ is a cache. The adjacency is folded from the
# log, so there is nothing under index/ for it to lose.
links_before=$(kbv links D-1 --json)
search_before=$(kbv search zzprov --json)
rm -rf prov/.kb/index
[ -d prov/.kb/index ] && fail "index/ was not removed"
expect_ok kbv rebuild
links_after=$(kbv links D-1 --json)
search_after=$(kbv search zzprov --json)
[ "$links_before" = "$links_after" ] || fail "links changed across a rebuild
--- before ---
$links_before
--- after ---
$links_after"
[ "$search_before" = "$search_after" ] || fail "search changed across a rebuild
--- before ---
$search_before
--- after ---
$search_after"

t "an older build reads a store with links in it and simply sees none"
# The loader skips record kinds it does not know, which is what let links go
# into documents.jsonl beside documents and touches.
printf '{"type":"from-a-later-build","from":"D-1","to":"D-2"}\n' \
    >> prov/.kb/documents.jsonl
expect_grep '"ok":true' kbv ls --json
expect_grep '"links":2' kbv status --json

# ============================================================ §7 maintenance
t "stats reports documents, chunks and bytes per collection"
out=$(kbv stats --json)
has "stats" "$out" '"name":"old-topic","documents":2'
has "stats" "$out" '"chunks":'
has "stats" "$out" '"bytes":'
has "stats" "$out" '"totals":{"documents":'
# the totals are the rows added up
total=$(printf '%s' "$out" | sed -n 's/.*"totals":{"documents":\([0-9]*\).*/\1/p')
rows=$(kbv ls --json | \
    sed -n 's/.*"count":\([0-9]*\).*/\1/p')
[ "$total" = "$rows" ] || fail "stats totals $total against $rows documents"

t "a collection is renamed, and the rename is visible everywhere"
expect_grep '"error":"not_found"' kbv collections rename nosuch other --json
expect_grep '"error":"usage"' kbv collections rename old-topic --json
expect_grep '"error":"usage"' kbv collections rename old-topic a/b --json
expect_grep '"error":"usage"' kbv collections rename old-topic old-topic --json
out=$(kbv collections rename old-topic renamed --json)
has "rename" "$out" '"action":"rename"'
has "rename" "$out" '"renamedTo":"renamed"'
has "rename" "$out" '"merged":false'
has "rename" "$out" '"sources":2'
expect_grep '"name":"renamed"' kbv collections --json
expect_not_grep '"name":"old-topic"' kbv collections --json
expect_grep '"collection":"renamed"' kbv ls --json
expect_grep '"collection":"renamed"' kbv search zzprov --json
out=$(kbv search zzprov --collection renamed --json)
hasnt "rename search" "$out" '"count":0'

t "a rename costs no rebuild"
# A collection is a filter read live from the log; the digest covers what
# decides a chunk's text and identity, and a collection is none of it.
expect_grep '"current":true' kbv status --json

t "renaming onto a name that already exists merges, and says so"
out=$(kbv collections rename fresh-topic renamed --json)
has "merge" "$out" '"merged":true'
expect_not_grep '"name":"fresh-topic"' kbv collections --json

t "a collection that still holds documents is not forgotten"
expect_code 1 kbv collections delete renamed
out=$(kbv collections delete renamed --json)
has "in use" "$out" '"error":"collection_in_use"'
has "in use" "$out" 'still holds 3 document'
has "in use" "$out" '"details":{"collection":"renamed","documents":3}'
expect_grep '"name":"renamed"' kbv collections --json
expect_grep '"error":"not_found"' kbv collections delete nosuch --json

t "a collection with no documents left is forgotten"
# A source with no documents under it is the state §7's DELETE is for: the
# topic exists, and nothing is in it.
printf '{"type":"source","id":"S-90","kind":"inline","locator":"orphan",' \
    > prov/orphan.txt
printf '"title":"o","collection":"emptied","createdAt":"2020-01-01T00:00:00Z"}\n' \
    >> prov/orphan.txt
cat prov/orphan.txt >> prov/.kb/sources.jsonl
out=$(kbv collections delete emptied --json)
has "forget" "$out" '"action":"delete"'
has "forget" "$out" '"sources":1'
has "forget" "$out" '"documents":0'
expect_grep '"error":"not_found"' kbv collections delete emptied --json
expect_not_grep 'emptied' kbv collections --json
expect_grep '"type":"forget","id":"S-90"' cat prov/.kb/sources.jsonl

t "an id is never reused after a source is forgotten"
out=$(kbv status --json)
has "forget ids" "$out" '"source":"S-91"'

t "collections rejects a verb it does not have"
expect_grep '"error":"usage"' kbv collections bogus --json

# ---------------------------------------------------------------- reindex
t "reindex is a no-op when the store was built by this chunker"
before_docs=$(wc -c < prov/.kb/documents.jsonl)
out=$(kbv reindex --json)
has "reindex noop" "$out" '"rechunked":0'
has "reindex noop" "$out" '"reembedded":0'
has "reindex noop" "$out" '"model":null'
has "reindex noop" "$out" 'keyword-only'
[ "$(wc -c < prov/.kb/documents.jsonl)" = "$before_docs" ] || \
    fail "a reindex with nothing to do still appended"

# A store whose parameters really did change. The document is long enough
# that 128-token and 400-token chunking cannot agree.
mkdir -p rechunk
kbr() { ( cd "$WORK/rechunk" && "$KB" "$@" ); }
kbr init > /dev/null
i=0
: > rechunk/long.txt
while [ $i -lt 300 ]; do
    echo "zzchunk a line of prose about completion ports and their queues." \
        >> rechunk/long.txt
    i=$((i + 1))
done
kbr add --title long --collection bulk --file long.txt --json > /dev/null
base_before=$(kbr get D-1 --json | \
    sed -n 's/.*"chunkBase":\([0-9]*\).*/\1/p')
count_before=$(kbr get D-1 --json | \
    sed -n 's/.*"chunkCount":\([0-9]*\).*/\1/p')
next_before=$(kbr status --json | \
    sed -n 's/.*"chunk":"C-\([0-9]*\)".*/\1/p')

t "a store whose chunker moved is detected before a reindex fixes it"
printf '{"chunker":"old","chunkTokens":128,"chunkOverlap":16}\n' \
    > rechunk/.kb/index/model.json
expect_grep '"current":false' kbr status --json
# The log's range no longer describes what the blob splits into.
out=$(kbr rebuild --json)
hasnt "mismatch" "$out" '"mismatched":0'

t "reindex takes a fresh chunk id range and never reuses the old one"
out=$(kbr reindex --json)
has "reindex" "$out" '"rechunked":1'
has "reindex" "$out" '"wasChunkTokens":128'
has "reindex" "$out" '"chunkTokens":400'
base_after=$(kbr get D-1 --json | \
    sed -n 's/.*"chunkBase":\([0-9]*\).*/\1/p')
[ "$base_after" != "$base_before" ] || \
    fail "reindex kept chunk base $base_before"
# Strictly past every id the store had ever handed out: §1.1's ids are never
# reused, and the range that was there described a different splitting.
[ "$base_after" -ge "$next_before" ] || \
    fail "reindex reissued ids from $base_after; the store was at $next_before"

t "reindex rewrites index/model.json and rebuild then obeys it"
expect_grep '"chunkTokens":400' cat rechunk/.kb/index/model.json
expect_grep '"chunker":"structural-1"' cat rechunk/.kb/index/model.json
expect_grep '"current":true' kbr status --json
out=$(kbr rebuild --json)
has "after reindex" "$out" '"mismatched":0'

t "the chunk ids the log records name the passages on disk again"
expect_ok kbr chunk "C-$base_after" --json
out=$(kbr get D-1 --include chunks --json)
has "chunk ids" "$out" "\"id\":\"C-$base_after\""
expect_grep '"current":true' kbr status --json

t "reindex leaves fetchedAt alone and moves indexedAt"
# The text was not fetched again, it was cut up again. Resetting fetchedAt
# would make a three-year-old page look freshly read (§5).
fetched=$(kbr get D-1 --json | \
    sed -n 's/.*"fetchedAt":"\([^"]*\)".*/\1/p')
printf '{"type":"touch","id":"D-1","fetchedAt":"2019-01-01T00:00:00Z"}\n' \
    >> rechunk/.kb/documents.jsonl
printf '{"chunker":"old","chunkTokens":150,"chunkOverlap":16}\n' \
    > rechunk/.kb/index/model.json
kbr reindex --json > /dev/null
out=$(kbr get D-1 --json)
has "fetchedAt kept" "$out" '"fetchedAt":"2019-01-01T00:00:00Z"'
has "still stale" "$out" '"stale":true'
hasnt "indexedAt moved" "$out" "\"indexedAt\":\"2019-01-01T00:00:00Z\""
[ -n "$fetched" ] || fail "no fetchedAt to compare"

t "search still answers correctly after a reindex"
out=$(kbr search zzchunk --json)
hasnt "post reindex" "$out" '"count":0'
has "post reindex" "$out" '"document":"D-1"'

t "reindex reports the one store it rechunked"
out=$(kbr reindex --json)
has "reindex" "$out" '"path":'
hasnt "reindex" "$out" '"stores":'

# ----------------------------------------------------------------- forget
mkdir -p forget
kbf() { ( cd "$WORK/forget" && "$KB" "$@" ); }
kbf init > /dev/null
printf 'zzfirst alpha\n' | kbf add --title first --collection topic --json > /dev/null
printf 'zzsecond beta\n' | kbf add --title second --collection topic \
    --url https://example.test/second --json > /dev/null
printf 'zzthird gamma\n' | kbf add --title third --collection other --json > /dev/null
kbf links add D-2 cites D-1 --json > /dev/null

t "forget removes a document from every read, and the index follows"
out=$(kbf forget D-1 --json)
has "forget" "$out" '"documents":\["D-1"\]'
has "forget" "$out" '"sources":\[\]'
expect_grep '"error":"not_found"' kbf get D-1 --json
expect_not_grep '"id":"D-1"' kbf ls --json
expect_grep '"count":0' kbf search zzfirst --json
expect_grep '"current":true' kbf status --json
expect_grep '"type":"forget","id":"D-1"' cat forget/.kb/documents.jsonl
expect_grep 'forgot D-2' kbf forget D-2

t "a link to a forgotten document reads as unresolved"
printf 'zzfourth\n' | kbf add --title fourth --collection other --json > /dev/null
kbf links add D-4 see_also D-3 --json > /dev/null
kbf forget D-3 --json > /dev/null
expect_grep '"resolved":false' kbf links D-4 --json

t "a forgotten id is never handed out again"
out=$(printf 'zzfirst alpha\n' | kbf add --title first --collection topic --json)
has "reuse" "$out" '"document":"D-5"'

t "forgetting a source forgets every document under it"
src=$(kbf get D-5 --json | sed -n 's/.*"source":"\(S-[0-9]*\)".*/\1/p')
out=$(kbf forget "$src" --json)
has "source" "$out" "\"sources\":\\[\"$src\"\\]"
has "source" "$out" '"documents":\["D-5"\]'
expect_grep '"error":"not_found"' kbf get D-5 --json

t "forget refuses what is not there and what is not an id"
expect_code 1 kbf forget D-99
expect_grep '"error":"not_found"' kbf forget D-99 --json
expect_grep '"error":"not_found"' kbf forget S-99 --json
expect_grep '"error":"usage"' kbf forget C-1 --json
expect_grep '"error":"usage"' kbf forget --json
expect_grep '"error":"usage"' kbf forget D-4 D-5 --json

t "compact then drops the text nothing refers to any more"
out=$(kbf compact --json)
dropped=$(printf '%s' "$out" | sed -n 's/.*"dropped":\([0-9]*\).*/\1/p')
[ "${dropped:-0}" -ge 1 ] || fail "compact dropped nothing after a forget: $out"

t "a collection is forgotten with its documents only when asked to"
printf 'zzsixth\n' | kbf add --title sixth --collection doomed --json > /dev/null
printf 'zzseventh\n' | kbf add --title seventh --collection doomed --json > /dev/null
out=$(kbf collections delete doomed --json)
has "delete" "$out" '"error":"collection_in_use"'
has "delete" "$out" 'with-documents'
out=$(kbf collections delete doomed --with-documents --json)
has "delete" "$out" '"action":"delete"'
has "delete" "$out" '"documents":2'
expect_not_grep '"name":"doomed"' kbf collections --json
expect_grep '"count":0' kbf search zzsixth --json
expect_grep '"current":true' kbf status --json
expect_grep '"error":"usage"' kbf collections rename other elsewhere --with-documents --json

# ------------------------------------------------------------------ batch
mkdir -p batch
kbb2() { ( cd "$WORK/batch" && "$KB" "$@" ); }
kbb2 init > /dev/null

t "a batch files every document under one lock"
out=$(printf '%s\n' \
    '{"title":"one","collection":"b","content":"zzbatchone"}' \
    '{"title":"two","collection":"b","content":"# Two\n\nzzbatchtwo","mime":"text/markdown","meta":{"year":2026}}' \
    '{"title":"page","collection":"b","content":"zzbatchpage","url":"https://example.test/page"}' \
    | kbb2 add --batch --json)
has "batch" "$out" '"count":3'
has "batch" "$out" '"added":\[{"document":"D-1"'
has "batch" "$out" '"splitter":"markdown"'
expect_grep '"count":1' kbb2 search zzbatchtwo --json
expect_grep '"current":true' kbb2 status --json
expect_grep '"year":2026' kbb2 get D-2 --json

t "the same locator twice in one batch is one document"
out=$(printf '%s\n' \
    '{"title":"page","collection":"b","content":"zzpagealpha","url":"https://example.test/page"}' \
    '{"title":"page","collection":"b","content":"zzpagebeta","url":"https://example.test/page"}' \
    | kbb2 add --batch --json)
has "same" "$out" '"document":"D-3","source":"S-3"'
hasnt "same" "$out" '"document":"D-4"'
expect_grep '"count":1' kbb2 search zzpagebeta --json
expect_grep '"count":0' kbb2 search zzpagealpha --json

t "a bad line files nothing at all"
before=$(sha_of batch/.kb/documents.jsonl)
out=$(printf '%s\n' \
    '{"title":"fine","collection":"b","content":"zzfine"}' \
    '{"title":"","collection":"b","content":"x"}' \
    | kbb2 add --batch --json)
has "bad line" "$out" '"error":"usage"'
has "bad line" "$out" 'batch line 2'
[ "$(sha_of batch/.kb/documents.jsonl)" = "$before" ] || fail "a refused batch wrote to the log"
expect_grep '"count":0' kbb2 search zzfine --json
expect_grep '"error":"usage"' sh -c "cd '$WORK/batch' && printf 'not json\n' | '$KB' add --batch --json"
expect_grep '"error":"usage"' sh -c "cd '$WORK/batch' && printf '' | '$KB' add --batch --json"
expect_grep '"error":"usage"' sh -c "cd '$WORK/batch' && printf 'x' | '$KB' add --batch --title t --json"

# ---------------------------------------------------------------- sources
mkdir -p srcs
kbs() { ( cd "$WORK/srcs" && "$KB" "$@" ); }
kbs init > /dev/null
printf '# Notes\n\nzzfileold words\n' > srcs/notes.md
kbs add --title notes --collection research --file notes.md --json > /dev/null
printf 'zzpage\n' | kbs add --title page --collection web --url https://example.test/p --json > /dev/null
printf 'zzinline\n' | kbs add --title inline --collection research --json > /dev/null

t "sources lists every source, narrowed by collection and kind"
out=$(kbs sources --json)
has "sources" "$out" '"count":3'
has "sources" "$out" '"kind":"file"'
has "sources" "$out" '"docCount":1'
expect_grep '"count":2' kbs sources --collection research --json
expect_grep '"count":1' kbs sources --kind url --json
expect_grep 'S-1' kbs sources

t "sources show gives the source, its documents and its fetch history"
out=$(kbs sources show S-1 --json)
has "show" "$out" '"source":{"id":"S-1"'
has "show" "$out" '"documents":\[{"id":"D-1"'
has "show" "$out" '"history":\[{"document":"D-1","fetchedAt":"[^"]*","changed":true}\]'
expect_grep '"error":"not_found"' kbs sources show S-99 --json
expect_grep '"error":"usage"' kbs sources show --json
expect_grep '"error":"usage"' kbs sources show S-1 --kind file --json

t "refreshing an unchanged file source touches it and indexes nothing"
out=$(kbs refresh S-1 --json)
has "refresh" "$out" '"changed":false'
has "refresh" "$out" '"document":"D-1"'
expect_grep '"changed":false}\]' kbs sources show S-1 --json

t "refreshing a changed file source re-indexes it under the same document"
printf '# Notes\n\nzzfilenew words\n' > srcs/notes.md
out=$(kbs refresh S-1 --json)
has "refresh" "$out" '"changed":true'
has "refresh" "$out" '"document":"D-1"'
expect_grep '"count":1' kbs search zzfilenew --json
expect_grep '"count":0' kbs search zzfileold --json
expect_grep '"current":true' kbs status --json
expect_grep 'changed, re-indexed' sh -c "printf '# Notes\n\nagain\n' > '$WORK/srcs/notes.md' && cd '$WORK/srcs' && '$KB' refresh S-1"

t "a source that cannot be read again says why"
mv srcs/notes.md srcs/notes.moved
expect_grep '"error":"fetch_failed"' kbs refresh S-1 --json
expect_grep '"details":{"locator":"[^"]*notes.md"}' kbs refresh S-1 --json
expect_grep '"status":"fetch_failed"' kbs sources show S-1 --json
expect_grep '"count":1' kbs sources --status fetch_failed --json
expect_grep '"error":"fetch_failed"' kbs refresh S-2 --json
expect_grep 'kb add --url' kbs refresh S-2 --json
expect_grep '"error":"usage"' kbs refresh S-3 --json
expect_grep '"error":"not_found"' kbs refresh S-99 --json
expect_grep '"error":"usage"' kbs refresh S-1 --collection research --json
expect_grep '"error":"usage"' kbs refresh D-1 --json

t "a source read again after a failure is ok again"
mv srcs/notes.moved srcs/notes.md
expect_ok kbs refresh S-1
expect_grep '"status":"ok"' kbs sources show S-1 --json
expect_grep '"count":0' kbs sources --status fetch_failed --json

# ---------------------------------------------------------------- compact
mkdir -p comp
kbc() { ( cd "$WORK/comp" && "$KB" "$@" ); }
kbc init > /dev/null
printf 'zzcomp the first version of this page\n' > comp/p.md
kbc add --title P --collection c --file p.md --json > /dev/null
live_before=$(kbc get D-1 --json | \
    sed -n 's/.*"contentHash":"\([0-9a-f]*\)".*/\1/p')
printf 'zzcomp the second version of this page, rewritten\n' > comp/p.md
kbc add --title P --collection c --file p.md --json > /dev/null
live=$(kbc get D-1 --json | \
    sed -n 's/.*"contentHash":"\([0-9a-f]*\)".*/\1/p')

t "a superseded blob is still on disk before a compact"
[ "$live" != "$live_before" ] || fail "the content did not change"
[ -f "comp/.kb/blobs/$live_before" ] || fail "the old blob is already gone"
n=$(ls comp/.kb/blobs | wc -l | tr -d ' ')
[ "$n" = "2" ] || fail "expected 2 blobs, found $n"

t "compact drops the superseded blob and keeps the live one"
out=$(kbc compact --json)
has "compact" "$out" '"dropped":1'
has "compact" "$out" '"kept":1'
hasnt "compact" "$out" '"bytesFreed":0'
[ -f "comp/.kb/blobs/$live" ] || fail "compact removed a referenced blob"
[ -f "comp/.kb/blobs/$live_before" ] && fail "the superseded blob survived"
expect_grep 'zzcomp the second version' kbc get D-1 --include text \
   
expect_ok kbc search zzcomp --json

t "a second compact has nothing left to do"
out=$(kbc compact --json)
has "compact again" "$out" '"dropped":0'
has "compact again" "$out" '"kept":1'

t "compact never removes a blob a live document references"
# Every document's blob, checked against the directory after the sweep.
kbc add --title Q --collection c --json <<'EOT' > /dev/null
zzcomp a second document with its own blob
EOT
kbc compact --json > /dev/null
for h in $(kbc ls --json | \
        grep -o '"contentHash":"[0-9a-f]*"' | \
        sed 's/.*"\([0-9a-f]*\)"/\1/'); do
    [ -f "comp/.kb/blobs/$h" ] || fail "compact removed live blob $h"
done
expect_ok kbc rebuild

t "compact leaves alone anything that is not a blob"
# A crashed atomic write can leave a temp file behind, and a person can put
# something here. compact drops superseded BLOBS, and a file this directory
# cannot explain is not one.
printf 'not a blob\n' > comp/.kb/blobs/README
printf 'half a write\n' > comp/.kb/blobs/abc.tmp.999
out=$(kbc compact --json)
has "skip" "$out" '"skipped":2'
has "skip" "$out" '"dropped":0'
[ -f comp/.kb/blobs/README ] || fail "compact deleted a file it cannot explain"
[ -f comp/.kb/blobs/abc.tmp.999 ] || fail "compact deleted a temp file"
rm -f comp/.kb/blobs/README comp/.kb/blobs/abc.tmp.999

t "compact and reindex are refused while another process holds the lock"
if [ -n "$KB_TESTS" ]; then
    rm -f compctl
    mkfifo compctl
    "$KB_TESTS" --hold-lock "$WORK/comp/.kb" < compctl > compout.txt 2>&1 &
    holder=$!
    exec 9> compctl
    i=0
    while [ $i -lt 200 ]; do
        if grep -q locked compout.txt 2>/dev/null; then break; fi
        sleep 0.05
        i=$((i + 1))
    done
    if grep -q locked compout.txt 2>/dev/null; then
        expect_grep '"error":"store_locked"' kbc compact --json
        expect_grep '"error":"store_locked"' kbc reindex --json
        expect_grep '"error":"store_locked"' kbc collections rename c d --json
        # A read is never blocked by a writer.
        expect_ok kbc stale --json
        expect_ok kbc refresh --json
        expect_ok kbc links D-1 --json
        expect_ok kbc stats --json
    else
        fail "the lock holder never started: $(cat compout.txt)"
    fi
    exec 9>&-
    wait $holder
    rm -f compctl
fi

# ------------------------------------------------------------------ model
# Needs the real weights, which a test cannot make: KB_TEST_MODEL names a
# nomic-embed-text GGUF, linked into this run's throwaway HOME.
if [ -n "${KB_TEST_MODEL:-}" ] && [ -f "$KB_TEST_MODEL" ]; then
    mkdir -p "$HOME/.kb/models" mdl
    ln "$KB_TEST_MODEL" "$HOME/.kb/models/" 2>/dev/null ||
        cp "$KB_TEST_MODEL" "$HOME/.kb/models/"
    kbm() { ( cd "$WORK/mdl" && "$KB" "$@" ); }
    kbm init > /dev/null

    t "the model is recorded at first ingest, with its file's hash"
    printf 'zzmodel text\n' | kbm add --title m --collection c > /dev/null
    out=$(cat mdl/.kb/index/model.json)
    has "model.json" "$out" '"model":"nomic-embed-text-v1.5"'
    has "model.json" "$out" '"dim":768'
    has "model.json" "$out" '"queryPrefix":"search_query: "'
    has "model.json" "$out" '"documentPrefix":"search_document: "'
    has "model.json" "$out" '"chunkTokens":400'
    has "model.json" "$out" "\"sha256\":\"$(sha_of "$KB_TEST_MODEL")\""
    has "status current" "$(kbm status --json)" '"current":true}'

    t "another recorded configuration is model_mismatch until reindex records this one"
    sed -i.bak 's/"weights":"[^"]*"/"weights":"F16"/' mdl/.kb/index/model.json
    has "status mismatch" "$(kbm status --json)" '"current":false}'
    out=$(kbm search zzmodel --mode hybrid --json)
    has "mismatch" "$out" '"error":"model_mismatch"'
    has "mismatch" "$out" '"details":{"stored":{"model":"nomic-embed-text-v1.5"'
    has "mismatch" "$out" 'weights'
    has "keyword unaffected" "$(kbm search zzmodel --json)" '"count":1'
    has "reindex records" "$(kbm reindex --json)" '"model":"nomic-embed-text-v1.5"'
    has "status current again" "$(kbm status --json)" '"current":true}'

    t "every chunk has a vector, and hybrid is what a plain search runs"
    [ -f mdl/.kb/index/vectors.bin ] || fail "no vectors.bin after reindex"
    printf '# Sourdough\n\nFeed the starter flour and water, then let the dough rise overnight.\n' |
        kbm add --title bread --collection food > /dev/null
    printf '# io_uring\n\nThe kernel shares a submission ring and a completion ring with the process.\n' |
        kbm add --title uring --collection io > /dev/null
    out=$(kbm search zzmodel --json)
    has "default hybrid" "$out" '"mode":"hybrid"'
    has "both paths" "$out" '"matched":\["keyword","semantic"\]'
    has "both scores" "$out" '"scores":{"bm25":[0-9.]*,"vector":[0-9.-]*,"fused"'

    t "the vector path finds what shares no word with the query"
    out=$(kbm search "how do I bake bread at home" --mode semantic --json)
    has "semantic" "$out" '"mode":"semantic"'
    first=$(printf '%s' "$out" | grep -o '"title":"[^"]*"' | head -1)
    [ "$first" = '"title":"bread"' ] || fail "semantic search ranked $first first for baking bread"
    hasnt "no bm25" "$out" '"bm25"'
    has "filtered" "$(kbm search "how do I bake bread at home" --mode semantic --collection io --json)" '"title":"uring"'
    hasnt "filtered" "$(kbm search "how do I bake bread at home" --mode semantic --collection io --json)" '"title":"bread"'

    t "rebuild keeps every vector it already has, and a forgotten chunk's goes"
    has "rebuild keeps" "$(kbm rebuild --json)" '"vectors":{"kept":3,"embedded":0,"dropped":0}'
    kbm forget D-2 > /dev/null
    has "rebuild drops" "$(kbm rebuild --json)" '"vectors":{"kept":2,"embedded":0,"dropped":1}'

    t "chunks without vectors make vector search refuse until rebuild embeds them"
    rm mdl/.kb/index/vectors.bin
    out=$(kbm search baking --mode hybrid --json)
    has "stale" "$out" '"structures":\["vectors"\],"missing":2'
    has "fallback" "$(kbm search zzmodel --json)" '"mode":"keyword"'
    has "rebuild embeds" "$(kbm rebuild --json)" '"embedded":2'
    has "hybrid again" "$(kbm search zzmodel --json)" '"mode":"hybrid"'
fi

t "kb still writes nothing outside its own store"
[ "$(cat .lap/log.jsonl)" = "lap log line" ] || fail ".lap was modified"
[ "$(cat .coboard/log.jsonl)" = "coboard log line" ] || fail ".coboard was modified"
n=$(ls -a .lap | wc -l | tr -d ' ')
[ "$n" = "3" ] || fail ".lap gained entries"
n=$(ls -a .coboard | wc -l | tr -d ' ')
[ "$n" = "3" ] || fail ".coboard gained entries"

# ------------------------------------------------------------------- done
echo "e2e: $TESTS tests, $FAILED failed"
[ "$FAILED" -eq 0 ] || exit 1
exit 0
