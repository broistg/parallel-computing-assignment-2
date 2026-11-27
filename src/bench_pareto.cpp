// bench_pareto: vector (Pareto) A*, one (algorithm, list, thread count, heuristic) configuration per process.
// Appends one CSV row per (query, repetition).  Quality = comparison with the exact Pareto front of the Python oracle
// (tools/oracle.py pareto): hypervolume ratio, exact points found, front sizes.  Schema: docs/INTERFACES.md (pareto_schema_ver 1).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "astar/bench_util.h"
#include "astar/pareto.h"
#include "astar/registry.h"

using namespace astar;

struct Query {
    int id, distance_bin;
    uint32_t source, target;
    int64_t departure_ms;
};

using ParetoFunction = ParetoResult (*)(const Problem&, ThreadPool&, ParetoWorkspace<SpinLock>&);
struct ParetoListEntry {
    const char* list_name;
    const char* lock_name;
    ParetoFunction search;
};
#define ASTAR_REGISTER_PARETO(name, lock, ListType) {name, lock, &par_pareto_astar<ListType>},
static const ParetoListEntry kParetoRegistry[] = {ASTAR_OPEN_LISTS(ASTAR_REGISTER_PARETO)};  // include/astar/registry.h

using Point = std::pair<int64_t, int64_t>;  // (time_ms, dist_dm)

// Area dominated by a non-dominated set (sorted by time ascending) inside the box [0, ref_t] x [0, ref_d].
static double hypervolume(const std::vector<Point>& points, int64_t ref_t, int64_t ref_d) {
    double area = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        if (points[i].first >= ref_t || points[i].second >= ref_d) continue;
        int64_t next_t = i + 1 < points.size() ? std::min(points[i + 1].first, ref_t) : ref_t;
        area += double(next_t - points[i].first) * double(ref_d - points[i].second);
    }
    return area;
}

int main(int argc, char** argv) try {
    auto options = parse_command_line(argc, argv);
    const auto kKnownOptions = {"snapshot", "profile", "queries", "golden", "algo", "list", "out", "platform", "threads", "reps", "smoke", "rho", "w", "seed", "latency", "list-profile", "diag", "window-div", "window-skip-empty", "window-slack", "batch", "cpus", "allow-unverified-golden", "version"};
    if (options.count("version")) { print_version("bench_pareto", kKnownOptions); return 0; }
    require_known_options(options, kKnownOptions);
    auto option = [&](const char* key, const char* fallback) { return options.count(key) ? options[key] : std::string(fallback); };
    std::string snapshot_dir = option("snapshot", "data/snapshot_hcmc"), profile_path = option("profile", "config/profile.csv"),
                queries_path = option("queries", "config/queries_pareto.csv"), golden_path = option("golden", ""),
                algorithm = option("algo", "par"), list_name = option("list", "coarse_ttas"), output_path = option("out", "results_pareto.csv"),
                platform_name = option("platform", "unknown");
    int num_threads = std::stoi(option("threads", "1")), repetitions = std::stoi(option("reps", "10"));
    int is_smoke_run = options.count("smoke") ? 1 : 0;
    HeuristicConfig heuristic_config;
    heuristic_config.noise_permille = std::stoi(option("rho", "0"));
    heuristic_config.inflation_percent = std::stoi(option("w", "100"));
    heuristic_config.noise_seed = std::stoull(option("seed", "1"));
    list_latency_sampling().store(options.count("latency") > 0);
    list_insert_profiling().store(options.count("list-profile") > 0);  // extra file <out>.listprof.csv
    list_diagnostics().store(options.count("diag") > 0);                // extra file <out>.diag.csv (stall diagnostics)
    window_divisor().store(std::stoi(option("window-div", "4096")));
    window_skip_empty().store(options.count("window-skip-empty") > 0);          // WindowedList: do not lock an empty shared list
    window_slack().store(std::stoi(option("window-slack", "-1")));  // WindowedList: cap on how far above the global bag minimum a window may open (-1 = off)
    window_batch().store(std::stoi(option("batch", "8")));  // WindowedList lists only: items per shared-list access  // WindowedList lists only: window = f(source) / divisor
    std::vector<int> cpu_ids;
    {
        std::stringstream cpu_list(option("cpus", ""));
        std::string token;
        while (std::getline(cpu_list, token, ',')) if (!token.empty()) cpu_ids.push_back(std::stoi(token));
    }
    if (algorithm != "seq" && algorithm != "par") throw std::runtime_error("unknown algo " + algorithm + " (seq | par)");
    if (algorithm == "seq") num_threads = 1;
    const ParetoListEntry* list_entry = nullptr;
    if (algorithm == "par")
        for (const ParetoListEntry& entry : kParetoRegistry) if (list_name == entry.list_name) list_entry = &entry;
    if (algorithm == "par" && !list_entry) throw std::runtime_error("unknown list " + list_name);

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
    std::map<int, std::vector<Point>> golden_front;  // query id -> exact front sorted by time
    if (!golden_path.empty()) {
        if (!options.count("allow-unverified-golden")) verify_golden_provenance(golden_path, graph, profile, queries_text);
        std::ifstream golden_file(golden_path);
        std::string line;
        std::getline(golden_file, line);  // header
        if (line.rfind("query_id,idx,time_ms,dist_dm", 0) != 0) throw std::runtime_error(golden_path + " is not a Pareto golden file (tools/oracle.py pareto)");
        while (std::getline(golden_file, line)) {
            std::stringstream fields(line);
            std::string id, index, time_ms, dist_dm;
            std::getline(fields, id, ','); std::getline(fields, index, ','); std::getline(fields, time_ms, ','); std::getline(fields, dist_dm, ',');
            golden_front[std::stoi(id)].push_back({std::stoll(time_ms), std::stoll(dist_dm)});
        }
        for (auto& entry : golden_front) std::sort(entry.second.begin(), entry.second.end());
    }
    std::string config_id;
    {
        std::string key = "p1|" + std::to_string(graph.content_hash) + "|" + std::to_string(profile.content_hash) + "|" + queries_text + "|" +
                          std::to_string(heuristic_config.noise_permille) + "|" + std::to_string(heuristic_config.inflation_percent) + "|" +
                          std::to_string(heuristic_config.noise_seed) + "|" + std::to_string(repetitions);
        config_id = to_hex16(fnv1a(key));
    }

    ThreadPool pool(num_threads, algorithm == "par" ? cpu_ids : std::vector<int>{});
    ParetoWorkspace<SpinLock> par_workspace;
    ParetoWorkspace<NullLock> seq_workspace;
    bool is_new_file = !std::ifstream(output_path).good();
    FILE* out = std::fopen(output_path.c_str(), "a");
    if (!out) throw std::runtime_error("cannot open " + output_path);
    if (is_new_file)  // column names are the frozen vector schema (docs/INTERFACES.md)
        std::fprintf(out,
            "pareto_schema_ver,config_id,platform,smoke,algo,list,lock,threads,pinned,rho_permille,w_percent,query_id,bin,src,dst,dep_ms,"
            "rep,warmup,search_ns,init_ns,front_size,golden_front_size,exact_points_found,hv_ratio_ppm,dominates_golden,path_ok,overflow,"
            "expansions,dead_pops,pruned,labels_created,labels_evicted,dominated_discards,pushes,idle_spins,list_adds,list_add_dups,"
            "list_pops,list_pop_empty,list_add_ns,list_add_samples,list_pop_ns,list_pop_samples,peak_rss_kb\n");

    if (algorithm == "par" && heuristic_config.is_exact()) {
        // Queries without a golden front: the reference is the sequential C++ front (exact heuristic, so both must equal the exact front).
        // Computed once, outside the timed repetitions; the columns golden_front_size / exact_points_found / hv_ratio_ppm then compare against it.
        int derived = 0;
        for (const Query& query : queries) {
            if (golden_front.count(query.id)) continue;
            Problem reference_problem{&graph, &profile, Objective::Time, query.source, query.target, query.departure_ms, heuristic_config};
            ParetoResult reference = seq_pareto_astar(reference_problem, seq_workspace);
            if (reference.overflow || !reference.path_ok) continue;  // no trustworthy reference: leave the columns at -1
            std::vector<Point> front;
            for (const FrontPoint& point : reference.front) front.push_back({point.time_ms, point.dist_dm});
            std::sort(front.begin(), front.end());
            golden_front[query.id] = front;
            ++derived;
        }
        if (derived) std::fprintf(stderr, "note: %d queries have no golden front; the sequential C++ front is the reference for them\n", derived);
    }

    for (int repetition = 0; repetition <= repetitions; ++repetition) {  // repetition 0 = warm-up
        for (const Query& query : queries) {
            Problem problem{&graph, &profile, Objective::Time, query.source, query.target, query.departure_ms, heuristic_config};
            ParetoResult result = algorithm == "seq" ? seq_pareto_astar(problem, seq_workspace) : list_entry->search(problem, pool, par_workspace);
            std::vector<Point> found;
            for (const FrontPoint& p : result.front) found.push_back({p.time_ms, p.dist_dm});
            std::sort(found.begin(), found.end());
            long long golden_size = -1, exact_found = -1, hv_ppm = -1, dominates_golden = -1;
            auto golden_it = golden_front.find(query.id);
            if (golden_it != golden_front.end()) {
                const std::vector<Point>& exact = golden_it->second;
                golden_size = (long long)exact.size();
                exact_found = 0; dominates_golden = 0;
                for (const Point& e : exact) {
                    if (std::binary_search(found.begin(), found.end(), e)) ++exact_found;
                    for (const Point& f : found)  // a found point strictly better than an exact point: oracle or path bug
                        if (f.first <= e.first && f.second <= e.second && f != e) { ++dominates_golden; break; }
                }
                int64_t max_t = 0, max_d = 0;
                for (const Point& e : exact) { max_t = std::max(max_t, e.first); max_d = std::max(max_d, e.second); }
                int64_t ref_t = max_t + max_t / 10 + 1, ref_d = max_d + max_d / 10 + 1;  // 1.1 x the largest exact component (+1 so a one-point front has area)
                double exact_volume = hypervolume(exact, ref_t, ref_d);
                if (exact_volume > 0) hv_ppm = (long long)std::llround(hypervolume(found, ref_t, ref_d) / exact_volume * 1e6);
            }
            const ParetoCounters& c = result.counters;
            const ListOpStats& l = result.list_stats;
            if (list_insert_profiling().load() && repetition > 0)
                append_list_profile(output_path + ".listprof.csv", config_id, algorithm == "seq" ? "pq" : list_entry->list_name, num_threads, query.id, query.distance_bin, repetition, l);
            if (list_diagnostics().load() && repetition > 0)
                append_list_diag(output_path + ".diag.csv", config_id, algorithm == "seq" ? "pq" : list_entry->list_name, num_threads, query.id, query.distance_bin, repetition, (long long)result.search_ns, l);
            auto u = [](uint64_t value) { return static_cast<unsigned long long>(value); };
            std::fprintf(out, "1,%s,%s,%d,%s,%s,%s,%d,%d,%d,%d,%d,%d,%u,%u,%lld,%d,%d,%lld,%lld,%zu,%lld,%lld,%lld,%lld,%d,%d,"
                              "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%ld\n",
                config_id.c_str(), platform_name.c_str(), is_smoke_run, algorithm.c_str(),
                algorithm == "seq" ? "pq" : list_entry->list_name, algorithm == "seq" ? "none" : list_entry->lock_name, num_threads,
                pool.pinned() ? 1 : 0, heuristic_config.noise_permille, heuristic_config.inflation_percent, query.id, query.distance_bin,
                query.source, query.target, (long long)query.departure_ms, repetition, repetition == 0 ? 1 : 0, (long long)result.search_ns,
                (long long)result.init_ns, result.front.size(), golden_size, exact_found, hv_ppm, dominates_golden, result.path_ok ? 1 : 0,
                result.overflow ? 1 : 0, u(c.expansions), u(c.dead_pops), u(c.pruned), u(c.labels_created), u(c.labels_evicted),
                u(c.dominated_discards), u(c.pushes), u(c.idle_spins), u(l.adds), u(l.add_dups), u(l.pops), u(l.pop_empty), u(l.add_ns),
                u(l.add_samples), u(l.pop_ns), u(l.pop_samples), peak_memory_kb());
        }
    }
    std::fclose(out);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
}
