#!/usr/bin/env python3
"""Generate config/profile.csv: cls,bucket,mult_permille (96 buckets of 15 min).

multiplier 1000 = free flow; larger = slower.  Values are integers, linearly
interpolated between bucket starts by BOTH the C++ code and the Python oracle.
The drop per 15 minutes is capped (scaled for other bucket widths) so FIFO (no overtaking) holds for every edge
shorter than ~10 minutes of free-flow travel; the loaders verify this.
"""
import csv
import math
import sys

import argparse

DEFAULT_BUCKETS = 96          # 15-minute buckets; try 288 for 5-minute buckets
MAX_SLOPE_PERMILLE_PER_15MIN = 250   # cap on the drop per 15 min; scaled with the bucket width
# (peak multiplier above free flow, in permille) per road class: arterials congest more
PEAK_EXTRA = {0: 700, 1: 1100, 2: 900, 3: 400}


def bump(hour, centre, width):
    return math.exp(-((hour - centre) ** 2) / (2 * width ** 2))


def profile(cls, buckets):
    max_drop = max(1, MAX_SLOPE_PERMILLE_PER_15MIN * 96 // buckets)
    vals = []
    for b in range(buckets):
        h = b * 24.0 / buckets
        x = bump(h, 8.0, 1.1) + bump(h, 17.5, 1.4) + 0.35 * bump(h, 12.5, 1.5)
        vals.append(1000 + round(PEAK_EXTRA[cls] * min(1.0, x)))
    # enforce the per-bucket drop cap (cyclic) by a forward/backward pass
    for _ in range(3):
        for i in range(2 * buckets):
            a, b = vals[i % buckets], vals[(i + 1) % buckets]
            if a - b > max_drop:
                vals[(i + 1) % buckets] = a - max_drop
    return vals


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="config/profile.csv")
    ap.add_argument("--buckets", type=int, default=DEFAULT_BUCKETS, help="buckets per day (must divide 86 400 000 ms)")
    a = ap.parse_args()
    assert 86_400_000 % a.buckets == 0
    with open(a.out, "w", newline="\n") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["cls", "bucket", "mult_permille"])
        for cls in range(4):
            for b, v in enumerate(profile(cls, a.buckets)):
                w.writerow([cls, b, v])
    print("wrote", a.out, "with", a.buckets, "buckets per day")
