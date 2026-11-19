#!/usr/bin/env python3
"""Fails (exit 1) if a bench_pareto result CSV disagrees with the exact Pareto fronts of the oracle.

    python3 tests/check_pareto.py results/x.csv [--relaxed]

Exact mode (default): every measured row must reproduce the golden front exactly (same size, every exact point found,
hypervolume ratio 1.0), with valid paths and no label-arena overflow.
--relaxed: only validity is required (paths ok, no overflow, no found point better than an exact point).
"""
import csv
import sys


def main():
    path = sys.argv[1]
    relaxed = "--relaxed" in sys.argv[2:]
    rows = bad = checked = 0
    with open(path) as f:
        for r in csv.DictReader(f):
            if r["warmup"] == "1":
                continue
            rows += 1
            problems = []
            if r["path_ok"] != "1" or r["overflow"] != "0":
                problems.append("invalid path or overflow")
            if r["golden_front_size"] != "-1":
                checked += 1
                if r["dominates_golden"] != "0":
                    problems.append("found point better than exact point")
                if not relaxed and not (r["front_size"] == r["golden_front_size"] == r["exact_points_found"] and r["hv_ratio_ppm"] == "1000000"):
                    problems.append("front differs from exact front (%s found of %s, %s points, hv %s)" % (
                        r["exact_points_found"], r["golden_front_size"], r["front_size"], r["hv_ratio_ppm"]))
            if problems:
                bad += 1
                if bad <= 5:
                    print("query %s rep %s: %s" % (r["query_id"], r["rep"], "; ".join(problems)))
    print("pareto check: %d rows (%d with golden front), %d mismatches" % (rows, checked, bad))
    sys.exit(1 if bad or not rows else 0)


if __name__ == "__main__":
    main()
