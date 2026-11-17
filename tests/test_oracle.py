#!/usr/bin/env python3
"""Layer 1 of the trust ladder: the exact oracle is checked against exhaustive path enumeration
on tiny graphs, so everything downstream (golden file) rests on something obviously correct.
Run: python3 tests/test_oracle.py"""
import os
import random
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
from oracle import Net, dijkstra, martins  # noqa: E402
from synth_graph import make  # noqa: E402
from snapshot_io import build_snapshot  # noqa: E402

PROFILE = os.path.join(HERE, "..", "config", "profile.csv")


def all_simple_paths_costs(net, src, dst, t0):
    """Exhaustive DFS over simple paths -> list of (time_ms, dist_dm)."""
    out = []
    seen = {src}

    def dfs(u, t, d):
        if u == dst:
            out.append((t, d))
            return
        for v, ldm, free_ms, cls in net.adj[u]:
            if v in seen:
                continue
            seen.add(v)
            dfs(v, t + net.tcost(free_ms, cls, t0 + t), d + ldm)
            seen.discard(v)

    dfs(src, 0, 0)
    return out


def pareto(points):
    pts = sorted(set(points))
    return [p for p in pts if not any(q != p and q[0] <= p[0] and q[1] <= p[1] for q in pts)]


def main():
    fails = 0
    checked = 0
    for seed in range(1, 7):
        nodes, edges = make(4, 4, seed)
        with tempfile.TemporaryDirectory() as d:
            build_snapshot(d, nodes, edges)
            net = Net(d, PROFILE)
            rnd = random.Random(seed)
            for _ in range(6):
                s, t = rnd.randrange(net.n), rnd.randrange(net.n)
                t0 = rnd.choice([3, 8, 12, 17]) * 3600_000 + rnd.randrange(0, 900_000)
                brute = all_simple_paths_costs(net, s, t, t0)
                front = pareto(brute)
                got = martins(net, s, t, t0)
                checked += 1
                if got != front:
                    fails += 1
                    print("PARETO MISMATCH seed", seed, s, t, got, front)
                if dijkstra(net, s, t, t0, "time") != min(b[0] for b in brute):
                    fails += 1
                    print("TIME MISMATCH", seed, s, t)
                if dijkstra(net, s, t, t0, "dist") != min(b[1] for b in brute):
                    fails += 1
                    print("DIST MISMATCH", seed, s, t)
    print("oracle vs brute force: %d queries, %d mismatches" % (checked, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
