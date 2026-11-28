# Running experiments in Docker

Yes — Docker is a good way to run this project on **Windows** without installing Visual Studio / WSL toolchains. The container is Ubuntu with `g++`, `make`, Python 3, `matplotlib`, and `networkx`, so you get the full Linux path (`make`, `tests/run_tests.sh`).

Caveat: absolute timings will **not** match bare metal or WSL2 (Docker Desktop shares the host CPU). Use Docker for correctness checks, smoke runs, and development; for report numbers, prefer a dedicated Linux/WSL2 machine left idle.

## One-time build

From the repository root (PowerShell):

```powershell
docker build -t concurrent-astar .
# or:
docker compose build
```

## Smoke (build + tests + ~1 minute benchmarks)

Mount `results/` so CSVs and `platform_*.json` stay on the host:

```powershell
docker run --rm `
  -v "${PWD}/results:/work/results" `
  -v "${PWD}/report/generated:/work/report/generated" `
  concurrent-astar `
  python3 tools/experiment.py --platform docker --stages setup,tests,inspect,smoke
```

Or with Compose:

```powershell
docker compose run --rm exp `
  python3 tools/experiment.py --platform docker --stages setup,tests,inspect,smoke
```

Success looks like: tests print `ALL TESTS PASSED`, then smoke finishes without `INCORRECT RESULT`.

## Full measurement grid (hours)

```powershell
docker compose run --rm exp `
  python3 tools/experiment.py --platform docker --dry-run

docker compose run --rm exp `
  python3 tools/experiment.py --platform docker
```

Stages are resumable; re-run the same command after an interrupt.

### Faster real run (recommended on Docker Desktop)

Default protocol is **10 reps × ~450 queries × 100+ configs**, plus heavy vector/window stages — often many hours inside a WSL2 VM.

Speed without dropping to smoke:

```powershell
docker compose run --rm exp `
  python3 tools/experiment.py --platform docker `
  --reps 3 --threads 1,2,4,14 `
  --stages main,fine,optimistic,vector,window-threads,analyze
```

| Change | Effect |
|--------|--------|
| `--reps 3` | ~3× less work per config (default 10) |
| `--threads 1,2,4,14` | skip oversubscribed T28 (slow + noisy on Docker) |
| skip `vector-plain`, `window-sweep` | drop timeout-heavy / combinatorial stages; keep headline + preset curves |

Also give WSL more RAM if you can (`%UserProfile%\.wslconfig`, e.g. `memory=16GB`), then `wsl --shutdown` and restart Docker — vector A* benefits a lot.

## Quick unit / Section-2 checks only

```powershell
docker run --rm concurrent-astar make test
docker run --rm concurrent-astar ./build/ex2_list both
```

## Keeping results for git

`docker compose` mounts host folders into the container:

* `./results` → `/work/results`
* `./report/generated` → `/work/report/generated`

CSVs, `platform_*.json`, analysis tables/figures, and report assets are written on the **host**, so they survive container exit and can be committed/pushed. They are **not** listed in `.gitignore` (same as the existing `results/ryzen-wsl2/` tree).

After a full run finishes:

```powershell
git status results report/generated
git add results/platform_docker.json results/docker report/generated
# commit when you are ready
```

Do **not** delete `results/docker/` while a stage is running; stages are resumable and skip finished configs.

## Tips

* Pick any short `--platform` id (e.g. `docker`); `inspect` writes `results/platform_<id>.json`.
* Edit sources on the host, then **rebuild the image** (`docker build` / `docker compose build`) before measuring again, or mount the whole repo as a volume and run `make` inside the container.
* For more stable timings, close other apps and optionally pin CPUs in `docker-compose.yml` (`cpuset`) to match your physical core count.
* `include/lock.h` is already in the image via `COPY`; no extra step.
* `.dockerignore` excludes `results/` only from the **image build** (keeps builds small); runtime still uses the host bind mount.
