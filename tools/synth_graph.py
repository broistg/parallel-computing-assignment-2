#!/usr/bin/env python3
"""DEVELOPMENT / TEST ONLY: deterministic synthetic road-like network in the exact
snapshot format (used for unit tests and for the pilot when OSM is not reachable).
Results on it must never appear in the report.

    python tools/synth_graph.py --rows 60 --cols 60 --seed 1 --out data/snapshot_synth_small
"""
import argparse
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from snapshot_io import build_snapshot  # noqa: E402


def make(rows, cols, seed):
    rnd = random.Random(seed)
    lat0, lon0 = 10.7700, 106.6900
    step_lat, step_lon = 0.0009, 0.00092          # ~100 m
    nodes, edges = {}, []
    nid = lambda r, c: 1000 + r * cols + c
    for r in range(rows):
        for c in range(cols):
            nodes[nid(r, c)] = (lat0 + r * step_lat + rnd.uniform(-2e-4, 2e-4),
                                lon0 + c * step_lon + rnd.uniform(-2e-4, 2e-4))

    def cls_of(r, c, horizontal):
        line = r if horizontal else c
        if line % 25 == 0:
            return 0
        if line % 10 == 0:
            return 1
        if line % 5 == 0:
            return 2
        return 3

    def add(a, b, cls):
        (la, oa), (lb, ob) = nodes[a], nodes[b]
        import math
        dy = (lb - la) * 111195.0
        dx = (ob - oa) * 111195.0 * math.cos(math.radians(la))
        length = math.hypot(dx, dy) * rnd.uniform(1.0, 1.25)
        edges.append((a, b, length, cls))
        if cls == 3 and rnd.random() < 0.15:     # one-way residential street
            return
        edges.append((b, a, length * rnd.uniform(1.0, 1.02), cls))

    for r in range(rows):
        for c in range(cols):
            if c + 1 < cols and (rnd.random() > 0.06 or cls_of(r, c, True) < 3):
                add(nid(r, c), nid(r, c + 1), cls_of(r, c, True))
            if r + 1 < rows and (rnd.random() > 0.06 or cls_of(r, c, False) < 3):
                add(nid(r, c), nid(r + 1, c), cls_of(r, c, False))
    return nodes, edges


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=60)
    ap.add_argument("--cols", type=int, default=60)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    n, e = make(a.rows, a.cols, a.seed)
    m = build_snapshot(a.out, n, e, {"source": "SYNTHETIC (dev only)", "seed": a.seed})
    print("synthetic snapshot:", m["nodes"], "nodes", m["edges"], "edges", m["sha256"][:16])
