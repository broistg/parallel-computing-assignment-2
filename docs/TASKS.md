# Tasks to do in parallel with Section 2 (concurrent lists)

Updated 18 Nov 2026. No owners/dates assigned (as requested). "Blocked by" shows real dependencies.

## A. What to run on your own machine (in this order)

Assumes WSL2 (Ubuntu) or Linux/macOS. Windows-native: use CMake (`cmake -B build && cmake --build build --config Release`) and run the Python tools the same way.

1. **Install the new files.** Extract the zip over the repo. Copy the project's A1 `lock.h` to `include/lock.h` (this repo has never been compiled against the real file, only against a small stand-in, so the first build is a real check).
2. **Build and test.** `./tests/run_tests.sh` -> must end with `ALL TESTS PASSED`. If the compile fails inside `lock.h`, send me the first error line. On Linux/WSL2 also run `make test-tsan` (about 45 s) and, once, `make test-tsan-stress` (several minutes: it adds the 48k-node synthetic graph, which the target generates; every stage prints its own time, so a slow stage is visible. A stage that is merely slow is normal, a stage with 0% CPU is a real hang: tell me). Both must print `0 failures` and no `WARNING: ThreadSanitizer`. TSan does not exist for macOS ARM with Apple clang the same way; on the Mac just run `run_tests.sh`.
3. **Record the platform.** `python3 tools/inspect_platform.py --name <short-id>` (e.g. `ryzen-wsl2`) -> commit `results/platform_<short-id>.json`. Do this on EVERY machine, including the Mac and any native-Windows run.
4. **Freeze the real graph (ONE person, ONCE).**
   First `python tools/check_osm_access.py` (shows which OSM hosts you can reach). Then `pip install osmnx networkx` and `python tools/export_graph.py --out data/snapshot_hcmc` (bounding box via Overpass; no Nominatim request). If it fails with a connection error, add mirrors: `--overpass-url https://overpass.private.coffee/api,https://maps.mail.ru/osm/tools/overpass/api`; if every host fails, use the local-file route in `docs/EXPORT_HELP.md` (Geofabrik extract + `--osm-file`), or let a teammate with a working network export it. It may take several minutes. It prints node/edge counts and a hash. Commit `data/snapshot_hcmc/` (about tens of MB). Everyone else only runs `python tools/snapshot_io.py data/snapshot_hcmc` and checks the hash matches.
   If `bench` later fails with `FIFO guard: an edge is too long`, tell me the message: it means a very long edge needs splitting or the profile slope needs lowering (the fix is small).
5. **Freeze queries and golden files.**
   `python tools/make_queries.py --snapshot data/snapshot_hcmc --per-bin 200,100,50,50,50 --seed 7 --out config/queries.csv`
   `python tools/oracle.py golden --snapshot data/snapshot_hcmc --profile config/profile.csv --queries config/queries.csv --obj time --out config/golden_time.csv`
   `... --obj dist --out config/golden_dist.csv`
   The Python oracle is slow (budget roughly 1 s per query on 100k nodes, so 10-20 min per file). Commit the CSVs AND their `.meta.json` sidecars. If a distance bin has fewer than 50 pairs (the city may be smaller than 25 km across), the script prints it: tell the group.
6. **Pilot.** `make` then `python tools/pilot.py --snapshot data/snapshot_hcmc --platform <short-id> --threads 1,2,4,8` -> commit `results/pilot_<short-id>.json`. Use thread counts that fit the machine (see the platform file). This decides pairs per bin, which bins are too short to time, and whether the 5 h budget fits.
7. **Send back:** the `run_tests.sh` summary, platform JSON, snapshot hash and counts, pilot JSON, and any error text.

8. **Diagnose the pilot's slowdown and check correctness on the real graph** (about 10 minutes). Rows are tagged `--smoke` (planning data), so the summarizer needs `--include-smoke`:
```
for cfg in "seq 1 coarse_ttas" "par 1 coarse_ttas" "par 2 coarse_ttas" "par 4 coarse_ttas" "par 2 coarse_std_mutex"; do
  set -- $cfg
  ./build/bench --snapshot data/snapshot_hcmc --queries config/queries.csv --golden config/golden_time.csv \
     --algo $1 --threads $2 --list $3 --reps 3 --latency --platform ryzen-wsl2 --smoke --out results/diag_ryzen.csv
done
python3 tools/summarize.py results/diag_ryzen.csv --include-smoke
python3 tests/check_gaps.py results/diag_ryzen.csv
./build/bench --snapshot data/snapshot_hcmc --queries config/queries.csv --golden config/golden_dist.csv \
     --obj dist --algo par --threads 4 --list coarse_ttas --reps 1 --platform ryzen-wsl2 --smoke --out results/diag_dist.csv
python3 tests/check_gaps.py results/diag_dist.csv
```
`check_gaps.py` must say `0 mismatches` (this is the first check of the Python golden files against the C++ search on the real graph). In the summarizer table compare `exp/q` (same in seq and par = no extra search work), `add_ns`/`pop_ns` (list latency) and `vs_T1`. Send me the table.
9. **Experiment driver (after regenerating queries and golden files, row 20).** `make`, then
   `python3 tools/run_experiments.py --platform ryzen-wsl2 --dry-run` (lists the configurations and gives a rough time estimate from the pilot; send me the first lines), then `--grid smoke` (about 1 minute, writes to `results/<platform>/smoke/`), then the real run `python3 tools/run_experiments.py --platform ryzen-wsl2` (resumable: if it is interrupted, run the same command again; it skips finished configurations; `--only <text>` reruns a subset). Leave the machine idle while it runs. At the end it prints PROBLEMS, if any (exit code 2 = a result differed from the golden file: tell me at once). Section-2 lists: add their names to `--lists`.
10. **Commit** `data/snapshot_hcmc/`, `config/queries.csv`, `config/golden_time.csv(.meta.json)`, `config/golden_dist.csv(.meta.json)`, `results/platform_*.json`, `results/pilot_*.json`. Never regenerate them afterwards without telling the group.

## B. Shared task list

| # | Task | Blocked by | Status |
|---|------|-----------|--------|
| 1 | Freeze HCMC snapshot: **done** 18 Nov via local OSM file (195,823 nodes, 427,793 edges, sha256 f0981c73dc996e19...); commit it | - | done, commit pending |
| 2 | Frozen queries + golden time/dist: generated (250 queries, 5 bins x 50). Decide row 20 first, then commit | 1 | generated |
| 3 | Run tests on all three machines, especially Apple M3 (ARM memory model) and each Windows setup | repo + real `lock.h` | not started |
| 4 | Platform inspection JSON per machine (A.3) | - | script ready |
| 5 | Pilot per platform; fix pairs/bins/thread counts/budget. Done on ryzen-wsl2: graph extent kept (bins 1-4 above the 20 ms floor); repeat on the other machines | 1, 2 | 1 of 3 machines |
| 6 | Optional: turn the flaws of the lecturer's starter notebook `osm-astar.ipynb` into regression tests (not in the spec; it is a ready-made list of known failure modes for the "approach to testing" bullet of Section 1; I have not yet written the flaw list) | - | optional, not started |
| 7 | Sequential vector (Pareto, NAMOA*) A* in C++, validated against `oracle.py pareto` and brute force | spec 1 | specified, not coded |
| 8 | Parallel vector A* (per-node lock + label sets, same termination rule) | 7 | not started |
| 9 | Pareto golden fronts, 10 pairs/bin (`oracle.py pareto`) | 1 | not started |
| 10 | Register Section-2 lists in `kListRegistry` (one line each), instantiate `test_list` / `test_search` for them, add the list micro-benchmark with latency and op mix | Section 2 code | interface frozen |
| 11 | `tools/run_experiments.py`: block-randomised order, one process per configuration, pinned vs unpinned tagged separately, writes `results/<platform>/run<k>/`, resumable, flags incorrect results. **Written 18 Nov, tested on the synthetic graph (79 configurations, resume, forced-failure check); not yet run on a real platform** | 5 | written |
| 12 | Analysis scripts: tables and figures only from CSVs (median/IQR, speedup and efficiency vs sequential, gap and hypervolume, memory, op mix and latency); test them on synthetic CSVs first | 11 | not written |
| 13 | Why parallel is slower than sequential: **diagnosed on the real graph (18 Nov)**: same expansions; (i) sorted-list insert is O(frontier) vs binary heap (par at 1 thread already 0.22-0.89x of seq); (ii) lock contention (threads spend ~100% of time in list ops; 0.42-0.54x of T1 at 4 threads). Remaining: re-measure with the Section-2 lists, add a sequential-with-sorted-list baseline (row 21), and the FIFO-lock convoy (real `lock_ticket` 16x slower than TTAS when threads outnumber cores, report section 4) | 10 | diagnosed |
| 14 | Decide landmark heuristic (ALT) by 22 Nov; contraction hierarchies skipped, to be explained in the report | - | open decision |
| 15 | Decide Windows policy: WSL2 only, or also a native MSVC/MinGW build (CMake file exists, untested) | 3 | open decision |
| 16 | Open parameters (PARAMETERS.md), incl. bucket width and fixed departures | - | **confirmed 18 Nov** |
| 17 | Report skeleton, submission checklist (zip = PDF report + sources), deadline 5 Dec 2026 | - | not started |
| 18 | Parallel-A* design ideas to test once Section-2 lists exist (the pilot shows no speedup with the single-lock list, and per-node work is tiny compared with a lock round trip): (a) batch insertion: add all successors of an expansion in one list traversal/lock acquisition; (b) pop several items per lock acquisition (extra out-of-order expansions are safe thanks to reopening); (c) cheaper pruning before insertion; (d) heavier per-node work (vector objective, noisy heuristic) improves parallel efficiency. A.8 confirmed the list is the bottleneck, so these are now justified; (a) and (b) need an optional `add_batch` / `pop_k` in the list API (default = loop of `add` / `pop_min`) so frozen Section-2 lists still work | 10 | justified, not coded |
| 19 | Check the Python golden files against the C++ search on the real graph (A.8) | 2 | **done**: 0 mismatches on 5,000 time rows and 500 distance rows |
| 20 | Bin 0 (1-3 km) was 10.6 ms per 50 pairs, below the 20 ms floor. **Decided 18 Nov:** pairs per bin 200,100,50,50,50 (`make_queries.py --per-bin 200,100,50,50,50`); regenerate queries and both golden files ONCE, then everybody pulls them. Re-seeding does not help (short queries stay short) | - | decided, regenerate |
| 21 | Sequential A* with the same sorted list and no lock (`--algo seqlist`): **done 18 Nov**; unit test checks it expands exactly like the heap version. Report separates: heap -> sorted list (data structure), + lock/atomics (par at 1 thread), + threads (contention) | - | done |

## C. Known gaps (so nobody is surprised)
* Only planning numbers exist (pilot and diagnostic runs, tagged `--smoke`) from one machine (Ryzen laptop, WSL2, 6 cores / 12 threads, 7.4 GB). Nothing measured counts for the report. Final thread counts for this machine: 1, 2, 4, 6, 12 (the pilot used 1, 2, 4, 8).
* The profile (rush-hour curve) is invented; say so in the report.
* The Windows-native and macOS code paths (affinity, memory measurement) and the macOS/Windows branches of `inspect_platform.py` are written but untested.
* Vector (Pareto) search exists only as a specification and as the Python oracle.
