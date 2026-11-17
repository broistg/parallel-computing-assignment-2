#!/usr/bin/env python3
"""Fails if any measured row differs from the golden cost or has an invalid path."""
import csv
import sys

rows = list(csv.DictReader(open(sys.argv[1])))
bad = [r for r in rows if r["gap_ppm"] != "0" or r["path_ok"] != "1"]
print("golden check: %d rows, %d mismatches" % (len(rows), len(bad)))
sys.exit(1 if bad else 0)
