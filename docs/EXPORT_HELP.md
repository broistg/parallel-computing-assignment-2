# Getting the HCMC graph when the download fails

Step 0, always: `python tools/check_osm_access.py` shows which hosts you can reach.

## Why you saw `Connection reset by peer`
The traceback ended in `osmnx/geocoder.py -> _nominatim.py`: the failure was the *geocoding* request to
`nominatim.openstreetmap.org` (asking "where is Ho Chi Minh City?"), before any map data was requested. A reset
during the TLS handshake means the connection is dropped on the way (ISP, VPN/proxy, antivirus HTTPS scanning,
WSL2 DNS/MTU), not that the server refused a query. A different Overpass mirror alone would not have helped,
because the first request never reached Overpass. The default export now needs **no geocoding**.

## Route A: bounding box via Overpass (default)
```
python tools/export_graph.py --out data/snapshot_hcmc
# if overpass-api.de also fails, list mirrors (tried in order):
python tools/export_graph.py --out data/snapshot_hcmc --overpass-url https://overpass.private.coffee/api,https://maps.mail.ru/osm/tools/overpass/api
```
The box `106.55,10.65,106.90,10.95` (west,south,east,north) covers urban HCMC and contains the starter notebook's
origin and destination. A box also avoids a real ambiguity: HCMC was merged with Binh Duong and Ba Ria - Vung Tau
in 2025, so "Ho Chi Minh City" as a place name may now mean a much larger area than the notebook intended
(I have not checked what OSM currently returns; the box makes it irrelevant).

## Route B: local file (works without Overpass; best for reproducibility)
1. Download the Vietnam extract (a few hundred MB): `wget https://download.geofabrik.de/asia/vietnam-latest.osm.pbf`
2. `sudo apt install osmium-tool`
3. Cut the box and convert to XML:
   `osmium extract -b 106.55,10.65,106.90,10.95 vietnam-latest.osm.pbf -o hcmc.osm.pbf`
   `osmium cat hcmc.osm.pbf -o hcmc.osm`
4. `python tools/export_graph.py --osm-file hcmc.osm --out data/snapshot_hcmc`

The exporter stores the sha256 of the input file in `meta.json`, so the graph can be traced to a fixed, archivable
input. Keep `vietnam-latest.osm.pbf` (or `hcmc.osm.pbf`) somewhere the group can reach and note its name and date.
The same drivable-road rule is applied in Python for every route, so A and B give the same kind of graph
(not the identical one: Overpass and a Geofabrik file are different moments in time).

## Route C: somebody else exports
If your network cannot reach any of the hosts, anyone in the group with a working connection can run Route A once,
commit `data/snapshot_hcmc/`, and everybody verifies the hash with `python tools/snapshot_io.py data/snapshot_hcmc`.

## Check the result
The script prints the node and edge counts and the sha256. Expect on the order of tens to a few hundred thousand
nodes; if it is more than about 500k nodes tell the group, because the Python oracle gets slow. Then continue with
step 5 of `docs/TASKS.md` part A.
