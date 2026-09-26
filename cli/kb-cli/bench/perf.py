#!/usr/bin/env python3
"""kb's performance benchmark: what filing and searching cost as a store grows.

    python3 perf.py --model <embedder.gguf> [--reranker <reranker.gguf>]
                    [--sizes 1000,10000] [--out results/perf.json]

For each size it builds a synthetic store of about that many chunks —
deterministic Markdown from a seeded generator, so two runs file the same
text — in a throwaway folder with a throwaway HOME, and times:

  file          the whole corpus as one batch, keyword index only
                (--embed-budget 0), then `kb embed` for the vectors
  add one       one more document of about 20 chunks, embedded (--wait)
  add batch     20 more documents in one batch, embedded (--wait)
  keyword       a keyword search (median of the queries)
  hybrid        a hybrid search, with KB_TIMING's split: process start,
                opening the store, mapping the model, loading the keyword
                index, the keyword search, embedding the query, the vector
                scan, fusing and rendering
  rerank        a hybrid search with the reranker (when --reranker is given)
  rebuild       `kb rebuild` of the whole store (nothing left to embed)

Python 3.8+, standard library only. Nothing is read from or written to the
real home directory: the models are hard-linked (or copied) into the
throwaway HOME.
"""

import argparse
import json
import os
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WORDS = ("completion port thread queue ring kernel submission buffer socket "
         "event poll timer signal handle file descriptor reactor proactor "
         "latency throughput batch worker scheduler memory page cache lock "
         "mutex atomic fence barrier channel stream packet frame header "
         "payload index vector chunk store search query token model layer "
         "attention embedding rank fusion score document section heading "
         "parser grammar symbol function method class struct module").split()
QUERIES = ["how does the completion port wake worker threads",
           "submission ring batch latency", "which lock guards the page cache",
           "vector index chunk store", "attention layer embedding model",
           "socket event poll timer", "rank fusion score",
           "parser grammar symbol table", "worker scheduler throughput",
           "memory fence and atomic barrier"]


def paragraph(rnd, words):
    return " ".join(rnd.choice(WORDS) for _ in range(words)).capitalize() + "."


def document(rnd, n, sections=10):
    parts = [f"# Notes {n}\n"]
    for s in range(sections):
        parts.append(f"## Part {s} of notes {n}: {rnd.choice(WORDS)} and {rnd.choice(WORDS)}\n")
        for _ in range(4):
            parts.append(paragraph(rnd, 60) + "\n")
    return "\n".join(parts)


def run(kb, args, cwd, home, stdin=None, env_extra=None):
    env = dict(os.environ, HOME=home, **(env_extra or {}))
    env.pop("KB_STORE", None)
    t0 = time.time()
    out = subprocess.run([kb, *args], cwd=cwd, env=env, input=stdin, capture_output=True, text=True)
    dt = time.time() - t0
    if out.returncode != 0:
        sys.exit(f"kb {' '.join(args)} failed: {out.stderr or out.stdout}")
    return out.stdout, out.stderr, dt


def link(src, models):
    dst = os.path.join(models, os.path.basename(src))
    if not os.path.exists(dst):
        try:
            os.link(src, dst)
        except OSError:
            shutil.copyfile(src, dst)


def timing(stderr):
    """KB_TIMING's lines as {phase: ms}."""
    out = {}
    for line in stderr.splitlines():
        if line.startswith("kb timing "):
            rest = line[len("kb timing "):].rsplit(None, 2)
            out[rest[0].strip()] = float(rest[1])
    return out


def measure(kb, model, reranker, size, work):
    home = os.path.join(work, "home")
    models = os.path.join(home, ".kb", "models")
    os.makedirs(models, exist_ok=True)
    link(model, models)
    if reranker:
        link(reranker, models)
    store = os.path.join(work, "store")
    os.makedirs(store)
    run(kb, ["init"], store, home)
    rnd = random.Random(size)
    ndocs = max(1, size // 10)
    batch = "".join(json.dumps({"title": f"notes {i}", "collection": "notes",
                                "content": document(rnd, i)}) + "\n" for i in range(ndocs))
    r = {"size": size, "documents": ndocs}
    out, _, r["file_keyword_s"] = run(kb, ["add", "--batch", "--embed-budget", "0", "--json"],
                                     store, home, stdin=batch)
    r["pending"] = json.loads(out)["pending"]
    out, _, r["embed_s"] = run(kb, ["embed", "--json"], store, home)
    r["embedded"] = json.loads(out)["embedded"]
    status = json.loads(run(kb, ["status", "--json"], store, home)[0])
    r["chunks"] = status.get("chunks") or status.get("store", {}).get("chunks")
    r["chunks_per_s"] = round(r["embedded"] / r["embed_s"], 1) if r["embed_s"] else None

    one = document(rnd, 10**6, sections=10)
    out, _, r["add_one_s"] = run(kb, ["add", "--title", "one more", "--collection", "notes",
                                      "--wait", "--json"], store, home, stdin=one)
    r["add_one_chunks"] = json.loads(out)["chunkCount"]
    more = "".join(json.dumps({"title": f"more {i}", "collection": "notes",
                               "content": document(rnd, 2 * 10**6 + i, sections=2)}) + "\n"
                   for i in range(20))
    _, _, r["add_batch_s"] = run(kb, ["add", "--batch", "--wait", "--json"], store, home, stdin=more)

    for mode in ("keyword", "hybrid") + (("rerank",) if reranker else ()):
        walls, phases = [], []
        qs = QUERIES if mode != "rerank" else QUERIES[:3]
        for q in qs:
            args = ["search", "--mode", "hybrid", "--rerank"] if mode == "rerank" else ["search", "--mode", mode]
            _, err, dt = run(kb, args + ["--json", "--", q], store, home, env_extra={"KB_TIMING": "1"})
            walls.append(dt * 1000)
            phases.append(timing(err))
        r[f"{mode}_ms_median"] = round(statistics.median(walls), 1)
        keys = sorted({k for p in phases for k in p})
        split = {k: round(statistics.median(p.get(k, 0.0) for p in phases), 2) for k in keys}
        split["process start and exit"] = round(r[f"{mode}_ms_median"] - split.get("total", 0.0), 1)
        r[f"{mode}_split_ms"] = split
    _, _, r["rebuild_s"] = run(kb, ["rebuild", "--json"], store, home)
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--kb", default=os.path.join(HERE, "..", "bin", "kb"))
    ap.add_argument("--model", required=True)
    ap.add_argument("--reranker")
    ap.add_argument("--sizes", default="1000,10000")
    ap.add_argument("--out", default=os.path.join(HERE, "results", "perf.json"))
    args = ap.parse_args()
    kb = os.path.abspath(args.kb)
    results = []
    for size in (int(s) for s in args.sizes.split(",")):
        work = tempfile.mkdtemp(prefix=f"kb-perf-{size}-")
        print(f"{size} chunks in {work}", file=sys.stderr, flush=True)
        r = measure(kb, os.path.abspath(args.model),
                    os.path.abspath(args.reranker) if args.reranker else None, size, work)
        results.append(r)
        print(json.dumps(r, indent=1), flush=True)
    json.dump({"kb": kb, "model": os.path.basename(args.model), "results": results},
              open(args.out, "w"), indent=1)
    print(f"wrote {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
