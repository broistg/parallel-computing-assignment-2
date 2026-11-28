# Runbook for a teammate's machine (internal; not part of the submission)

One command does everything below: `python3 tools/experiment.py --platform <short-id>`. Use these steps the first time so a
failure is easy to locate. `<short-id>` examples: `ryzen-wsl2`, `m3-mac`, `win-native`.

## Before you start
1. Pull / extract the repository. The frozen files (`data/snapshot_hcmc/`, `config/queries*.csv`, `config/golden_*.csv` and their `.meta.json`) must be present: NEVER regenerate them.
2. Copy the Assignment-1 `lock.h` to `include/lock.h`.
3. Python 3 with `matplotlib` (`pip install matplotlib`, for the figures) and a C++17 compiler (g++ or clang++). Windows machines run everything inside WSL2 (Ubuntu); native Windows is not supported. Close other heavy programs; plug the laptop in; disable sleep. Leave the machine idle during measurement stages.
4. After EVERY change to the sources (new zip extracted): rebuild (`python3 tools/experiment.py --platform <id> --stages setup`). The driver refuses to run with a stale binary and tells you.

## Steps
```
python3 tools/experiment.py --platform <id> --stages setup,tests,inspect,smoke
```
* `tests` must end with `ALL TESTS PASSED`. On Linux/WSL2 also run `make test-tsan` once (about 45 s) and, once, `make test-tsan-stress` (several minutes).
* `inspect` writes `results/platform_<id>.json`: open it and check the core counts and the thread list (Apple M3: no pinning; performance cores first). Tell us if something is wrong; thread counts above the physical cores are oversubscribed.
* `smoke` takes about a minute.
```
python3 tools/experiment.py --platform <id> --dry-run
python3 tools/experiment.py --platform <id> --stages main,fine,optimistic,vector,vector-plain,window-sweep,window-threads,analyze
```
The dry run prints the number of configurations per stage and a time estimate: send us those lines first (the full run takes hours; start it when the machine can stay idle). Each stage can be rerun alone and is resumable (finished configurations are skipped). An interrupted stage leaves `.partial` files that are never used.

## If something goes wrong
* `INCORRECT RESULT` (exit code 2): a configuration differed from the golden file. Stop, send the full output.
* `PROBLEMS` with timeouts: expected for plain lists on long vector queries; keep going.
* `... is older than the sources` / `does not know the options`: rebuild.
* Compile error inside `lock.h`: send the first error line.

## Send back
`results/<id>/analysis/` (tables, figures, `window_summary.md`), `report/generated/<id>/` (figures and tables for the report; written by the `analyze` stage, small), `results/platform_<id>.json`, and the text of any error. The raw CSVs are large: send them only if asked (zip `results/<id>/`).

## Final run (after the freeze; the coordinator says when)
The numbers in the report must all come from ONE tagged commit and an empty results folder:
1. Extract the final zip / `git checkout <tag>`; check `git describe` prints the tag.
2. Move the old `results/<id>/` out of the repository (do not delete it), keep `results/platform_<id>.json`.
3. Rebuild, then run the commands of "Steps" again, all stages (including `window-sweep`, which is the small per-platform parameter sweep).
4. Send the same files as above, plus the printed `git describe`.
`tools/opt_diag.py` (explanation of the optimistic list and the latency table) is run on ONE machine only (see `docs/TASKS.md` A.26); the others skip it.
