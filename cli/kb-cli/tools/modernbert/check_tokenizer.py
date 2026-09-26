#!/usr/bin/env python3
"""Checks kb's byte-level BPE (bin/kb-bpe-dump) against the Hugging Face
tokenizer, id for id, over a list of text files, and reports each file that
differs with the first place it differs.

    python check_tokenizer.py <embedder-snapshot> <model.gguf> <kb-bpe-dump> <file>...

Exit status 0 when every file matches.
"""

import json
import subprocess
import sys

from transformers import AutoTokenizer


def main():
    snap, model, dump, files = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    tok = AutoTokenizer.from_pretrained(snap)
    ours = {}
    for i in range(0, len(files), 200):
        out = subprocess.run([dump, model, *files[i:i + 200]], capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            row = json.loads(line)
            ours[row["file"]] = row["ids"]
    bad = 0
    total_tokens = 0
    for f in files:
        with open(f, "rb") as fh:
            text = fh.read().decode("utf-8", errors="replace")
        ref = tok(text)["input_ids"]
        got = ours.get(f)
        total_tokens += len(ref)
        if got == ref:
            continue
        bad += 1
        if got is None:
            print(f"MISSING {f}")
            continue
        k = next((i for i, (x, y) in enumerate(zip(got, ref)) if x != y), min(len(got), len(ref)))
        lo = max(0, k - 3)
        print(f"DIFF {f}: {len(got)} vs {len(ref)} tokens, first at {k}")
        print(f"   kb:  {got[lo:k + 5]}  {tok.convert_ids_to_tokens(got[lo:k + 5])}")
        print(f"   ref: {ref[lo:k + 5]}  {tok.convert_ids_to_tokens(ref[lo:k + 5])}")
    print(f"{len(files) - bad}/{len(files)} files match, {total_tokens} reference tokens")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
