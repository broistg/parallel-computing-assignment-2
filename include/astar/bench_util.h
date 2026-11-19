#pragma once
// Helpers shared by bench (scalar A*) and bench_pareto (vector A*): command line, peak memory, golden-file provenance.
#include <sys/types.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

#include "graph.h"

#if defined(_WIN32)
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace astar {

inline long peak_memory_kb() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters;
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters);
    return long(counters.PeakWorkingSetSize / 1024);
#else
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
    return long(usage.ru_maxrss / 1024);  // bytes on macOS
#else
    return long(usage.ru_maxrss);  // KB on Linux
#endif
#endif
}

inline std::map<std::string, std::string> parse_command_line(int argc, char** argv) {
    std::map<std::string, std::string> options;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--", 2) != 0) throw std::runtime_error(std::string("bad argument ") + argv[i]);
        std::string key = argv[i] + 2;
        if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) options[key] = argv[++i];
        else options[key] = "1";
    }
    return options;
}

// Reads one quoted hex value such as  "snapshot_fnv": "00ff..."  from the small golden sidecar JSON.
inline std::string json_string_value(const std::string& json, const std::string& key) {
    size_t at = json.find("\"" + key + "\"");
    if (at == std::string::npos) return "";
    size_t start = json.find('"', json.find(':', at) + 1) + 1;
    return json.substr(start, json.find('"', start) - start);
}

// A golden file is only valid for the exact snapshot, profile and query set it was computed from.
inline void verify_golden_provenance(const std::string& golden_path, const Graph& graph, const SpeedProfile& profile,
                                     const std::string& queries_text) {
    std::string json = read_file(golden_path + ".meta.json");
    struct Check { const char* key; uint64_t actual; };
    const Check checks[] = {{"snapshot_fnv", graph.content_hash}, {"profile_fnv", profile.content_hash}, {"queries_fnv", fnv1a(queries_text)}};
    for (const Check& check : checks)
        if (json_string_value(json, check.key) != to_hex16(check.actual))
            throw std::runtime_error(std::string("golden file does not match its inputs (") + check.key +
                                     "): regenerate it with tools/oracle.py");
}


}  // namespace astar
