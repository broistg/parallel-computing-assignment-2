#!/usr/bin/env python3
"""Freeze the HCMC drive network into data/snapshot_hcmc/ (RUN ONCE, BY ONE PERSON).

    pip install osmnx networkx
    python tools/export_graph.py --out data/snapshot_hcmc

OSM changes daily, so a second export would give a different graph.  The person who
runs this commits the snapshot; everybody else only runs
    python tools/snapshot_io.py data/snapshot_hcmc
to verify the hash.  Never re-export after the freeze.
"""
import argparse
import datetime
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from snapshot_io import build_snapshot, verify_snapshot  # noqa: E402

PLACE = "Ho Chi Minh City, Vietnam"

CLASS_OF_HIGHWAY = {
    "motorway": 0, "motorway_link": 0, "trunk": 0, "trunk_link": 0,
    "primary": 1, "primary_link": 1,
    "secondary": 2, "secondary_link": 2, "tertiary": 2, "tertiary_link": 2,
}


def highway_class(hw):
    if isinstance(hw, (list, tuple)):          # osmnx merges tags on simplified edges
        return min(CLASS_OF_HIGHWAY.get(h, 3) for h in hw)
    return CLASS_OF_HIGHWAY.get(hw, 3)


def graph_to_snapshot(G, out_dir, meta_extra=None):
    """G: osmnx / NetworkX MultiDiGraph with node attrs x,y and edge attrs length,highway."""
    nodes = {n: (d["y"], d["x"]) for n, d in G.nodes(data=True)}
    edges = [(u, v, float(d["length"]), highway_class(d.get("highway")))
             for u, v, d in G.edges(data=True)]
    return build_snapshot(out_dir, nodes, edges, meta_extra)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="data/snapshot_hcmc")
    ap.add_argument("--place", default=PLACE)
    a = ap.parse_args()
    import osmnx as ox
    ox.settings.use_cache = True
    G = ox.graph_from_place(a.place, network_type="drive", simplify=True)
    meta = graph_to_snapshot(G, a.out, {
        "source": "OpenStreetMap via osmnx",
        "place_query": a.place,
        "osmnx_version": ox.__version__,
        "export_date_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "raw_nodes": G.number_of_nodes(),
        "raw_edges": G.number_of_edges(),
    })
    print("exported", meta["nodes"], "nodes,", meta["edges"], "edges, sha256", meta["sha256"])
    sys.exit(0 if verify_snapshot(a.out) else 1)


if __name__ == "__main__":
    main()
