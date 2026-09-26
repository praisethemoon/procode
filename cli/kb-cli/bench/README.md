# kb's retrieval benchmark

How often `kb search` puts the right place first, over this repository's own
documentation and code. It is not part of `make test`: building a store
embeds several thousand chunks and takes minutes.

| file | holds |
|---|---|
| `corpus.json` | the pinned commit and the files taken from it: 13 documents, 195 source files (C, TypeScript/TSX, JavaScript, Python) |
| `queries/docs.jsonl`, `queries/code.jsonl` | the queries, each tagged and labelled with the lines that answer it |
| `bench.py` | builds a store from the corpus and scores search against the labels |
| `results/` | one JSON per recorded run: the summary and every query's rank per mode |

## Running it

```sh
make -C cli/kb-cli                      # the kb being measured
python3 cli/kb-cli/bench/bench.py --check                      # validate the labels
python3 cli/kb-cli/bench/bench.py --name gte-baseline \
    --model ~/.kb/models/gte-modernbert-base.F16.gguf --store /tmp/kb-bench-gte
python3 cli/kb-cli/bench/bench.py --compare results/nomic-baseline.json results/gte-baseline.json
```

The model file is only read: it is hard-linked (or copied) into a throwaway
HOME, and kb never sees the real home directory. `--store` keeps the built
store and reuses it while the commit, the corpus, the model and the kb binary
are unchanged; without it every run builds a new one in a temporary folder.
Without `--model` only keyword search runs.

## What is measured

A query is a line of JSON:

```json
{"id": "da012", "q": "what stops two writers appending at once", "tag": "paraphrase",
 "rel": [{"path": "specs/index-api.md", "lines": [141, 152]}]}
```

- **tag** says what kind of search it is: `identifier` names something exact
  (a function, a flag, an error code); `paraphrase` asks about what a passage
  says in other words; `purpose` asks why, or how to get something done.
- **rel** is one to three line ranges. A hit is right when its chunk overlaps
  one of them. Labels are lines rather than chunk ids so that a change to the
  chunker can be measured instead of breaking the answer key.

For each query set, tag and mode (keyword, semantic, hybrid) the run reports
hit@1 (the first hit is right), hit@3 and MRR@10, each with a bootstrap 95%
interval over the queries. `--compare` pairs two runs query by query and gives
a bootstrap interval on each difference; a difference whose interval contains
zero has not been shown.

## How the queries were made

Agents drafted them from the pinned files, each told to phrase paraphrase and
purpose queries in words the passage does not use. A second pass read every
label against the lines it names and fixed or dropped the ones that were
wrong, vague, or answered better elsewhere. The queries are written by people
who had read the corpus, which makes them easier than a stranger's; compare
runs against each other, not against numbers measured elsewhere.

## Changing the corpus

A new commit or file list changes every line number. Re-pin only together with
re-checking the labels, and never compare results across two pins.
