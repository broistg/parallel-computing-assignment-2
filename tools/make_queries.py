#!/usr/bin/env python3
"""Deterministic query set: stratified by straight-line distance bin and departure time.

    python tools/make_queries.py --snapshot D --per-bin 200,100,50,50,50 --seed 7 --out config/queries.csv

--per-bin is one number (same for every bin) or a comma list with one number per bin: short bins need more
pairs so that their summed sequential time reaches the 20 ms batch floor.

query_id,bin,src,dst,dep_ms.  Departure times rotate over DEPARTURES so every bin gets
off-peak, morning peak, midday and evening peak queries in equal share.
"""
import argparse
import csv
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from snapshot_io import haversine_m  # noqa: E402

DEPARTURES_MS = [3 * 3600_000, 8 * 3600_000, 12 * 3600_000 + 1800_000, 17 * 3600_000 + 1800_000]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--snapshot", required=True)
    ap.add_argument("--per-bin", default="20", help="pairs per bin: one number, or comma list (one per bin)")
    ap.add_argument("--bins-km", default="1,3,8,15,25,1000")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    edges_km = [float(x) for x in a.bins_km.split(",")]
    with open(os.path.join(a.snapshot, "nodes.csv")) as f:
        r = csv.reader(f); next(r)
        pts = [(int(c) / 1e7, int(d) / 1e7) for _, _, c, d in r]
    nbins = len(edges_km) - 1
    counts = [int(x) for x in a.per_bin.split(",")]
    if len(counts) == 1:
        counts = counts * nbins
    if len(counts) != nbins:
        sys.exit("--per-bin needs 1 or %d numbers, got %d" % (nbins, len(counts)))
    rnd = random.Random(a.seed)
    n = len(pts)
    out, qid = [], 0
    for b in range(len(edges_km) - 1):
        lo, hi = edges_km[b] * 1000, edges_km[b + 1] * 1000
        got, tries = 0, 0
        while got < counts[b] and tries < 400000:
            tries += 1
            s, t = rnd.randrange(n), rnd.randrange(n)
            if s == t:
                continue
            d = haversine_m(*pts[s], *pts[t])
            if lo <= d < hi:
                out.append((qid, b, s, t, DEPARTURES_MS[got % len(DEPARTURES_MS)]))
                qid += 1
                got += 1
        print("bin %d [%g,%g) km: %d pairs" % (b, edges_km[b], edges_km[b + 1], got))
    with open(a.out, "w", newline="\n") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["query_id", "bin", "src", "dst", "dep_ms"])
        w.writerows(out)


if __name__ == "__main__":
    main()
