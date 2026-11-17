#!/usr/bin/env python3
"""Record the exact experiment platform.  Run on EVERY machine before any measurement and
commit the resulting results/platform_<name>.json.  Standard library only.

    python3 tools/inspect_platform.py --name ryzen-wsl2 [--out results/]

Captures: OS / kernel, WSL2 vs native Windows vs macOS vs Linux, CPU model, physical and logical
cores, P/E core split (Apple), SMT, cache sizes, RAM, governor/power plan hints, compiler
versions, whether hard thread pinning is possible and how, how peak memory is measured, and the
list of CPUs to use for a pinned run ("one thread per physical core first").
"""
import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys


def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=20).stdout.strip()
    except Exception:
        return ""


def detect_os():
    s = platform.system()
    if s == "Linux":
        rel = platform.release().lower()
        wsl = "microsoft" in rel or "wsl" in rel or "microsoft" in sh("cat /proc/version").lower()
        return ("wsl2" if wsl else "linux")
    return {"Darwin": "macos", "Windows": "windows-native"}.get(s, s.lower())


def linux_info(info):
    cpuinfo = open("/proc/cpuinfo").read() if os.path.exists("/proc/cpuinfo") else ""
    m = re.search(r"model name\s*:\s*(.+)", cpuinfo)
    info["cpu_model"] = m.group(1).strip() if m else platform.processor()
    info["logical_cores"] = os.cpu_count()
    # physical cores = unique (physical id, core id); WSL2 often hides topology -> fall back to lscpu
    pairs = set(re.findall(r"physical id\s*:\s*(\d+)\s*\n(?:.*\n)*?core id\s*:\s*(\d+)", cpuinfo))
    lscpu = sh("lscpu")
    tpc = re.search(r"Thread\(s\) per core:\s*(\d+)", lscpu)
    cps = re.search(r"Core\(s\) per socket:\s*(\d+)", lscpu)
    soc = re.search(r"Socket\(s\):\s*(\d+)", lscpu)
    if tpc and cps and soc:
        info["smt_threads_per_core"] = int(tpc.group(1))
        info["physical_cores"] = int(cps.group(1)) * int(soc.group(1))
    elif pairs:
        info["physical_cores"] = len(pairs)
    info["lscpu_raw_cache"] = re.findall(r"(L[123]d?\s+cache:.*)", lscpu)
    mem = re.search(r"MemTotal:\s*(\d+) kB", open("/proc/meminfo").read())
    info["ram_gb"] = round(int(mem.group(1)) / 1048576, 1) if mem else None
    info["governor"] = sh("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null") or "unknown"
    info["pinning"] = "sched_setaffinity (pthread_setaffinity_np)"
    info["pinning_possible"] = hasattr(os, "sched_setaffinity")
    if info["pinning_possible"]:
        info["allowed_cpus"] = sorted(os.sched_getaffinity(0))
    info["peak_memory_method"] = "getrusage ru_maxrss (KB), one process per configuration"
    # CPU list for pinned runs: first hardware thread of each physical core, then SMT siblings
    first, rest, seen = [], [], set()
    for cpu in info.get("allowed_cpus", range(info["logical_cores"])):
        sib = sh("cat /sys/devices/system/cpu/cpu%d/topology/thread_siblings_list 2>/dev/null" % cpu)
        key = sib or str(cpu)
        (rest if key in seen else first).append(cpu)
        seen.add(key)
    info["pin_order_physical_first"] = first + rest
    if info["os"] == "wsl2":
        info["threats"] = ["WSL2 is a VM: host scheduler may preempt vCPUs; topology/SMT may be hidden; "
                           "check .wslconfig (processors, memory) and record it"]
        info["wslconfig"] = sh("cat /mnt/c/Users/*/.wslconfig 2>/dev/null")[:500]


def macos_info(info):
    info["cpu_model"] = sh("sysctl -n machdep.cpu.brand_string")
    info["logical_cores"] = int(sh("sysctl -n hw.logicalcpu") or 0)
    info["physical_cores"] = int(sh("sysctl -n hw.physicalcpu") or 0)
    p, e = sh("sysctl -n hw.perflevel0.physicalcpu"), sh("sysctl -n hw.perflevel1.physicalcpu")
    if p:
        info["performance_cores"], info["efficiency_cores"] = int(p), int(e or 0)
    info["ram_gb"] = round(int(sh("sysctl -n hw.memsize") or 0) / 2**30, 1)
    info["cache_line_bytes"] = sh("sysctl -n hw.cachelinesize")
    info["l1d_l2_bytes"] = [sh("sysctl -n hw.l1dcachesize"), sh("sysctl -n hw.l2cachesize")]
    info["pinning"] = "none (macOS has no hard affinity); QoS hints only"
    info["pinning_possible"] = False
    info["peak_memory_method"] = "getrusage ru_maxrss (BYTES on macOS, converted to KB), one process per configuration"
    info["power"] = sh("pmset -g batt | head -1")
    info["threats"] = ["unpinned: scheduler may move threads between P and E cores; keep on AC power, "
                       "close other apps, thread counts above the P-core count mix core types"]
    info["pin_order_physical_first"] = []


def windows_info(info):
    info["cpu_model"] = platform.processor()
    info["logical_cores"] = os.cpu_count()
    out = sh('wmic cpu get NumberOfCores,NumberOfLogicalProcessors /value') or ""
    m = re.search(r"NumberOfCores=(\d+)", out)
    if m:
        info["physical_cores"] = int(m.group(1))
    info["ram_gb"] = None
    info["pinning"] = "SetThreadAffinityMask (first 64 logical CPUs)"
    info["pinning_possible"] = True
    info["peak_memory_method"] = "GetProcessMemoryInfo PeakWorkingSetSize"
    info["power_plan"] = sh("powercfg /getactivescheme")
    info["pin_order_physical_first"] = list(range(0, info["logical_cores"], 2)) + list(range(1, info["logical_cores"], 2))
    info["threats"] = ["Windows native: 15.6 ms default timer granularity affects sleep-based backoff; "
                       "check power plan = High performance"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True, help="short platform id used in result files, e.g. ryzen-wsl2")
    ap.add_argument("--out", default="results")
    a = ap.parse_args()
    info = {"platform_name": a.name, "os": detect_os(), "kernel": platform.release(), "machine": platform.machine(),
            "python": sys.version.split()[0]}
    {"macos": macos_info, "windows-native": windows_info}.get(info["os"], linux_info)(info)
    for cc in ("g++", "clang++", "cl"):
        if shutil.which(cc):
            info["compiler_" + cc] = (sh(cc + " --version") or sh(cc)).splitlines()[0] if sh(cc + " --version") or sh(cc) else ""
    info["cmake"] = (sh("cmake --version") or "").splitlines()[0:1]
    if "physical_cores" in info and info.get("logical_cores"):
        info["max_threads_to_test"] = info["logical_cores"]
        t = sorted({1, 2, 4, info["physical_cores"], info["logical_cores"]} |
                   ({info["performance_cores"]} if "performance_cores" in info else set()))
        info["suggested_thread_counts"] = [x for x in t if x <= info["logical_cores"]]
    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, "platform_%s.json" % a.name)
    with open(path, "w") as f:
        json.dump(info, f, indent=2, sort_keys=True)
        f.write("\n")
    print(json.dumps(info, indent=2, sort_keys=True))
    print("\nwritten:", path)


if __name__ == "__main__":
    main()
