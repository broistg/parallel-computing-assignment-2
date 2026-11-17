#!/usr/bin/env python3
"""Tests the exporter without network access: the drivable-road rule, and the local-file route
(tools/export_graph.py --osm-file) end to end with the real osmnx.  Run: python3 tests/test_export.py"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
from export_graph import is_drivable  # noqa: E402
from snapshot_io import verify_snapshot  # noqa: E402

OSM = """<?xml version='1.0' encoding='UTF-8'?>
<osm version="0.6" generator="test">
 <node id="1" lat="10.7700" lon="106.7000"/><node id="2" lat="10.7700" lon="106.7020"/>
 <node id="3" lat="10.7700" lon="106.7040"/><node id="4" lat="10.7715" lon="106.7040"/>
 <node id="5" lat="10.7730" lon="106.7040"/><node id="6" lat="10.7730" lon="106.7000"/>
 <way id="100"><nd ref="1"/><nd ref="2"/><nd ref="3"/><tag k="highway" v="primary"/></way>
 <way id="101"><nd ref="3"/><nd ref="4"/><nd ref="5"/><tag k="highway" v="residential"/><tag k="oneway" v="yes"/></way>
 <way id="102"><nd ref="5"/><nd ref="6"/><nd ref="1"/><tag k="highway" v="secondary"/></way>
 <way id="103"><nd ref="2"/><nd ref="6"/><tag k="highway" v="footway"/></way>
 <way id="104"><nd ref="4"/><nd ref="6"/><tag k="highway" v="service"/><tag k="service" v="driveway"/></way>
</osm>
"""


def main():
    fails = 0

    def check(cond, msg):
        nonlocal fails
        if not cond:
            fails += 1
            print("FAIL:", msg)

    # drivable rule
    check(is_drivable({"highway": "residential"}), "residential drivable")
    check(not is_drivable({"highway": "footway"}), "footway excluded")
    check(not is_drivable({"highway": ["footway", "path"]}), "footway+path excluded")
    check(is_drivable({"highway": ["residential", "footway"]}), "mixed tags kept")
    check(not is_drivable({"highway": "service", "service": "driveway"}), "driveway excluded")
    check(not is_drivable({"highway": "residential", "motor_vehicle": "no"}), "motor_vehicle=no excluded")
    check(not is_drivable({"highway": "residential", "area": "yes"}), "area excluded")
    check(not is_drivable({}), "no highway tag excluded")

    with tempfile.TemporaryDirectory() as tmp:
        osm = os.path.join(tmp, "mini.osm")
        out = os.path.join(tmp, "snap")
        open(osm, "w").write(OSM)
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "export_graph.py"), "--osm-file", osm, "--out", out],
                           capture_output=True, text=True)
        check(r.returncode == 0, "export from local file failed:\n" + r.stdout[-600:] + r.stderr[-600:])
        if r.returncode == 0:
            check(verify_snapshot(out), "snapshot hash")
            meta = json.load(open(os.path.join(out, "meta.json")))
            check(meta.get("method") == "osm_file" and len(meta.get("osm_file_sha256", "")) == 64, "provenance recorded")
            check(meta["nodes"] >= 3 and meta["edges"] >= 6, "graph too small: %s" % meta)
            check(meta["edges"] < meta["raw_edges"], "excluded ways should reduce the edge count")
    print("export tests: %s" % ("ALL OK" if not fails else "%d FAILURES" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
