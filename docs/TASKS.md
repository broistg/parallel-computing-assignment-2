# Tasks to do in parallel with Section 2 (concurrent lists)

Updated 17 Nov 2026. No owners/dates assigned (as requested). "Blocked by" shows real dependencies.

## A. What to run on your own machine (in this order)

Assumes WSL2 (Ubuntu) or Linux/macOS. Windows-native: use CMake (`cmake -B build && cmake --build build --config Release`) and run the Python tools the same way.

1. **Install the new files.** Extract the zip over the repo. Copy the project's A1 `lock.h` to `include/lock.h` (this repo has never been compiled against the real file, only against a small stand-in, so the first build is a real check).
2. **Build and test.** `./tests/run_tests.sh` -> must end with `ALL TESTS PASSED`. If the compile fails inside `lock.h`, send me the first error line. On Linux/WSL2 also run `make test-tsan` (about 45 s) and, once, `make test-tsan-stress` (several minutes: it adds the 48k-node synthetic graph, which the target generates; every stage prints its own time, so a slow stage is visible. A stage that is merely slow is normal, a stage with 0% CPU is a real hang: tell me). Both must print `0 failures` and no `WARNING: ThreadSanitizer`. TSan does not exist for macOS ARM with Apple clang the same way; on the Mac just run `run_tests.sh`.
3. **Record the platform.** `python3 tools/inspect_platform.py --name <short-id>` (e.g. `ryzen-wsl2`) -> commit `results/platform_<short-id>.json`. Do this on EVERY machine, including the Mac and any native-Windows run.
4. **Freeze the real graph (ONE person, ONCE).**
   First `python tools/check_osm_access.py` (shows which OSM hosts you can reach). Then `pip install osmnx networkx` and `python tools/export_graph.py --out data/snapshot_hcmc` (bounding box via Overpass; no Nominatim request). If it fails with a connection error, add mirrors: `--overpass-url https://overpass.private.coffee/api,https://maps.mail.ru/osm/tools/overpass/api`; if every host fails, use the local-file route in `docs/EXPORT_HELP.md` (Geofabrik extract + `--osm-file`), or let a teammate with a working network export it. It may take several minutes. It prints node/edge counts and a hash. Commit `data/snapshot_hcmc/` (about tens of MB). Everyone else only runs `python tools/snapshot_io.py data/snapshot_hcmc` and checks the hash matches.
   If `bench` later fails with `FIFO guard: an edge is too long`, tell me the message: it means a very long edge needs splitting or the profile slope needs lowering (the fix is small).
5. **Freeze queries and golden files.**
   `python tools/make_queries.py --snapshot data/snapshot_hcmc --per-bin 50 --seed 7 --out config/queries.csv`
   `python tools/oracle.py golden --snapshot data/snapshot_hcmc --profile config/profile.csv --queries config/queries.csv --obj time --out config/golden_time.csv`
   `... --obj dist --out config/golden_dist.csv`
   The Python oracle is slow (budget roughly 1 s per query on 100k nodes, so 10-20 min per file). Commit the CSVs AND their `.meta.json` sidecars. If a distance bin has fewer than 50 pairs (the city may be smaller than 25 km across), the script prints it: tell the group.
6. **Pilot.** `make` then `python tools/pilot.py --snapshot data/snapshot_hcmc --platform <short-id> --threads 1,2,4,8` -> commit `results/pilot_<short-id>.json`. Use thread counts that fit the machine (see the platform file). This decides pairs per bin, which bins are too short to time, and whether the 5 h budget fits.
7. **Send back:** the `run_tests.sh` summary, platform JSON, snapshot hash and counts, pilot JSON, and any error text.

## B. Shared task list

| # | Task | Blocked by | Status |
|---|------|-----------|--------|
| 1 | Freeze HCMC snapshot (A.4). First attempt failed at the Nominatim geocoding step (connection reset); exporter now defaults to a bounding box, supports mirrors and a local file | working route to OSM data | in progress |
| 2 | Frozen queries + golden time/dist (A.5) | 1 | not started |
| 3 | Run tests on all three machines, especially Apple M3 (ARM memory model) and each Windows setup | repo + real `lock.h` | not started |
| 4 | Platform inspection JSON per machine (A.3) | - | script ready |
| 5 | Pilot per platform; fix pairs/bins/thread counts/budget | 1, 2 | script ready |
| 6 | Read `osm-astar.ipynb`, list the 9 bugs, write one regression test per bug (Section 1 requirement) | - | not started |
| 7 | Sequential vector (Pareto, NAMOA*) A* in C++, validated against `oracle.py pareto` and brute force | spec 1 | specified, not coded |
| 8 | Parallel vector A* (per-node lock + label sets, same termination rule) | 7 | not started |
| 9 | Pareto golden fronts, 10 pairs/bin (`oracle.py pareto`) | 1 | not started |
| 10 | Register Section-2 lists in `kListRegistry` (one line each), instantiate `test_list` / `test_search` for them, add the list micro-benchmark with latency and op mix | Section 2 code | interface frozen |
| 11 | `tools/run_experiments.py`: block-randomised order, one process per configuration, pinned vs unpinned tagged separately, writes `results/<platform>/` | 5 | not written |
| 12 | Analysis scripts: tables and figures only from CSVs (median/IQR, speedup and efficiency vs sequential, gap and hypervolume, memory, op mix and latency); test them on synthetic CSVs first | 11 | not written |
| 13 | Investigate why parallel is slower than sequential in the pilot (coarse lock is the suspect); re-measure with fine-grained/lazy lists before drawing conclusions. Also measure the FIFO-lock convoy: the real `lock_ticket` was 16x slower than TTAS when threads outnumbered cores (see report, section 4) | 10 | observation only |
| 14 | Decide landmark heuristic (ALT) by 22 Nov; contraction hierarchies skipped, to be explained in the report | - | open decision |
| 15 | Decide Windows policy: WSL2 only, or also a native MSVC/MinGW build (CMake file exists, untested) | 3 | open decision |
| 16 | Confirm open parameters (PARAMETERS.md) at the group meeting, incl. bucket width and fixed departures | - | open decision |
| 17 | Report skeleton, submission checklist (zip = PDF report + sources), deadline 5 Dec 2026 | - | not started |

## C. Known gaps (so nobody is surprised)
* All timings so far come from a 2-core cloud sandbox and a synthetic graph. Nothing measured counts for the report.
* The profile (rush-hour curve) is invented; say so in the report.
* The Windows-native and macOS code paths (affinity, memory measurement) and the macOS/Windows branches of `inspect_platform.py` are written but untested.
* Vector (Pareto) search exists only as a specification and as the Python oracle.
