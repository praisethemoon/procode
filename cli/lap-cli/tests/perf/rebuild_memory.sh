#!/bin/sh
# Peak memory of `lap rebuild`, `lap verify` and `lap verify --deep` on a
# generated history.
#
#   tests/perf/rebuild_memory.sh <lap> <folder> [megabytes]
#
# Writes a history of about <megabytes> MB (default 500) into <folder>, a
# new folder: FILES files created with LINES lines each, then edits that
# each replace one line with a long one, chained and chunked as lap writes
# them. Then runs `lap rebuild` and `lap verify --deep` there and prints the
# history's size and rebuild's peak resident set size, in MB, and fails when
# the peak passes its bound: 64 MB plus twelve times the index. Rebuild
# keeps one file's replayed state and one chunk at a time; what grows with
# the history is the index (64 bytes a record) and what is sized by its
# entries, held while it is built and again when it is loaded (measured:
# 41 MB at 100 MB of history, 117 MB at 500 MB, 174 MB at 1 GB). The folder is left for inspection; remove it yourself.
set -e
LAP=$1
DIR=$2
MB=${3:-500}
[ -n "$LAP" ] && [ -n "$DIR" ] || { echo "usage: $0 <lap> <folder> [megabytes]" >&2; exit 2; }
[ -e "$DIR" ] && { echo "$DIR exists; give a new folder" >&2; exit 2; }
mkdir -p "$DIR/.lap/log"
python3 - "$DIR" "$MB" <<'PY'
import hashlib, json, os, random, sys

root, mb = sys.argv[1], int(sys.argv[2])
FILES, LINES, LONG = 20, 1000, 2400
LIMIT = 4 * 1024 * 1024
random.seed(7)
chunk_no, chunk, size, prev = 1, None, 0, "0" * 64
total, target = 0, mb * 1024 * 1024

def put(rec):
    global chunk_no, chunk, size, prev, total
    rec["ts"] = "2026-09-28T00:00:00Z"
    rec["prev"] = prev
    line = json.dumps(rec, separators=(",", ":"), ensure_ascii=False).encode()
    if chunk is None or (size > 0 and size + len(line) + 1 > LIMIT):
        if chunk is not None:
            chunk.close()
            chunk_no += 1
        chunk = open(os.path.join(root, ".lap/log/main.%06d.jsonl" % chunk_no), "wb")
        size = 0
    chunk.write(line + b"\n")
    size += len(line) + 1
    total += len(line) + 1
    prev = hashlib.sha256(line).hexdigest()

put({"type": "init", "version": 1})
files = {}
n = 0
def commit(path, op, start, old, new):
    global n
    n += 1
    put({"type": "commit", "id": "L%d" % n, "user": "perf", "session": None,
         "file": path, "op": op, "old_start": start, "old_lines": len(old),
         "new_start": start, "new_lines": len(new), "eof_nl": True,
         "old_text": old, "new_text": new,
         "intent": "Grow a history to measure rebuild.",
         "behavior": "Rewrites one line of %s." % path})
for f in range(FILES):
    path = "src/f%02d.txt" % f
    files[path] = ["line %d of %s" % (i, path) for i in range(LINES)]
    commit(path, "create", 1, [], files[path])
while total < target:
    path = "src/f%02d.txt" % random.randrange(FILES)
    i = random.randrange(LINES)
    new = "%d:" % n + "x" * LONG
    commit(path, "edit", i + 1, [files[path][i]], [new])
    files[path][i] = new
chunk.close()
for path, lines in files.items():
    os.makedirs(os.path.join(root, os.path.dirname(path)), exist_ok=True)
    open(os.path.join(root, path), "w").write("\n".join(lines) + "\n")
PY
cd "$DIR"
HIST=$(cat .lap/log/*.jsonl | wc -c)
# peak_mb <command...>: the command's peak resident set size, in MB
peak_mb() {
    if [ "$(uname)" = Darwin ]; then
        P=$( { /usr/bin/time -l "$@" >/dev/null; } 2>&1 | awk '/maximum resident set size/ {print $1}')
        echo $((P / 1024 / 1024))
    else
        P=$( { /usr/bin/time -v "$@" >/dev/null; } 2>&1 | awk -F: '/Maximum resident set size/ {print $2}')
        echo $((P / 1024))
    fi
}
REBUILD=$(peak_mb "$LAP" rebuild)
"$LAP" verify --deep | tail -n 1
VERIFY=$(peak_mb "$LAP" verify)
DEEP=$(peak_mb "$LAP" verify --deep)
BOUND_MB=$((64 + 12 * $(wc -c < .lap/index) / 1024 / 1024))
echo "history: $((HIST / 1024 / 1024)) MB, peaks: rebuild $REBUILD MB, verify $VERIFY MB, verify --deep $DEEP MB (bound $BOUND_MB MB)"
for p in "$REBUILD" "$VERIFY" "$DEEP"; do
    [ "$p" -le "$BOUND_MB" ] || { echo "over the bound" >&2; exit 1; }
done
