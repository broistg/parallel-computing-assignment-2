#!/usr/bin/env bash
# One-command test run (Linux / WSL2 / macOS).  Every layer of the trust ladder, with watchdogs
# so a broken termination protocol fails instead of hanging.
#   LOCK_INC=<dir with lock.h>  make sure include/lock.h (A1 file) exists or set LOCK_INC.
set -euo pipefail
cd "$(dirname "$0")/.."
TO=${TEST_TIMEOUT:-600}
tout() { if command -v timeout >/dev/null; then timeout "$TO" "$@"; elif command -v gtimeout >/dev/null; then gtimeout "$TO" "$@"; else "$@"; fi; }

echo "== L0 data tools"
python3 tools/make_profile.py config/profile.csv >/dev/null
[ -d data/snapshot_synth_small ] || python3 tools/synth_graph.py --rows 40 --cols 40 --seed 1 --out data/snapshot_synth_small
[ -d data/snapshot_synth_large ] || python3 tools/synth_graph.py --rows 220 --cols 220 --seed 2 --out data/snapshot_synth_large
python3 tools/snapshot_io.py data/snapshot_synth_small

echo "== L1 oracle vs brute force"
python3 tests/test_oracle.py

echo "== build"
make all LOCK_INC="${LOCK_INC:-include}" >/dev/null

echo "== L2-L5 C++ (data invariants, lists, CAS-min, search vs Dijkstra, stress)"
tout ./build/test_astar data/snapshot_synth_small config/profile.csv data/snapshot_synth_large

echo "== L6 C++ vs Python golden file"
TMP=$(mktemp -d)
python3 tools/make_queries.py --snapshot data/snapshot_synth_small --per-bin 6 --bins-km 0.3,1,2,4,100 --seed 7 --out "$TMP/q.csv" >/dev/null
python3 tools/oracle.py golden --snapshot data/snapshot_synth_small --profile config/profile.csv --queries "$TMP/q.csv" --out "$TMP/g.csv" >/dev/null
for cfg in "seq 1 coarse_ttas" "par 1 coarse_ttas" "par 4 coarse_ttas" "par 8 coarse_ticket" "par 4 coarse_std_mutex"; do
  set -- $cfg
  tout ./build/bench --snapshot data/snapshot_synth_small --queries "$TMP/q.csv" --golden "$TMP/g.csv" --algo "$1" --threads "$2" --list "$3" --reps 3 --platform test --smoke --out "$TMP/r.csv"
done
python3 tests/check_gaps.py "$TMP/r.csv"

echo "== L6b other bucket width (288 buckets of 5 min): C++ == Python"
python3 tools/make_profile.py "$TMP/profile288.csv" --buckets 288 >/dev/null
python3 tools/oracle.py golden --snapshot data/snapshot_synth_small --profile "$TMP/profile288.csv" --queries "$TMP/q.csv" --out "$TMP/g288.csv" >/dev/null
tout ./build/bench --snapshot data/snapshot_synth_small --profile "$TMP/profile288.csv" --queries "$TMP/q.csv" --golden "$TMP/g288.csv" --algo par --threads 4 --list coarse_std_mutex --reps 2 --platform test --smoke --out "$TMP/r288.csv"
python3 tests/check_gaps.py "$TMP/r288.csv"

echo "== L6c a golden file from different inputs must be rejected"
if ./build/bench --snapshot data/snapshot_synth_small --profile "$TMP/profile288.csv" --queries "$TMP/q.csv" --golden "$TMP/g.csv" --algo seq --reps 1 --out "$TMP/never.csv" 2>"$TMP/err.txt"; then
  echo "FAIL: stale golden accepted"; exit 1
else
  grep -q "does not match its inputs" "$TMP/err.txt" && echo "stale golden rejected: OK"
fi
echo "ALL TESTS PASSED  (optional: make test-tsan)"
