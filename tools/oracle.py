#!/usr/bin/env python3
"""Exact oracle + golden-file writer.  Deliberately simple and SLOW, and written
independently of the C++ code (shares only the data files and the integer cost formula
from docs/SPEC_1_objectives.md).

  time objective : time-dependent Dijkstra (exact because of FIFO)
  dist objective : plain Dijkstra on length_dm
  pareto         : Martins label-setting on (time_ms, dist_dm), small graphs only

CLI:
  oracle.py golden  --snapshot D --profile P --queries Q --out golden.csv [--obj time|dist]
  oracle.py pareto  --snapshot D --profile P --queries Q --out front.csv  [--limit N]
"""
import argparse
import csv
import heapq
import math
import os
import sys

DAY_MS = 86_400_000
MASK64 = 0xFFFFFFFFFFFFFFFF


def fnv1a(data, h=1469598103934665603):
    """Same function as fnv1a() in include/astar/graph.h (ignores CR so CRLF checkouts match)."""
    for c in data:
        if c == 13:
            continue
        h = ((h ^ c) * 1099511628211) & MASK64
    return h


def file_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def input_hashes(snapshot, profile, queries):
    """Fingerprints that tie a golden file to the exact data it was computed from."""
    snap = fnv1a(file_bytes(os.path.join(snapshot, "edges.csv")),
                 fnv1a(file_bytes(os.path.join(snapshot, "nodes.csv"))))
    return {"snapshot_fnv": "%016x" % snap,
            "profile_fnv": "%016x" % fnv1a(file_bytes(profile)),
            "queries_fnv": "%016x" % fnv1a(file_bytes(queries))}


class Net:
    def __init__(self, snap, profile):
        with open(os.path.join(snap, "nodes.csv")) as f:
            r = csv.reader(f); next(r)
            rows = [(int(a), int(c), int(d)) for a, _, c, d in r]
        self.n = len(rows)
        self.lat = [c / 1e7 for _, c, _ in rows]
        self.lon = [d / 1e7 for _, _, d in rows]
        self.adj = [[] for _ in range(self.n)]       # (v, len_dm, free_ms, cls)
        with open(os.path.join(snap, "edges.csv")) as f:
            r = csv.reader(f); next(r)
            for u, v, ldm, kmh, cls in r:
                u, v, ldm, kmh, cls = int(u), int(v), int(ldm), int(kmh), int(cls)
                free_ms = -(-ldm * 360 // kmh)        # ceil(len_dm*360/kmh)
                self.adj[u].append((v, ldm, free_ms, cls))
        with open(profile) as f:
            r = csv.reader(f); next(r)
            rows = [(int(c), int(b), int(m)) for c, b, m in r]
        self.nb = len(rows) // 4                  # number of time buckets comes from the file
        assert len(rows) == 4 * self.nb and DAY_MS % self.nb == 0
        self.bucket_ms = DAY_MS // self.nb
        self.mult = [[0] * self.nb for _ in range(4)]
        for cls, b, m in rows:
            self.mult[cls][b] = m

    def tcost(self, free_ms, cls, t):
        tm = t % DAY_MS
        k = tm // self.bucket_ms
        rem = tm - k * self.bucket_ms
        m0 = self.mult[cls][k]
        m1 = self.mult[cls][(k + 1) % self.nb]
        mnum = m0 * self.bucket_ms + (m1 - m0) * rem
        return -(-(free_ms * mnum) // (self.bucket_ms * 1000))


def dijkstra(net, src, dst, t0, obj):
    INF = 1 << 62
    g = [INF] * net.n
    g[src] = 0
    pq = [(0, src)]
    while pq:
        d, u = heapq.heappop(pq)
        if d > g[u]:
            continue
        if u == dst:
            return d
        for v, ldm, free_ms, cls in net.adj[u]:
            c = ldm if obj == "dist" else net.tcost(free_ms, cls, t0 + d)
            nd = d + c
            if nd < g[v]:
                g[v] = nd
                heapq.heappush(pq, (nd, v))
    return None


def martins(net, src, dst, t0, max_labels=5_000_000):
    """Pareto front of (time_ms, dist_dm) from src to dst departing at t0."""
    fronts = [[] for _ in range(net.n)]       # settled non-dominated (t, d) per node
    pq = [(0, 0, src)]
    fronts[src].append((0, 0))
    pushed = 1
    while pq:
        t, d, u = heapq.heappop(pq)
        if any(a <= t and b <= d and (a, b) != (t, d) for a, b in fronts[u]):
            continue                            # dominated after being queued
        for v, ldm, free_ms, cls in net.adj[u]:
            nt = t + net.tcost(free_ms, cls, t0 + t)
            nd = d + ldm
            if any(a <= nt and b <= nd for a, b in fronts[v]):
                continue
            fronts[v] = [(a, b) for a, b in fronts[v] if not (nt <= a and nd <= b)]
            fronts[v].append((nt, nd))
            heapq.heappush(pq, (nt, nd, v))
            pushed += 1
            if pushed > max_labels:
                raise RuntimeError("label budget exceeded; use a smaller graph")
    return sorted(set(fronts[dst]))


def read_queries(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["golden", "pareto"])
    ap.add_argument("--snapshot", required=True)
    ap.add_argument("--profile", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--obj", default="time", choices=["time", "dist"])
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()
    net = Net(a.snapshot, a.profile)
    qs = read_queries(a.queries)
    if a.limit:
        qs = qs[: a.limit]
    with open(a.out, "w", newline="\n") as f:
        w = csv.writer(f, lineterminator="\n")
        if a.mode == "golden":
            w.writerow(["query_id", "obj", "cost"])
            for q in qs:
                c = dijkstra(net, int(q["src"]), int(q["dst"]), int(q["dep_ms"]), a.obj)
                w.writerow([q["query_id"], a.obj, c])
        else:
            w.writerow(["query_id", "idx", "time_ms", "dist_dm"])
            for q in qs:
                fr = martins(net, int(q["src"]), int(q["dst"]), int(q["dep_ms"]))
                for i, (t, d) in enumerate(fr):
                    w.writerow([q["query_id"], i, t, d])
    import json
    meta = input_hashes(a.snapshot, a.profile, a.queries)
    meta.update({"mode": a.mode, "objective": a.obj if a.mode == "golden" else "pareto", "num_queries": len(qs)})
    with open(a.out + ".meta.json", "w", newline="\n") as f:
        json.dump(meta, f, indent=2, sort_keys=True)
        f.write("\n")
    print("wrote", a.out, "and", a.out + ".meta.json", "for", len(qs), "queries")


if __name__ == "__main__":
    main()
