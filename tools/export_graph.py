#!/usr/bin/env python3
"""Freeze the HCMC drive network into data/snapshot_hcmc/ (RUN ONCE, BY ONE PERSON).

Three ways to get the OSM data, from most to least convenient.  Whatever you use, the SAME
drivable-road rule and the same snapshot writer are applied, so the result format is identical.

  A) bounding box via Overpass (default; no Nominatim / geocoding request):
       python tools/export_graph.py --out data/snapshot_hcmc
       python tools/export_graph.py --bbox 106.55,10.65,106.90,10.95 --out data/snapshot_hcmc
     If overpass-api.de is unreachable, try mirrors (tried in order, comma separated):
       --overpass-url https://overpass.private.coffee/api,https://maps.mail.ru/osm/tools/overpass/api

  B) a local OSM XML file (works fully offline, and the input file can be hashed and archived):
       python tools/export_graph.py --osm-file hcmc.osm --out data/snapshot_hcmc
     (how to make hcmc.osm from a Geofabrik extract: docs/EXPORT_HELP.md)

  C) by place name (legacy; needs Nominatim; the boundary of "Ho Chi Minh City" may have changed
     after the 2025 province merger, so prefer A or B):
       python tools/export_graph.py --place "Ho Chi Minh City, Vietnam"

OSM changes daily, so a second export gives a different graph.  The person who runs this commits
the snapshot; everybody else only runs  python tools/snapshot_io.py data/snapshot_hcmc  to verify
the hash.  Never re-export after the freeze.  Check connectivity first:  python tools/check_osm_access.py
"""
import argparse
import datetime
import hashlib
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from snapshot_io import build_snapshot, verify_snapshot  # noqa: E402

DEFAULT_BBOX = "106.55,10.65,106.90,10.95"   # west,south,east,north: urban HCMC; contains the notebook's origin and destination
PLACE = "Ho Chi Minh City, Vietnam"

CLASS_OF_HIGHWAY = {
    "motorway": 0, "motorway_link": 0, "trunk": 0, "trunk_link": 0,
    "primary": 1, "primary_link": 1,
    "secondary": 2, "secondary_link": 2, "tertiary": 2, "tertiary_link": 2,
}

# Same exclusions as osmnx's network_type="drive" Overpass filter, applied in Python so that every
# data source (Overpass, local file) yields the same definition of "drivable".
EXCLUDED_HIGHWAY = {"abandoned", "bridleway", "bus_guideway", "construction", "corridor", "cycleway", "elevator",
                    "escalator", "footway", "no", "path", "pedestrian", "planned", "platform", "proposed",
                    "raceway", "razed", "steps", "track"}
EXCLUDED_SERVICE = {"alley", "driveway", "emergency_access", "parking", "parking_aisle", "private"}


def as_list(value):
    return list(value) if isinstance(value, (list, tuple)) else [value]


def is_drivable(data):
    highways = as_list(data.get("highway"))
    if not highways or all(h in EXCLUDED_HIGHWAY or h is None for h in highways):
        return False
    if data.get("area") == "yes":
        return False
    if "no" in as_list(data.get("motor_vehicle")) or "no" in as_list(data.get("motorcar")):
        return False
    services = as_list(data.get("service"))
    if services != [None] and any(s in EXCLUDED_SERVICE for s in services):
        return False
    return True


def highway_class(hw):
    if isinstance(hw, (list, tuple)):          # osmnx merges tags on simplified edges
        return min(CLASS_OF_HIGHWAY.get(h, 3) for h in hw)
    return CLASS_OF_HIGHWAY.get(hw, 3)


def graph_to_snapshot(G, out_dir, meta_extra=None):
    """G: osmnx / NetworkX MultiDiGraph with node attrs x,y and edge attrs length,highway."""
    nodes = {n: (d["y"], d["x"]) for n, d in G.nodes(data=True)}
    edges = [(u, v, float(d["length"]), highway_class(d.get("highway")))
             for u, v, d in G.edges(data=True) if is_drivable(d)]
    return build_snapshot(out_dir, nodes, edges, meta_extra)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_graph(args, ox):
    """Returns (graph, provenance dict)."""
    # tags we need to decide "drivable" when the data comes from a file (the Overpass query filters on the server)
    for tag in ("motor_vehicle", "motorcar", "area", "service", "highway", "oneway"):
        if tag not in ox.settings.useful_tags_way:
            ox.settings.useful_tags_way = list(ox.settings.useful_tags_way) + [tag]
    ox.settings.use_cache = True
    ox.settings.requests_timeout = args.timeout
    if args.osm_file:
        print("reading", args.osm_file)
        return (ox.graph_from_xml(args.osm_file, bidirectional=False, simplify=True),
                {"method": "osm_file", "osm_file_name": os.path.basename(args.osm_file),
                 "osm_file_sha256": sha256_file(args.osm_file)})
    urls = [u.strip() for u in args.overpass_url.split(",") if u.strip()]
    last_error = None
    for url in urls:
        ox.settings.overpass_url = url
        for attempt in range(1, args.retries + 1):
            try:
                print("downloading via %s (attempt %d/%d)" % (url, attempt, args.retries))
                if args.place:
                    G = ox.graph_from_place(args.place, network_type="drive", simplify=True)
                    prov = {"method": "place", "place_query": args.place}
                else:
                    west, south, east, north = [float(x) for x in args.bbox.split(",")]
                    G = ox.graph_from_bbox((west, south, east, north), network_type="drive", simplify=True)
                    prov = {"method": "bbox", "bbox_west_south_east_north": args.bbox}
                prov["overpass_url"] = url
                return G, prov
            except Exception as err:  # network errors surface as many exception types
                last_error = err
                print("  failed: %s: %s" % (type(err).__name__, str(err)[:160]))
                time.sleep(min(30, 5 * attempt))
    sys.exit("All Overpass endpoints failed (last error: %s).\n"
             "Run  python tools/check_osm_access.py  to see which hosts are reachable, or use the local-file route\n"
             "(docs/EXPORT_HELP.md): download a Geofabrik extract and pass it with --osm-file." % last_error)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="data/snapshot_hcmc")
    ap.add_argument("--bbox", default=DEFAULT_BBOX, help="west,south,east,north in degrees (default: urban HCMC)")
    ap.add_argument("--place", default="", help="legacy: geocode a place name with Nominatim (not recommended)")
    ap.add_argument("--osm-file", default="", help="read a local .osm XML file instead of downloading")
    ap.add_argument("--overpass-url", default="https://overpass-api.de/api",
                    help="Overpass base URL(s), comma separated, tried in order (osmnx appends /interpreter)")
    ap.add_argument("--retries", type=int, default=2, help="attempts per endpoint")
    ap.add_argument("--timeout", type=int, default=300, help="request timeout in seconds")
    args = ap.parse_args()
    import osmnx as ox
    G, prov = load_graph(args, ox)
    meta = {
        "source": "OpenStreetMap via osmnx",
        "osmnx_version": ox.__version__,
        "export_date_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "raw_nodes": G.number_of_nodes(),
        "raw_edges": G.number_of_edges(),
        "drivable_rule": "osmnx 'drive' exclusions re-applied in tools/export_graph.py:is_drivable",
    }
    meta.update(prov)
    out = graph_to_snapshot(G, args.out, meta)
    print("exported", out["nodes"], "nodes,", out["edges"], "edges, sha256", out["sha256"])
    sys.exit(0 if verify_snapshot(args.out) else 1)


if __name__ == "__main__":
    main()
