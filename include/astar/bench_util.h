#pragma once
// Helpers shared by bench (scalar A*) and bench_pareto (vector A*): command line, peak memory, golden-file provenance.
#include <sys/types.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
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

// A misspelt or unsupported option must stop the run: silently ignoring it once produced results for the wrong configuration
// (a stale binary that did not know --window-skip-empty).  Each main lists every option it understands.
inline std::string known_options_text(std::initializer_list<const char*> known) {
    std::string text;
    for (const char* key : known) text += std::string(text.empty() ? "" : " ") + key;
    return text;
}

inline void require_known_options(const std::map<std::string, std::string>& options, std::initializer_list<const char*> known) {
    for (const auto& entry : options) {
        bool is_known = false;
        for (const char* key : known) is_known = is_known || entry.first == key;
        if (!is_known) throw std::runtime_error("unknown option --" + entry.first + " (--version lists the options of this build)");
    }
}

// --version output: build time and the options this binary understands (tools/run_experiments.py checks both before it starts).
inline void print_version(const char* program, std::initializer_list<const char*> known) {
    std::printf("%s build %s %s\noptions: %s\n", program, __DATE__, __TIME__, known_options_text(known).c_str());
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

// --diag sidecar <out>.diag.csv: where threads wait (lock, empty list) and who holds work while a thread is idle.
inline void append_list_diag(const std::string& path, const std::string& config_id, const char* list_name, int threads, int query_id, int bin,
                             int repetition, long long search_ns, const ListOpStats& l) {
    bool is_new_file = !std::ifstream(path).good();
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) return;
    if (is_new_file)
        std::fprintf(f, "config_id,list,threads,query_id,bin,rep,search_ns,lock_acq,lock_wait_ns,lock_wait_max_ns,idle_episodes,idle_ns,idle_max_ns,"
                        "idle_final_ns,idle_samples,hoard_sum,hoard_max,held_sum,shared_sum,takes,take_items,take_short,hold_ns,hold_max_ns,hold_ge100us,hold_ge1ms,hold_ge1ms_add,hold_ge1ms_size_sum,list_size_sum,list_size_max,list_size_ge1ms_max,promotes,promoted_items,promoted_max,advances,advance_gap_sum,advance_gap_max,advance_far\n");
    auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    std::fprintf(f, "%s,%s,%d,%d,%d,%d,%lld,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", config_id.c_str(), list_name,
                 threads, query_id, bin, repetition, search_ns, u(l.lock_acq), u(l.lock_wait_ns), u(l.lock_wait_max_ns), u(l.idle_episodes), u(l.idle_ns),
                 u(l.idle_max_ns), u(l.idle_final_ns), u(l.idle_samples), u(l.hoard_sum), u(l.hoard_max), u(l.held_sum), u(l.shared_sum), u(l.takes),
                 u(l.take_items), u(l.take_short), u(l.hold_ns), u(l.hold_max_ns), u(l.hold_ge100us), u(l.hold_ge1ms), u(l.hold_ge1ms_add),
                 u(l.hold_ge1ms_size_sum), u(l.list_size_sum), u(l.list_size_max), u(l.list_size_ge1ms_max), u(l.promotes), u(l.promoted_items), u(l.promoted_max), u(l.advances), u(l.advance_gap_sum), u(l.advance_gap_max), u(l.advance_far));
    std::fclose(f);
}

}  // namespace astar
