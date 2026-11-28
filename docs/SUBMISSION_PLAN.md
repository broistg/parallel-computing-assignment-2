# Submission refactor plan (agreed 20 Nov 2026) — PARTLY EXECUTED 21 Nov (see the status block); the rest waits for the freeze

Deliverable (spec): ONE zip = final PDF report + all relevant Python/C++ source, to the course website by **5 Dec 2026**. At least as many shared-memory platforms as group members (3).

## Status 21 Nov (decisions of 21 Nov: ALT only after a clean submission; one machine runs native Windows (DROPPED 28 Nov: Windows machines use WSL2); lock variants skipped; Section-2 lists merged later)
Done (changes_30): presets file; guards (unknown options, `--version`, stale binary, manifest with commit and presets); `tools/experiment.py` (one command per machine, also native Windows); `tools/make_submission.py` (zip without internal documents, generated DESIGN.md / TESTING.md, `--verify`); README rewritten; `docs/RUNBOOK.md` (teammates); `docs/COMPLIANCE.md` (spec checklist); unmeasured windowed lock variants removed from registry and tests; `pareto-plain` grid; `analyze.py` also runs `analyze_window.py` and analyses `pareto_plain`.
Done 26 Nov (approved): merging `analyze_window.py` into `analyze.py`, the report-figure script, the direct "front equals sequential front" check in `bench_pareto`. Not done on purpose (after the freeze): deleting internal files from the repository (the zip script leaves them out instead), `config/experiment.sh` (replaced by `tools/experiment.py`), tagging the final commit.

**26 Nov: Section-2 lists merged (decision 33).** `ex2/` is part of the zip (`make_submission.py` includes it), is built by make / CMake and run by `run_tests.sh`; the `optimistic` stage of `tools/experiment.py` measures the optimistic list. Remaining before the freeze: run A.25 and decide whether the optimistic results go into the report.

## What is missing before the FREEZE (28 Nov)
Freeze = no more source changes; then refactor deletions, tag, final runs. Blocking (they can force code changes):
1. **Third member's machine (WSL2; native Windows was dropped 28 Nov and its code paths removed)**: `tools/experiment.py --stages setup,tests,inspect,smoke` (A.24), so that every platform has built and passed the tests before the freeze. The group must confirm that this machine exists, since the spec needs at least three platforms.
2. **Apple M3**: the same four stages (A.24). First run on ARM and macOS (memory model, `inspect_platform.py` macOS branch, memory measurement); no TSan there, so `run_tests.sh` is the check.
3. **Section 2 evaluation scenario** (teammate): if it needs a stand-alone list micro-benchmark or other code, that code must exist before the freeze. If the A* scenario is the chosen one, nothing is needed beyond the report text.
4. **Group decision on the ALT commit and tag policy** (`docs/ALT_PLAN.md`), so that the freeze tag is chosen knowing what comes after it.
Not blocking (can be done after the freeze or are decided): ALT code; the add-side gap of `win_opt_ttas` (documented as observed, not investigated); exponential backoff (optional, stated as not done); the report's open items (title page, work split, conclusion, abstract, Section 2 text, third-machine and M3 tables); `docs/COMPLIANCE.md` is up to date except the report-language row (Vietnamese, decided).
After the freeze, in order: delete or exclude internal files (zip script already excludes them), tag, final runs on three machines from an empty results folder (RUNBOOK "Final run"), `tools/analyze.py` and `tools/make_report_figures.py` on the final results, fill the `\todo` numbers, `tools/make_submission.py --verify`.

## Decisions of the group (20 Nov)
1. **Section 2 lists** are merged later; the group compares its own fine-grained list with my `FineList` (`include/astar/fine_list.h`) and keeps ONE fine-grained variant. Until then both stay registered.
2. ~~Windows native is kept~~ **Reversed 28 Nov: native Windows is dropped.** The `_WIN32` branches (affinity, memory), the MSVC / WIN32 parts of CMake, and the Windows branches of `experiment.py`, `run_experiments.py`, `inspect_platform.py` and `make_submission.py` were removed; Windows machines use WSL2 (as in Assignment 1). `include/lock.h` (the course file) is untouched.
3. **Zip content**: code, tables, figures, golden files, snapshot compressed. Raw per-run CSVs only if the size limit of the course website allows (ask: limit unknown).
4. **Unmeasured windowed variants** (`win_coarse_ticket`, `win_coarse_std_mutex`, `win_fine_ticket`, `win_fine_std_mutex`): decision PENDING. They were registered and tested for symmetry but the sweeps used TTAS only (cost: each list = 32 sweep cells per search type; ticket collapses when threads exceed cores; std::mutex was the slowest lock in every earlier measurement). Proposal: one small lock comparison at the chosen setting (scalar divisor 4096 / batch 8, vector divisor 16384 / batch 8; threads 1, 2, 4, 6, 12; ticket capped at the physical core count), then keep only the variants that show a difference worth reporting.
5. **Final runs**: after the freeze, rerun everything on all three machines from ONE tagged commit (the Ryzen headline predates the insert-profile counters), plus the small window-parameter sweep per platform (divisors 1024, 4096, 16384 x batches 4, 8, 16 at 1 thread and at the physical core count). Report both the common default and the platform-tuned best, and say that parameters were tuned on the queries they are scored on.

## What stays in the report
| Experiment | Where |
|---|---|
| Scalar main study: sequential heap, sequential sorted list without lock, parallel x lists x threads x pinned/unpinned | Section 3 headline (data structure vs lock vs contention split) |
| Distance objective | one table |
| Heuristic sensitivity (noise x inflation) | Section 1 (dynamic/uncertain h) + quality metrics |
| Vector (Pareto) search, hypervolume | Sections 1 and 3 |
| Plain lists on the vector search: small proper run (bins 0-1, T1 and 6 threads, timeouts recorded); the smoke run is tagged smoke and cannot be cited | Section 3 |
| Fine vs coarse lists | Sections 2-3 |
| Insert profile (walk length, position) | one small table, diagnosis |
| Windowed list: parameter sweep + thread scaling | Section 4, labelled a variant of the specified sorted list; 4x4 sweep as one figure/appendix table |
| Pilot, diagnostic and smoke runs | method note only |

## Code changes (no algorithm change)
* Delete from the zip: `tools/summarize.py`, `tools/check_osm_access.py`, `docs/TASKS.md`, `docs/REPORT*.md`, `docs/PARAMETERS.md`, this file (internal group documents).
* DONE 26 Nov: `analyze_window.py` merged into `analyze.py` (`--window-only` for the quick summary); report figures and tables: `tools/make_report_figures.py`.
* Registry lists exactly what the report measures (+ group Section-2 lists); sandbox-only bits (`LOCK_INC`) and stale comments removed.
* Docs: `README.md` (build, test, run, reproduce, layout), `DESIGN.md` (Specs 1-3 + decisions), `TESTING.md` (trust ladder, mutation checks, TSan).
* `config/experiment.sh`: one command per platform = build, tests, platform inspection, grids, analysis.
* `bench_pareto`: add a direct check "front equals the sequential C++ front" for queries without a golden front.

## Regression check for the refactor
Expansions per query are deterministic (sequential: 23,026 per query on the scalar bins; vector sequential 230,163 per query). After the refactor: `./tests/run_tests.sh` passes, `check_gaps.py` shows 0 mismatches, a smoke rerun reproduces those expansion counts exactly.

## Order of work
1. Vector bins 2-4 diagnosis (running now). 2. Section-2 merge and FineList decision. 3. Lock comparison for the windowed variants (decision 4). 4. FREEZE. 5. Refactor + regression check. 6. Final runs on all three machines from one tagged commit. 7. Figures, report PDF, zip. Deadline 5 Dec.
