// bench_main: runs one (algorithm, list, thread count) configuration over the query set and
// appends one CSV row per (query, repetition).  One process = one configuration, so the
// reported peak memory belongs to that configuration.  Schema: docs/INTERFACES.md (schema_ver 1).
#include <sys/types.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "astar/search.h"
#include "lock.h"  // Assignment 1 lock family (copy the project's lock.h into include/)

#if defined(_WIN32)
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

using namespace astar;

static long peak_memory_kb() {
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

struct Query {
    int id, distance_bin;
    uint32_t source, target;
    int64_t departure_ms;
};

// Registry of open-list implementations.  Section-2 variants (fine-grained, lazy, ...) add one line here.
using SearchFunction = SearchResult (*)(const Problem&, ThreadPool&, ParWorkspace&);
struct ListEntry {
    const char* list_name;
    const char* lock_name;
    SearchFunction search;
};
static const ListEntry kListRegistry[] = {
    {"coarse_ttas", "ttas_pause", &par_astar<CoarseList<lock_ttas_pause>>},
    {"coarse_ticket", "ticket", &par_astar<CoarseList<lock_ticket>>},
    {"coarse_std_mutex", "std_mutex", &par_astar<CoarseList<std::mutex>>},  // library-lock baseline
};

static std::map<std::string, std::string> parse_command_line(int argc, char** argv) {
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
static std::string json_string_value(const std::string& json, const std::string& key) {
    size_t at = json.find("\"" + key + "\"");
    if (at == std::string::npos) return "";
    size_t start = json.find('"', json.find(':', at) + 1) + 1;
    return json.substr(start, json.find('"', start) - start);
}

// A golden file is only valid for the exact snapshot, profile and query set it was computed from.
static void verify_golden_provenance(const std::string& golden_path, const Graph& graph, const SpeedProfile& profile,
                                     const std::string& queries_text) {
    std::string json = read_file(golden_path + ".meta.json");
    struct Check { const char* key; uint64_t actual; };
    const Check checks[] = {{"snapshot_fnv", graph.content_hash}, {"profile_fnv", profile.content_hash}, {"queries_fnv", fnv1a(queries_text)}};
    for (const Check& check : checks)
        if (json_string_value(json, check.key) != to_hex16(check.actual))
            throw std::runtime_error(std::string("golden file does not match its inputs (") + check.key +
                                     "): regenerate it with tools/oracle.py");
}

int main(int argc, char** argv) try {
    auto options = parse_command_line(argc, argv);
    auto option = [&](const char* key, const char* fallback) { return options.count(key) ? options[key] : std::string(fallback); };
    std::string snapshot_dir = option("snapshot", "data/snapshot_hcmc"), profile_path = option("profile", "config/profile.csv"),
                queries_path = option("queries", "config/queries.csv"), golden_path = option("golden", ""),
                algorithm = option("algo", "par"), list_name = option("list", "coarse_ttas"), output_path = option("out", "results.csv"),
                platform_name = option("platform", "unknown");
    int num_threads = std::stoi(option("threads", "1")), repetitions = std::stoi(option("reps", "10"));
    int is_smoke_run = options.count("smoke") ? 1 : 0;
    Objective objective = option("obj", "time") == "dist" ? Objective::Distance : Objective::Time;
    HeuristicConfig heuristic_config;
    heuristic_config.noise_permille = std::stoi(option("rho", "0"));        // CLI/CSV keep the short names rho / w
    heuristic_config.inflation_percent = std::stoi(option("w", "100"));
    heuristic_config.noise_seed = std::stoull(option("seed", "1"));
    list_latency_sampling().store(options.count("latency") > 0);
    std::vector<int> cpu_ids;
    {
        std::stringstream cpu_list(option("cpus", ""));
        std::string token;
        while (std::getline(cpu_list, token, ',')) if (!token.empty()) cpu_ids.push_back(std::stoi(token));
    }

    SpeedProfile profile;
    profile.load(profile_path);
    Graph graph;
    graph.load(snapshot_dir, profile);
    std::string queries_text = read_file(queries_path);
    std::vector<Query> queries;
    {
        std::vector<int64_t> rows = parse_int_csv(queries_text, 5);
        for (size_t i = 0; i < rows.size(); i += 5)
            queries.push_back({int(rows[i]), int(rows[i + 1]), uint32_t(rows[i + 2]), uint32_t(rows[i + 3]), rows[i + 4]});
    }
    std::map<int, int64_t> golden_cost_of_query;
    if (!golden_path.empty()) {
        if (!options.count("allow-unverified-golden")) verify_golden_provenance(golden_path, graph, profile, queries_text);
        std::ifstream golden_file(golden_path);
        std::string line;
        std::getline(golden_file, line);  // header
        while (std::getline(golden_file, line)) {
            std::stringstream fields(line);
            std::string id, objective_name, cost;
            std::getline(fields, id, ',');
            std::getline(fields, objective_name, ',');
            std::getline(fields, cost, ',');
            if (!cost.empty() && cost != "None") golden_cost_of_query[std::stoi(id)] = std::stoll(cost);
        }
    }
    // config_id: platform-independent fingerprint of everything that must be identical across machines.
    std::string config_id;
    {
        std::string key = "v1|" + std::to_string(graph.content_hash) + "|" + std::to_string(profile.content_hash) + "|" + queries_text + "|" +
                          (objective == Objective::Time ? "time" : "dist") + "|" + std::to_string(heuristic_config.noise_permille) + "|" +
                          std::to_string(heuristic_config.inflation_percent) + "|" + std::to_string(heuristic_config.noise_seed) + "|" +
                          std::to_string(repetitions);
        config_id = to_hex16(fnv1a(key));
    }
    const ListEntry* list_entry = nullptr;
    if (algorithm == "par")
        for (const ListEntry& entry : kListRegistry) if (list_name == entry.list_name) list_entry = &entry;
    if (algorithm == "par" && !list_entry) throw std::runtime_error("unknown list " + list_name);
    const bool is_sequential = (algorithm == "seq" || algorithm == "seqlist");
    if (algorithm != "par" && !is_sequential) throw std::runtime_error("unknown algo " + algorithm + " (seq | seqlist | par)");
    if (is_sequential) num_threads = 1;

    ThreadPool pool(num_threads, algorithm == "par" ? cpu_ids : std::vector<int>{});
    ParWorkspace par_workspace;
    SeqWorkspace seq_workspace;
    bool is_new_file = !std::ifstream(output_path).good();
    FILE* out = std::fopen(output_path.c_str(), "a");
    if (!out) throw std::runtime_error("cannot open " + output_path);
    if (is_new_file)  // column names are the frozen schema (docs/INTERFACES.md)
        std::fprintf(out,
            "schema_ver,config_id,platform,smoke,algo,list,lock,threads,pinned,obj,rho_permille,w_percent,query_id,bin,src,dst,dep_ms,"
            "rep,warmup,search_ns,init_ns,found,cost_reported,cost_checked,golden_cost,gap_ppm,path_ok,expansions,reopens,stale,pruned,"
            "inserts,relax_ok,cas_fail,idle_spins,list_adds,list_add_dups,list_pops,list_pop_empty,list_add_ns,list_add_samples,"
            "list_pop_ns,list_pop_samples,peak_rss_kb\n");

    for (int repetition = 0; repetition <= repetitions; ++repetition) {  // repetition 0 = warm-up
        for (const Query& query : queries) {
            Problem problem{&graph, &profile, objective, query.source, query.target, query.departure_ms, heuristic_config};
            SearchResult result = algorithm == "seq" ? seq_astar(problem, seq_workspace)
                                  : algorithm == "seqlist" ? seq_astar_list<CoarseList<NullLock>>(problem, seq_workspace)
                                                           : list_entry->search(problem, pool, par_workspace);
            auto golden_it = golden_cost_of_query.find(query.id);
            int64_t golden_cost = golden_it != golden_cost_of_query.end() ? golden_it->second : -1;
            bool path_ok = result.found && result.checked_cost >= 0 && result.checked_cost <= result.reported_cost;
            bool has_gap = golden_cost > 0 && result.found && result.checked_cost >= 0;
            long gap_ppm = has_gap ? long((result.checked_cost - golden_cost) * 1000000 / golden_cost) : 0;
            const Counters& c = result.counters;
            const ListOpStats& l = result.list_stats;
            auto u = [](uint64_t value) { return static_cast<unsigned long long>(value); };
            std::fprintf(out, "1,%s,%s,%d,%s,%s,%s,%d,%d,%s,%d,%d,%d,%d,%u,%u,%lld,%d,%d,%lld,%lld,%d,%lld,%lld,%lld,%s,%d,"
                              "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%ld\n",
                config_id.c_str(), platform_name.c_str(), is_smoke_run, algorithm.c_str(),
                algorithm == "seq" ? "pq" : algorithm == "seqlist" ? "sorted_nolock" : list_entry->list_name,
                is_sequential ? "none" : list_entry->lock_name, num_threads,
                pool.pinned() ? 1 : 0, objective == Objective::Time ? "time" : "dist", heuristic_config.noise_permille,
                heuristic_config.inflation_percent, query.id, query.distance_bin, query.source, query.target, (long long)query.departure_ms,
                repetition, repetition == 0 ? 1 : 0, (long long)result.search_ns, (long long)result.init_ns, result.found ? 1 : 0,
                (long long)result.reported_cost, (long long)result.checked_cost, (long long)golden_cost,
                has_gap ? std::to_string(gap_ppm).c_str() : "", path_ok ? 1 : 0, u(c.expansions), u(c.reopens), u(c.stale_skips),
                u(c.pruned), u(c.inserts), u(c.improvements), u(c.cas_failures), u(c.idle_spins), u(l.adds), u(l.add_dups), u(l.pops),
                u(l.pop_empty), u(l.add_ns), u(l.add_samples), u(l.pop_ns), u(l.pop_samples), peak_memory_kb());
        }
    }
    std::fclose(out);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
}
