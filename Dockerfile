# Concurrent A* experiments — Linux container (use this on Windows via Docker Desktop).
# Build:  docker build -t concurrent-astar .
# Smoke:  docker run --rm -v "%cd%/results:/work/results" concurrent-astar \
#           python3 tools/experiment.py --platform docker --stages setup,tests,inspect,smoke
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        g++ \
        make \
        python3 \
        python3-matplotlib \
        python3-networkx \
        python3-pip \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
COPY . .

# Host checkouts on Windows may ship CRLF; bash rejects `set -o pipefail\r`.
RUN find . -type f \( -name '*.sh' -o -path './tools/*.py' \) -print0 \
    | xargs -0 sed -i 's/\r$//' \
 && sed -i 's/\r$//' Makefile || true

# Pre-build binaries so smoke/main stages do not pay compile cost every time
# (experiment.py --stages setup still rebuilds if sources change).
RUN make -j"$(nproc)" all

# Default: show how to drive experiments. Override the command when running.
CMD ["python3", "tools/experiment.py", "--list"]
