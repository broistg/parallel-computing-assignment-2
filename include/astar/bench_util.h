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
#include "openlist.h"

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


// Insert profile (--list-profile): one row per measured (query, repetition) in <out>.listprof.csv; the main result CSV keeps its frozen schema.
// pos0..pos7 = how many inserts landed in the 1st..8th eighth of the list (pos0 = near the head, pos7 = near the tail).
inline void append_list_profile(const std::string& path, const std::string& config_id, const char* list_name, int threads, int query_id, int bin,
                                int repetition, const ListOpStats& l) {
    bool is_new_file = !std::ifstream(path).good();
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) return;
    if (is_new_file)
        std::fprintf(f, "config_id,list,threads,query_id,bin,rep,list_adds,list_pops,ins_profiled,ins_steps,ins_size_sum,ins_size_max,"
                        "pos0,pos1,pos2,pos3,pos4,pos5,pos6,pos7\n");
    auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    std::fprintf(f, "%s,%s,%d,%d,%d,%d,%llu,%llu,%llu,%llu,%llu,%llu", config_id.c_str(), list_name, threads, query_id, bin, repetition, u(l.adds),
                 u(l.pops), u(l.ins_profiled), u(l.ins_steps), u(l.ins_size_sum), u(l.ins_size_max));
    for (int i = 0; i < ListOpStats::kPositionBuckets; ++i) std::fprintf(f, ",%llu", u(l.ins_position[i]));
    std::fprintf(f, "\n");
    std::fclose(f);
}

}  // namespace astar
