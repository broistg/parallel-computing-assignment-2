#!/usr/bin/env python3
"""Tells you which OpenStreetMap hosts are reachable from THIS machine (standard library only).

    python tools/check_osm_access.py

Typical reading of the result:
  * "Connection reset" during the TLS handshake  -> something between you and the host drops the
    connection (ISP, VPN/proxy, antivirus HTTPS scanning, WSL2 DNS/MTU).  Try another host, or the
    local-file route in docs/EXPORT_HELP.md.
  * HTTP 429 / 406 -> you are rate limited; wait a minute.
  * HTTP 200 -> fine.
"""
import ssl
import time
import urllib.request

HOSTS = [
    ("Nominatim (geocoding; only needed by --place)", "https://nominatim.openstreetmap.org/status.php"),
    ("Overpass main (overpass-api.de)", "https://overpass-api.de/api/status"),
    ("Overpass private.coffee mirror", "https://overpass.private.coffee/api/status"),
    ("Overpass VK Maps mirror", "https://maps.mail.ru/osm/tools/overpass/api/status"),
    ("Geofabrik downloads (local-file route)", "https://download.geofabrik.de/asia/vietnam.html"),
]


def main():
    ctx = ssl.create_default_context()
    for name, url in HOSTS:
        start = time.time()
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "parallel-computing-assignment-2 connectivity check"})
            with urllib.request.urlopen(req, timeout=20, context=ctx) as r:
                result = "HTTP %d" % r.status
        except Exception as err:
            result = "FAILED  %s: %s" % (type(err).__name__, str(err)[:100])
        print("%-48s %-9.1fs %s" % (name, time.time() - start, result))


if __name__ == "__main__":
    main()
