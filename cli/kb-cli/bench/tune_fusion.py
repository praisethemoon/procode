#!/usr/bin/env python3
"""Chooses hybrid search's fusion weight by cross-validation on the benchmark.

    python3 tune_fusion.py --store <a bench.py --store folder> [--folds 5]

For every benchmark query it takes kb's keyword and semantic candidate lists
(top 50 each, with their scores) from the built store, then scores fusion
rules offline, so a grid of weights costs no extra searches:

  rrf        reciprocal rank fusion, k = 60 (kb's rule before score fusion)
  score(a)   a * vector' + (1 - a) * bm25', each min-max normalised over the
             query's candidates; a candidate one list did not find scores 0
             there

`a` is chosen separately for identifier-shaped queries (see identifier_shaped
in cmd_search.c, mirrored below) and for the rest. The report gives, per
fold, the weight chosen on the other folds and hit@1 on the held-out fold,
then the weight chosen on all queries: that one goes into kb.

Standard library only.
"""

import argparse
import json
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bench  # noqa: E402

DEPTH = 50
GRID = [round(0.05 * i, 2) for i in range(21)]
# The semantic first answer's lead over its second (cosine) at which it keeps
# rank 1 whatever the fused order; 1.0 is "never".
LEADS = [1.0, 0.10, 0.08, 0.06, 0.05, 0.04, 0.03, 0.02]


def identifier_shaped(q):
    """Mirrors cmd_search.c: one word that looks like a name in code — it has
    an underscore, an inner capital, a digit, `::`, `->`, a dot or `()` in it,
    or starts with a dash (a flag)."""
    q = q.strip()
    if not q or " " in q:
        return False
    return bool(re.search(r"_|[a-z][A-Z]|\d|::|->|\.|\(\)|^-", q))


def lists_for(kb, store, home, q, s):
    out = {}
    for m in ("keyword", "semantic"):
        res = json.loads(bench.kb_run(kb, ["search", "--mode", m, "--k", str(DEPTH),
                                           "--collection", s, "--json", "--", q],
                                      store, home, check=False) or "{}")
        key = "bm25" if m == "keyword" else "vector"
        out[m] = [(h["chunk"], h["scores"].get(key, 0.0)) for h in res.get("hits", [])]
    return out


def fuse_rrf(kw, sem, k=60):
    s = {}
    for i, (c, _) in enumerate(kw):
        s[c] = s.get(c, 0) + 1 / (k + i + 1)
    for i, (c, _) in enumerate(sem):
        s[c] = s.get(c, 0) + 1 / (k + i + 1)
    return sorted(s, key=lambda c: -s[c])


def norm(lst):
    if not lst:
        return {}
    hi = max(v for _, v in lst)
    lo = min(v for _, v in lst)
    span = hi - lo
    return {c: ((v - lo) / span if span > 0 else 1.0) for c, v in lst}


def fuse_score(kw, sem, a, lead=1.0):
    nk, ns = norm(kw), norm(sem)
    cands = set(nk) | set(ns)
    s = {c: a * ns.get(c, 0.0) + (1 - a) * nk.get(c, 0.0) for c in cands}
    order = sorted(cands, key=lambda c: (-s[c], c))
    if len(sem) >= 2 and sem[0][1] - sem[1][1] >= lead and sem[0][0] in order:
        order.remove(sem[0][0])
        order.insert(0, sem[0][0])
    return order


def rank_of(order, right):
    for i, c in enumerate(order[:10]):
        if c in right:
            return i + 1
    return None


def hit1(rows, a_id, a_other, lead=1.0):
    n = 0
    for r in rows:
        a = a_id if r["ident"] else a_other
        n += rank_of(fuse_score(r["kw"], r["sem"], a, lead), r["right"]) == 1
    return n / len(rows) if rows else 0.0


def best(rows):
    """The (identifier weight, other weight, lead) with the best hit@1; ties
    to the weight nearest 0.5 and the lead that fires least (the least
    committed choices)."""
    ids = [r for r in rows if r["ident"]]
    other = [r for r in rows if not r["ident"]]
    def pick(group, lead):
        if not group:
            return 0.5
        return max(GRID, key=lambda a: (hit1(group, a, a, lead), -abs(a - 0.5)))
    choices = []
    for lead in LEADS:
        a_id, a_other = pick(ids, lead), pick(other, lead)
        choices.append((hit1(rows, a_id, a_other, lead), lead, a_id, a_other))
    score, lead, a_id, a_other = max(choices, key=lambda c: (c[0], c[1]))
    return a_id, a_other, lead


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--store", required=True)
    ap.add_argument("--kb", default=os.path.join(HERE, "..", "bin", "kb"))
    ap.add_argument("--folds", type=int, default=5)
    args = ap.parse_args()
    kb = os.path.abspath(args.kb)
    store, home = os.path.join(args.store, "store"), os.path.join(args.store, "home")
    root = bench.repo_root()
    corpus = json.load(open(os.path.join(HERE, "corpus.json")))
    bench.CORPUS = corpus
    chunks = bench.chunk_map(kb, store, home, bench.SETS)
    rows = []
    for s in bench.SETS:
        texts = {p: subprocess.run(["git", "-C", root, "show", f"{corpus['commit']}:{p}"],
                                   capture_output=True, check=True).stdout for p in corpus[s]}
        for q in bench.load_queries(s):
            ranges = [(r["path"], bench.line_bytes(texts[r["path"]], *r["lines"])) for r in q["rel"]]
            right = {cid for cid, (_, path, a, b) in chunks.items()
                     if any(path == p and a < br[1] and br[0] < b for p, br in ranges)}
            l = lists_for(kb, store, home, q["q"], s)
            rows.append({"id": q["id"], "set": s, "tag": q["tag"], "ident": identifier_shaped(q["q"]),
                         "kw": l["keyword"], "sem": l["semantic"], "right": right})
        print(f"{s}: {sum(1 for r in rows if r['set'] == s)} queries", file=sys.stderr)

    rrf = sum(rank_of(fuse_rrf(r["kw"], r["sem"]), r["right"]) == 1 for r in rows) / len(rows)
    print(f"rrf hit@1 {rrf:.3f}   identifier-shaped queries: {sum(r['ident'] for r in rows)}")
    rnd = random.Random(0)
    order = list(range(len(rows)))
    rnd.shuffle(order)
    held = []
    for f in range(args.folds):
        test = [rows[i] for i in order[f::args.folds]]
        train = [rows[i] for i in order if i not in set(order[f::args.folds])]
        a_id, a_other, lead = best(train)
        h = hit1(test, a_id, a_other, lead)
        held.append(h)
        print(f"fold {f}: a(identifier)={a_id:.2f} a(other)={a_other:.2f} lead={lead:.2f}  "
              f"held-out hit@1 {h:.3f}")
    print(f"cross-validated hit@1 {sum(held) / len(held):.3f} (rrf {rrf:.3f})")
    a_id, a_other, lead = best(rows)
    print(f"chosen on all queries: a(identifier)={a_id:.2f} a(other)={a_other:.2f} lead={lead:.2f}  "
          f"hit@1 {hit1(rows, a_id, a_other, lead):.3f}")
    for s in bench.SETS:
        sub = [r for r in rows if r["set"] == s]
        r1 = sum(rank_of(fuse_rrf(r["kw"], r["sem"]), r["right"]) == 1 for r in sub) / len(sub)
        print(f"  {s}: rrf {r1:.3f} -> score {hit1(sub, a_id, a_other, lead):.3f}")


if __name__ == "__main__":
    main()
