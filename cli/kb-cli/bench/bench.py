#!/usr/bin/env python3
"""kb's retrieval benchmark: how often search puts the right place first.

    python3 bench.py --name <run> [--model <file.gguf>] [--kb <kb binary>]
                     [--set docs|code|all] [--modes keyword,semantic,hybrid]
                     [--store <dir>] [--rebuild]
    python3 bench.py --compare results/<a>.json results/<b>.json
    python3 bench.py --check [queries.jsonl ...]

THE CORPUS IS PINNED. corpus.json names a commit and the files to take from
it; they are read with `git show <commit>:<path>`, never from the working
tree, so the labels keep pointing at the same lines while the code they
describe moves on.

THE STORE IS BUILT THE WAY A USER BUILDS ONE: the files are written into a
throwaway folder and filed with `kb add --dir`, docs and code in their own
collections. HOME points into the throwaway folder, with the model linked into
its .kb/models, so nothing is read from or written to the real home directory.
Building embeds every chunk and takes minutes; --store keeps the built store
in a folder of your choosing and reuses it while the commit, the corpus, the
model file and the kb binary are the same (--rebuild forces a new one).

A HIT IS RIGHT when its chunk overlaps one of the query's labelled line ranges
in the labelled file. Labels are lines, not chunks, so a change to the chunker
cannot silently move the answer key.

Reported per query set, per tag and per mode: hit@1, hit@3 and MRR@10, each
with a bootstrap 95% interval over the queries (2000 resamples, seed 0).
--compare prints two runs side by side with a paired bootstrap interval on
each difference: an interval that includes 0 is not a difference.

Python 3.8+, standard library only.
"""

import argparse
import hashlib
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SETS = ("docs", "code")
MODES = ("keyword", "semantic", "hybrid")
# Not run unless asked for (--modes ...,rerank,rrf): hybrid then the reranker
# over the fused top 20, and hybrid fused by reciprocal rank.
EXTRA_MODES = ("rerank", "rrf")
K = 10
RESAMPLES = 2000


def repo_root():
    out = subprocess.run(["git", "-C", HERE, "rev-parse", "--show-toplevel"],
                         capture_output=True, text=True, check=True)
    return out.stdout.strip()


def on_disk(path):
    """Where a corpus file is written: a hidden segment would be skipped by
    kb add --dir, so `.claude/x` is filed as `_claude/x`."""
    return "/".join("_" + s[1:] if s.startswith(".") else s for s in path.split("/"))


def load_queries(name):
    rows = []
    with open(os.path.join(HERE, "queries", name + ".jsonl")) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            q = json.loads(line)
            for key in ("id", "q", "tag", "rel"):
                if key not in q:
                    sys.exit(f"queries/{name}.jsonl:{n}: no {key!r}")
            rows.append(q)
    return rows


# ---- building the store ---------------------------------------------------

def kb_run(kb, args, cwd, home, check=True):
    env = dict(os.environ, HOME=home)
    env.pop("KB_STORE", None)
    out = subprocess.run([kb, *args], cwd=cwd, env=env, capture_output=True, text=True)
    if check and out.returncode != 0:
        sys.exit(f"kb {' '.join(args)} failed ({out.returncode}): {out.stderr or out.stdout}")
    return out.stdout


def file_digest(path, whole=False):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        if whole:
            for block in iter(lambda: f.read(1 << 20), b""):
                h.update(block)
        else:
            h.update(f.read(1 << 20))
    h.update(str(os.path.getsize(path)).encode())
    return h.hexdigest()


def stamp_of(corpus, model, kb):
    return {
        "commit": corpus["commit"],
        "corpus": hashlib.sha256(json.dumps(corpus, sort_keys=True).encode()).hexdigest(),
        "model": os.path.basename(model) + ":" + file_digest(model) if model else None,
        "kb": file_digest(kb, whole=True),
    }


def build(root, corpus, kb, model, work, sets):
    """Writes the corpus, files it, and returns (store dir, home, texts)."""
    home = os.path.join(work, "home")
    os.makedirs(home, exist_ok=True)
    if model:
        models = os.path.join(home, ".kb", "models")
        os.makedirs(models, exist_ok=True)
        dst = os.path.join(models, os.path.basename(model))
        if not os.path.exists(dst):
            try:
                os.link(model, dst)
            except OSError:
                shutil.copyfile(model, dst)
    store = os.path.join(work, "store")
    os.makedirs(store, exist_ok=True)
    kb_run(kb, ["init"], store, home)
    for s in sets:
        base = os.path.join(work, "corpus", s)
        for path in corpus[s]:
            blob = subprocess.run(["git", "-C", root, "show", f"{corpus['commit']}:{path}"],
                                  capture_output=True, check=True).stdout
            dst = os.path.join(base, on_disk(path))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(blob)
        t0 = time.time()
        print(f"filing {len(corpus[s])} {s} files", file=sys.stderr, flush=True)
        # --wait: every chunk embedded before the queries run, however long
        # it takes (an add otherwise stops after its time budget, §2).
        out = json.loads(kb_run(kb, ["add", "--dir", base, "--collection", s, "--wait", "--json"],
                                store, home))
        if out["files"] != len(corpus[s]):
            sys.exit(f"{s}: kb filed {out['files']} of {len(corpus[s])} files; skipped {out['skipped']}")
        print(f"  {out['files']} files, {out['embedded']} chunks embedded, "
              f"{time.time() - t0:.0f}s", file=sys.stderr, flush=True)
    return store, home


def chunk_map(kb, store, home, sets):
    """chunk id -> (set, repo path, start byte, end byte)."""
    back = {}
    for s in sets:
        for path in CORPUS[s]:
            back[(s, on_disk(path))] = path
    out = {}
    docs = json.loads(kb_run(kb, ["ls", "--limit", "100000", "--json"], store, home))["documents"]
    for d in docs:
        key = (d["collection"], d["path"])
        if key not in back:
            continue
        got = json.loads(kb_run(kb, ["get", d["id"], "--include", "chunks", "--json"], store, home))
        for c in got["chunks"]:
            out[c["id"]] = (d["collection"], back[key], c["span"]["start"], c["span"]["end"])
    return out


# ---- scoring ----------------------------------------------------------------

def line_bytes(text, first, last):
    """Byte range of lines first..last (1-based, inclusive) of text (bytes)."""
    starts = [0]
    for i, b in enumerate(text):
        if b == 0x0A:
            starts.append(i + 1)
    if first < 1 or last < first or first > len(starts):
        return None
    end = starts[last] if last < len(starts) else len(text)
    return starts[first - 1], end


def mode_args(m):
    """A mode's search flags. `rerank` may carry a depth and a token budget:
    rerank-d10-t256 is --rerank --rerank-depth 10 --rerank-tokens 256."""
    if m == "rrf":
        return ["--mode", "hybrid", "--fusion", "rrf"]
    if m.startswith("rerank"):
        args = ["--mode", "hybrid", "--rerank"]
        for part in m.split("-")[1:]:
            if part.startswith("d"):
                args += ["--rerank-depth", part[1:]]
            elif part.startswith("t"):
                args += ["--rerank-tokens", part[1:]]
        return args
    return ["--mode", m]


def run_queries(kb, store, home, queries, s, modes, texts, chunks):
    per = []
    for q in queries:
        ranges = []
        for r in q["rel"]:
            if r["path"] not in texts:
                sys.exit(f"{q['id']}: {r['path']} is not in the {s} corpus")
            br = line_bytes(texts[r["path"]], r["lines"][0], r["lines"][1])
            if br is None:
                sys.exit(f"{q['id']}: lines {r['lines']} are outside {r['path']}")
            ranges.append((r["path"], br))
        row = {"id": q["id"], "tag": q["tag"], "q": q["q"], "ranks": {}, "top": {}}
        for m in modes:
            # The query goes after `--`: an identifier query can itself start
            # with a dash (`--no-forget`), and kb would take it for an option.
            args = mode_args(m)
            t0 = time.time()
            out = json.loads(kb_run(kb, ["search", *args, "--k", str(K),
                                         "--collection", s, "--json", "--", q["q"]],
                                    store, home, check=False) or "{}")
            row.setdefault("ms", {})[m] = round((time.time() - t0) * 1000, 1)
            if not out.get("ok"):
                sys.exit(f"{q['id']} {m}: {out}")
            rank = None
            for i, h in enumerate(out["hits"]):
                c = chunks.get(h["chunk"])
                if c and any(c[1] == p and c[2] < b[1] and b[0] < c[3] for p, b in ranges):
                    rank = i + 1
                    break
            row["ranks"][m] = rank
            h0 = out["hits"][0] if out["hits"] else None
            row["top"][m] = f"{h0['title']} » {(h0.get('heading') or '')[:40]}" if h0 else None
        per.append(row)
    return per


def metrics(ranks):
    n = len(ranks)
    if n == 0:
        return (0.0, 0.0, 0.0)
    h1 = sum(1 for r in ranks if r == 1) / n
    h3 = sum(1 for r in ranks if r and r <= 3) / n
    mrr = sum(1 / r for r in ranks if r) / n
    return (h1, h3, mrr)


def interval(ranks, rng):
    n = len(ranks)
    samples = [metrics([ranks[rng.randrange(n)] for _ in range(n)]) for _ in range(RESAMPLES)]
    out = []
    for i in range(3):
        v = sorted(s[i] for s in samples)
        out.append((v[int(0.025 * RESAMPLES)], v[int(0.975 * RESAMPLES) - 1]))
    return out


def summarise(per, modes):
    rng = random.Random(0)
    summary = {}
    for s in per:
        groups = {"all": per[s]}
        for row in per[s]:
            groups.setdefault(row["tag"], []).append(row)
        for tag, rows in groups.items():
            for m in modes:
                ranks = [r["ranks"][m] for r in rows]
                point = metrics(ranks)
                ci = interval(ranks, rng)
                ms = sorted(r["ms"][m] for r in rows if m in r.get("ms", {}))
                summary[f"{s}/{tag}/{m}"] = {
                    "n": len(ranks),
                    "p50_ms": ms[len(ms) // 2] if ms else None,
                    "p95_ms": ms[min(len(ms) - 1, int(0.95 * len(ms)))] if ms else None,
                    "hit1": point[0], "hit3": point[1], "mrr": point[2],
                    "hit1_ci": ci[0], "hit3_ci": ci[1], "mrr_ci": ci[2],
                    "missed": sum(1 for r in ranks if r is None),
                }
    return summary


def show(summary):
    print(f"{'set/tag':22} {'mode':9} {'n':>4}  {'hit@1':>17}  {'hit@3':>17}  {'MRR@10':>17}  missed  p50/p95 ms")
    for key, v in summary.items():
        s, tag, m = key.split("/")
        cell = lambda x, ci: f"{x:.2f} [{ci[0]:.2f}–{ci[1]:.2f}]"
        print(f"{s + '/' + tag:22} {m:9} {v['n']:>4}  {cell(v['hit1'], v['hit1_ci']):>17}  "
              f"{cell(v['hit3'], v['hit3_ci']):>17}  {cell(v['mrr'], v['mrr_ci']):>17}  {v['missed']:>6}  "
              f"{v.get('p50_ms') or 0:.0f}/{v.get('p95_ms') or 0:.0f}")


def compare(a_path, b_path):
    a, b = json.load(open(a_path)), json.load(open(b_path))
    rng = random.Random(0)
    print(f"{a['name']} → {b['name']}   (paired bootstrap 95% interval on the difference)")
    print(f"{'set/tag':22} {'mode':9} {'n':>4}  {'hit@1':>26}  {'hit@3':>26}  {'MRR@10':>26}")
    for s in SETS:
        ra = {r["id"]: r for r in a["queries"].get(s, [])}
        rb = {r["id"]: r for r in b["queries"].get(s, [])}
        ids = sorted(set(ra) & set(rb))
        if not ids:
            continue
        tags = ["all"] + sorted({ra[i]["tag"] for i in ids})
        for tag in tags:
            sel = [i for i in ids if tag == "all" or ra[i]["tag"] == tag]
            both = set(ra[sel[0]]["ranks"]) & set(rb[sel[0]]["ranks"])
            order = [m for m in MODES + EXTRA_MODES if m in both] + sorted(
                m for m in both if m not in MODES + EXTRA_MODES)
            for m in order:
                pa = [ra[i]["ranks"][m] for i in sel]
                pb = [rb[i]["ranks"][m] for i in sel]
                ma, mb = metrics(pa), metrics(pb)
                diffs = []
                for _ in range(RESAMPLES):
                    pick = [rng.randrange(len(sel)) for _ in sel]
                    x = metrics([pa[j] for j in pick])
                    y = metrics([pb[j] for j in pick])
                    diffs.append([y[k] - x[k] for k in range(3)])
                cells = []
                for k in range(3):
                    v = sorted(d[k] for d in diffs)
                    lo, hi = v[int(0.025 * RESAMPLES)], v[int(0.975 * RESAMPLES) - 1]
                    mark = "*" if lo > 0 or hi < 0 else " "
                    cells.append(f"{ma[k]:.2f}→{mb[k]:.2f} [{lo:+.2f},{hi:+.2f}]{mark}")
                print(f"{s + '/' + tag:22} {m:9} {len(sel):>4}  " + "  ".join(f"{c:>26}" for c in cells))
    print("* the interval excludes 0")


def check(paths):
    """Validates query files against the pinned corpus without a store:
    unique ids, known tags, one to three labels, each a real line range of a
    corpus file in the query's own set. Exits non-zero on the first file with
    problems, after listing all of them."""
    root = repo_root()
    corpus = json.load(open(os.path.join(HERE, "corpus.json")))
    lines = {}
    for s in SETS:
        for path in corpus[s]:
            blob = subprocess.run(["git", "-C", root, "show", f"{corpus['commit']}:{path}"],
                                  capture_output=True, check=True).stdout
            lines[(s, path)] = blob.count(b"\n") + (0 if blob.endswith(b"\n") else 1)
    if not paths:
        paths = [os.path.join(HERE, "queries", s + ".jsonl") for s in SETS]
    bad = 0
    for p in paths:
        s = "docs" if os.path.basename(p).startswith("doc") else "code"
        seen, tags = set(), {}
        with open(p) as f:
            for n, raw in enumerate(f, 1):
                if not raw.strip():
                    continue
                where = f"{p}:{n}"
                try:
                    q = json.loads(raw)
                except ValueError as e:
                    print(f"{where}: not JSON ({e})"); bad += 1; continue
                if q.get("id") in seen:
                    print(f"{where}: duplicate id {q.get('id')}"); bad += 1
                seen.add(q.get("id"))
                if q.get("tag") not in ("paraphrase", "identifier", "purpose"):
                    print(f"{where}: tag {q.get('tag')!r}"); bad += 1
                tags[q.get("tag")] = tags.get(q.get("tag"), 0) + 1
                if not isinstance(q.get("q"), str) or not q["q"].strip():
                    print(f"{where}: empty query"); bad += 1
                rel = q.get("rel")
                if not isinstance(rel, list) or not 1 <= len(rel) <= 3:
                    print(f"{where}: rel must hold one to three places"); bad += 1; continue
                for r in rel:
                    key = (s, r.get("path"))
                    a, b = (r.get("lines") or [0, 0])[:2]
                    if key not in lines:
                        print(f"{where}: {r.get('path')} is not in the {s} corpus"); bad += 1
                    elif not (1 <= a <= b <= lines[key]):
                        print(f"{where}: lines {a}-{b} outside {r['path']} (1-{lines[key]})"); bad += 1
        print(f"{p}: {len(seen)} queries {dict(sorted(tags.items()))}")
    if bad:
        sys.exit(f"{bad} problems")


def main():
    global CORPUS
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--name")
    ap.add_argument("--kb", default=os.path.join(HERE, "..", "bin", "kb"))
    ap.add_argument("--model", help="an embedding model file; without one only keyword runs")
    ap.add_argument("--reranker", help="the reranker model file, for --modes ...,rerank")
    ap.add_argument("--set", default="all", choices=("docs", "code", "all"))
    ap.add_argument("--modes", default=",".join(MODES))
    ap.add_argument("--store", help="a folder to keep the built store in and reuse")
    ap.add_argument("--rebuild", action="store_true")
    ap.add_argument("--trust-store", action="store_true",
                    help="reuse --store even if kb changed since it was built; only for "
                         "a change that touches search and not indexing (a reranker, fusion)")
    ap.add_argument("--out", help="default: results/<name>.json")
    ap.add_argument("--compare", nargs=2, metavar=("A", "B"))
    ap.add_argument("--check", nargs="*", metavar="QUERIES")
    args = ap.parse_args()
    if args.check is not None:
        check(args.check)
        return
    if args.compare:
        compare(*args.compare)
        return
    if not args.name:
        ap.error("--name is required")
    kb = os.path.abspath(args.kb)
    model = os.path.abspath(args.model) if args.model else None
    modes = [m for m in args.modes.split(",") if m]
    if not model:
        modes = [m for m in modes if m == "keyword"]
    sets = list(SETS) if args.set == "all" else [args.set]
    root = repo_root()
    CORPUS = json.load(open(os.path.join(HERE, "corpus.json")))

    stamp = stamp_of(CORPUS, model, kb)
    if args.store:
        work = os.path.abspath(args.store)
        stamp_path = os.path.join(work, "stamp.json")
        old = json.load(open(stamp_path)) if os.path.exists(stamp_path) else None
        if old and args.trust_store:
            old = dict(old, kb=stamp["kb"])
        fresh = not args.rebuild and old == stamp
        if not fresh and os.path.exists(os.path.join(work, "store")):
            sys.exit(f"{work} holds a store built from something else; pick an empty "
                     f"folder (the benchmark never deletes one)")
        os.makedirs(work, exist_ok=True)
    else:
        work = tempfile.mkdtemp(prefix="kb-bench-")
        fresh = False
    if fresh:
        store, home = os.path.join(work, "store"), os.path.join(work, "home")
    else:
        store, home = build(root, CORPUS, kb, model, work, SETS)
        if args.store:
            json.dump(stamp, open(os.path.join(work, "stamp.json"), "w"))

    if any(m.startswith("rerank") for m in modes):
        if not args.reranker:
            sys.exit("--modes rerank needs --reranker <file.gguf>")
        models = os.path.join(home, ".kb", "models")
        os.makedirs(models, exist_ok=True)
        dst = os.path.join(models, os.path.basename(args.reranker))
        if not os.path.exists(dst):
            try:
                os.link(os.path.abspath(args.reranker), dst)
            except OSError:
                shutil.copyfile(args.reranker, dst)

    texts = {}
    for s in SETS:
        for path in CORPUS[s]:
            texts[(s, path)] = subprocess.run(
                ["git", "-C", root, "show", f"{CORPUS['commit']}:{path}"],
                capture_output=True, check=True).stdout
    chunks = chunk_map(kb, store, home, SETS)
    per = {}
    for s in sets:
        qs = load_queries(s)
        print(f"{len(qs)} {s} queries × {len(modes)} modes", file=sys.stderr, flush=True)
        per[s] = run_queries(kb, store, home, qs, s, modes,
                             {p: t for (ss, p), t in texts.items() if ss == s}, chunks)
    summary = summarise(per, modes)
    show(summary)
    model_json = kb_run(kb, ["status", "--json"], store, home, check=False)
    try:
        model_rec = json.loads(model_json).get("model")
    except ValueError:
        model_rec = None
    out = args.out or os.path.join(HERE, "results", args.name + ".json")
    json.dump({"name": args.name, "commit": CORPUS["commit"], "model": model_rec,
               "stamp": stamp, "modes": modes, "summary": summary, "queries": per},
              open(out, "w"), indent=1)
    print(f"wrote {out}" + ("" if args.store else f" (store left in {work})"), file=sys.stderr)


CORPUS = None

if __name__ == "__main__":
    main()
