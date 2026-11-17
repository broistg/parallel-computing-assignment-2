#!/usr/bin/env python3
"""Snapshot builder / verifier (the ONLY code that writes a graph snapshot).

A snapshot is a directory with three files:
  nodes.csv  idx,osmid,lat_e7,lon_e7          (idx = 0..N-1, sorted by osmid)
  edges.csv  u,v,length_dm,speed_kmh,cls      (sorted by (u,v); integers only)
  meta.json  counts, source, versions, sha256 over nodes.csv + edges.csv bytes

Everything is integer so the hash is identical on every platform.
Rules enforced here (the C++ loader re-checks the cheap ones):
  * self loops dropped, parallel edges collapsed to the shortest one
  * only the largest strongly connected component is kept
  * length_dm >= ceil(10 * haversine(u, v)) + 1  -> haversine/vmax heuristics stay
    admissible and consistent despite rounding
"""
import hashlib
import json
import math
import os
import sys

import networkx as nx

R_EARTH_M = 6371008.8  # must equal kEarthRadiusM in include/astar/graph.h
CLASS_SPEED_KMH = {0: 60, 1: 50, 2: 40, 3: 30}


def haversine_m(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = p2 - p1
    dl = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * R_EARTH_M * math.asin(min(1.0, math.sqrt(a)))


def build_snapshot(out_dir, nodes, edges, meta_extra=None):
    """nodes: dict osmid -> (lat_deg, lon_deg)
       edges: iterable of (osmid_u, osmid_v, length_m, cls)  cls in 0..3
       Returns the meta dict."""
    g = nx.DiGraph()
    for u, v, length_m, cls in edges:
        if u == v or u not in nodes or v not in nodes:
            continue
        if g.has_edge(u, v):
            old = g[u][v]
            if (length_m, cls) >= (old["length_m"], old["cls"]):   # deterministic tie-break
                continue
        g.add_edge(u, v, length_m=float(length_m), cls=int(cls))
    keep = max(nx.strongly_connected_components(g), key=len)
    g = g.subgraph(keep).copy()

    ids = sorted(g.nodes())
    index = {osmid: i for i, osmid in enumerate(ids)}
    node_rows = []
    for osmid in ids:
        lat, lon = nodes[osmid]
        node_rows.append((index[osmid], osmid, round(lat * 1e7), round(lon * 1e7)))

    edge_rows = []
    for u, v, d in g.edges(data=True):
        lat1, lon1 = nodes[u]
        lat2, lon2 = nodes[v]
        hav_dm = math.ceil(haversine_m(lat1, lon1, lat2, lon2) * 10.0) + 1
        length_dm = max(math.ceil(d["length_m"] * 10.0), hav_dm)
        edge_rows.append((index[u], index[v], length_dm, CLASS_SPEED_KMH[d["cls"]], d["cls"]))
    edge_rows.sort()

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "nodes.csv"), "w", newline="\n") as f:
        f.write("idx,osmid,lat_e7,lon_e7\n")
        for r in node_rows:
            f.write("%d,%d,%d,%d\n" % r)
    with open(os.path.join(out_dir, "edges.csv"), "w", newline="\n") as f:
        f.write("u,v,length_dm,speed_kmh,cls\n")
        for r in edge_rows:
            f.write("%d,%d,%d,%d,%d\n" % r)

    meta = {
        "format": 1,
        "nodes": len(node_rows),
        "edges": len(edge_rows),
        "sha256": hash_snapshot(out_dir),
        "class_speed_kmh": CLASS_SPEED_KMH,
        "earth_radius_m": R_EARTH_M,
    }
    meta.update(meta_extra or {})
    with open(os.path.join(out_dir, "meta.json"), "w", newline="\n") as f:
        json.dump(meta, f, indent=2, sort_keys=True)
        f.write("\n")
    return meta


def hash_snapshot(d):
    h = hashlib.sha256()
    for name in ("nodes.csv", "edges.csv"):
        with open(os.path.join(d, name), "rb") as f:
            data = f.read().replace(b"\r\n", b"\n")  # git autocrlf on Windows
        h.update(name.encode() + b"\0" + data)
    return h.hexdigest()


def verify_snapshot(d):
    with open(os.path.join(d, "meta.json")) as f:
        meta = json.load(f)
    actual = hash_snapshot(d)
    ok = actual == meta["sha256"]
    print("snapshot %s: %s (nodes=%d edges=%d sha256=%s)" % (
        d, "OK" if ok else "HASH MISMATCH", meta["nodes"], meta["edges"], actual[:16]))
    return ok


if __name__ == "__main__":
    sys.exit(0 if verify_snapshot(sys.argv[1]) else 1)
