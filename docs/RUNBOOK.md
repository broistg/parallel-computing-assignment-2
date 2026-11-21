# Runbook for a teammate's machine (internal; not part of the submission)

One command does everything below: `python3 tools/experiment.py --platform <short-id>`. Use these steps the first time so a
failure is easy to locate. `<short-id>` examples: `ryzen-wsl2`, `m3-mac`, `win-native`.

## Before you start
1. Pull / extract the repository. The frozen files (`data/snapshot_hcmc/`, `config/queries*.csv`, `config/golden_*.csv` and their `.meta.json`) must be present: NEVER regenerate them.
2. Copy the Assignment-1 `lock.h` to `include/lock.h`.
3. Python 3 and a C++17 compiler (Windows native: Visual Studio Build Tools + CMake). Close other heavy programs; plug the laptop in; disable sleep. Leave the machine idle during measurement stages.
4. After EVERY change to the sources (new zip extracted): rebuild (`python3 tools/experiment.py --platform <id> --stages setup`). The driver refuses to run with a stale binary and tells you.

## Steps
```
python3 tools/experiment.py --platform <id> --stages setup,tests,inspect,smoke
```
* `tests` must end with `ALL TESTS PASSED` (native Windows: both test programs print their summary with 0 failures). On Linux/WSL2 also run `make test-tsan` once (about 45 s) and, once, `make test-tsan-stress` (several minutes).
* `inspect` writes `results/platform_<id>.json`: open it and check the core counts and the thread list (Apple M3: no pinning; performance cores first). Tell us if something is wrong; thread counts above the physical cores are oversubscribed.
* `smoke` takes about a minute.
```
python3 tools/experiment.py --platform <id> --dry-run
python3 tools/experiment.py --platform <id> --stages main,fine,vector,vector-plain,window-sweep,window-threads,analyze
```
The dry run prints the number of configurations per stage: send us those lines first. Each stage can be rerun alone and is resumable (finished configurations are skipped). An interrupted stage leaves `.partial` files that are never used.

## If something goes wrong
* `INCORRECT RESULT` (exit code 2): a configuration differed from the golden file. Stop, send the full output.
* `PROBLEMS` with timeouts: expected for plain lists on long vector queries; keep going.
* `... is older than the sources` / `does not know the options`: rebuild.
* Compile error inside `lock.h`: send the first error line.

## Send back
`results/<id>/analysis/` (tables, figures, `window_summary.md`), `results/platform_<id>.json`, and the text of any error. The raw CSVs are large: send them only if asked (zip `results/<id>/`).
