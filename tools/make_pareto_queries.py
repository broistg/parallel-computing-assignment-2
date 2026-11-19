#!/usr/bin/env python3
"""Subset of the frozen query set for the vector (Pareto) experiments: the first K queries of every distance bin.

    python3 tools/make_pareto_queries.py --queries config/queries.csv --per-bin 12 --out config/queries_pareto.csv

Query ids, pairs and departure times are copied unchanged (the rotating departures stay balanced when K is a multiple of 4),
so results can be compared with the scalar runs of the same queries. Fronts are expensive to compute exactly, hence the subset
(docs/PARAMETERS.md: 10 pairs per bin; 12 keeps the four departures balanced). If the oracle skips queries that exceed its label budget, drop them and finalise the golden file's fingerprints:
    python3 tools/make_pareto_queries.py --queries config/queries_pareto.csv --only-solved-by config/golden_pareto.csv --out config/queries_pareto_solved.csv
    (then replace queries_pareto.csv by it and run the oracle again with --resume: it computes nothing and writes the .meta.json)
Then:
    python3 tools/oracle.py pareto --snapshot data/snapshot_hcmc --profile config/profile.csv --queries config/queries_pareto.csv --out config/golden_pareto.csv
"""
import argparse
import csv
from collections import defaultdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--queries", required=True)
    ap.add_argument("--per-bin", type=int, default=12)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only-solved-by", help="keep only queries that have a front in this golden file (queries the oracle had to skip are dropped)")
    a = ap.parse_args()
    solved = None
    if a.only_solved_by:
        with open(a.only_solved_by) as g:
            solved = {r["query_id"] for r in csv.DictReader(g)}
    taken = defaultdict(int)
    kept = []
    with open(a.queries) as f:
        reader = csv.DictReader(f)
        header = reader.fieldnames
        for r in reader:
            if solved is not None and r["query_id"] not in solved:
                continue
            if taken[r["bin"]] < a.per_bin:
                taken[r["bin"]] += 1
                kept.append(r)
    with open(a.out, "w", newline="\n") as f:
        w = csv.DictWriter(f, fieldnames=header, lineterminator="\n")
        w.writeheader()
        w.writerows(kept)
    print("wrote %s: %s queries per bin" % (a.out, dict(sorted(taken.items()))))


if __name__ == "__main__":
    main()
