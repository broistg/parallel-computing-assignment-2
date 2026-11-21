// Tests of the vector (Pareto) A*.  Usage: test_pareto <snapshot_dir> <profile.csv> [stress_snapshot_dir]
// Reference = an independent multi-objective label-correcting search written here (no heuristic, no open list, FIFO queue).
// Cross-language check against the Python oracle (Martins) is done by tests/run_tests.sh through bench_pareto.
#include <chrono>
#include <cstdio>
#include <deque>
#include <random>

#include "astar/fine_list.h"
#include "astar/pareto.h"
#include "lock.h"

using namespace astar;

static std::atomic<int> g_fail{0}, g_checks{0};
#define CHECK(c, ...)                                              \
    do {                                                           \
        ++g_checks;                                                \
        if (!(c)) {                                                \
            ++g_fail;                                              \
            std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #c); \
            std::printf(__VA_ARGS__);                              \
            std::printf("\n");                                     \
        }                                                          \
    } while (0)

using Point = std::pair<int64_t, int64_t>;  // (time_ms, dist_dm)

static std::vector<Point> ref_pareto(const Problem& problem) {
    const Graph& graph = *problem.graph;
    Problem time_problem = problem; time_problem.objective = Objective::Time;
    std::vector<std::vector<Point>> sets(graph.num_nodes);
    std::deque<std::pair<uint32_t, Point>> queue;
    sets[problem.source].push_back({0, 0});
    queue.push_back({problem.source, {0, 0}});
    while (!queue.empty()) {
        auto [node, label] = queue.front();
        queue.pop_front();
        bool still_alive = false;
        for (const Point& p : sets[node]) if (p == label) still_alive = true;
        if (!still_alive) continue;
        for (uint32_t e = graph.first_edge[node]; e < graph.first_edge[node + 1]; ++e) {
            uint32_t next = graph.edge_target[e];
            Point cand{label.first + time_problem.edge_cost(e, label.first), label.second + int64_t(graph.edge_length_dm[e])};
            bool dominated = false;
            for (const Point& p : sets[next]) if (p.first <= cand.first && p.second <= cand.second) { dominated = true; break; }
            if (dominated) continue;
            std::vector<Point> kept;
            for (const Point& p : sets[next]) if (!(cand.first <= p.first && cand.second <= p.second)) kept.push_back(p);
            kept.push_back(cand);
            sets[next] = kept;
            queue.push_back({next, cand});
        }
    }
    std::vector<Point> front = sets[problem.target];
    std::sort(front.begin(), front.end());
    return front;
}

static std::vector<Point> as_points(const ParetoResult& result) {
    std::vector<Point> out;
    for (const FrontPoint& p : result.front) out.push_back({p.time_ms, p.dist_dm});
    std::sort(out.begin(), out.end());
    return out;
}

static bool well_formed(const ParetoResult& result) {  // sorted by time asc, distance strictly desc, no overflow
    if (result.overflow || !result.path_ok) return false;
    for (size_t i = 1; i < result.front.size(); ++i)
        if (!(result.front[i - 1].time_ms < result.front[i].time_ms && result.front[i - 1].dist_dm > result.front[i].dist_dm)) return false;
    return true;
}

static std::vector<Problem> make_queries(const Graph& graph, const SpeedProfile& profile, int count, HeuristicConfig config, uint64_t seed) {
    std::mt19937_64 rng(seed);
    const int64_t deps[] = {3 * 3600000LL, 8 * 3600000LL, 17 * 3600000LL + 1800000};
    std::vector<Problem> out;
    for (int q = 0; q < count; ++q) {
        uint32_t s = rng() % graph.num_nodes, d = (q == 0) ? s : rng() % graph.num_nodes;
        out.push_back(Problem{&graph, &profile, Objective::Time, s, d, deps[q % 3], config});
    }
    return out;
}

constexpr int kQueries = 30, kRepeats = 3;

static void test_sequential(const Graph& graph, const SpeedProfile& profile) {
    ParetoWorkspace<NullLock> workspace;
    int multi_point_fronts = 0;
    for (const Problem& problem : make_queries(graph, profile, kQueries, HeuristicConfig{}, 11)) {
        ParetoResult result = seq_pareto_astar(problem, workspace);
        std::vector<Point> ref = ref_pareto(problem);
        CHECK(well_formed(result), "seq: front malformed s=%u d=%u", problem.source, problem.target);
        CHECK(as_points(result) == ref, "seq front differs from reference s=%u d=%u (%zu vs %zu points)", problem.source, problem.target,
              result.front.size(), ref.size());
        if (ref.size() > 1) ++multi_point_fronts;
    }
    CHECK(multi_point_fronts >= 3, "test is too weak: only %d queries have a front with more than one point", multi_point_fronts);
    std::printf("  (queries with a multi-point front: %d of %d)\n", multi_point_fronts, kQueries);
    // relaxed heuristic: valid paths, never better than the exact front
    HeuristicConfig noisy; noisy.noise_permille = 250; noisy.inflation_percent = 130;
    for (const Problem& problem : make_queries(graph, profile, kQueries, noisy, 12)) {
        Problem exact = problem; exact.heuristic_config = HeuristicConfig{};
        std::vector<Point> ref = ref_pareto(exact);
        ParetoResult result = seq_pareto_astar(problem, workspace);
        CHECK(result.path_ok && !result.overflow && !result.front.empty(), "relaxed: invalid result");
        for (const FrontPoint& p : result.front) {
            bool covered = false;
            for (const Point& e : ref) if (e.first <= p.time_ms && e.second <= p.dist_dm) covered = true;
            CHECK(covered, "relaxed: found point (%lld,%lld) better than the exact front", (long long)p.time_ms, (long long)p.dist_dm);
        }
    }
}

template <class L>
static void test_parallel(const Graph& graph, const SpeedProfile& profile, const char* name, int max_threads) {
    ParetoWorkspace<NullLock> seq_workspace;
    for (int num_threads : {1, 2, 4, 8}) {
        if (num_threads > max_threads) continue;
        ThreadPool pool(num_threads, {});
        ParetoWorkspace<SpinLock> workspace;
        for (const Problem& problem : make_queries(graph, profile, kQueries, HeuristicConfig{}, 11)) {
            std::vector<Point> ref = as_points(seq_pareto_astar(problem, seq_workspace));
            for (int rep = 0; rep < kRepeats; ++rep) {
                ParetoResult result = par_pareto_astar<L>(problem, pool, workspace);
                CHECK(well_formed(result) && as_points(result) == ref, "par %s threads=%d s=%u d=%u: %zu points vs %zu expected", name, num_threads,
                      problem.source, problem.target, result.front.size(), ref.size());
                CHECK(result.list_stats.adds >= result.list_stats.pops || result.front.size() <= 1, "list op counters");
            }
        }
    }
}

// The sorted-list open list costs O(list length) per insert, and vector search keeps very large open lists (one 25 km query on the
// 48k-node graph: 356k labels, 0.35 s sequential but 18 s with the parallel sorted list at ANY thread count). The stress test therefore
// only uses queries that the sequential search finishes with at most kStressMaxExpansions expansions.
constexpr uint64_t kStressMaxExpansions = 20000;

template <class L>
static void test_stress(const Graph& graph, const SpeedProfile& profile, const char* name, int max_threads) {
    ParetoWorkspace<NullLock> seq_workspace;
    std::vector<std::pair<Problem, std::vector<Point>>> cases;
    std::mt19937_64 rng(21);
    const int64_t deps[] = {3 * 3600000LL, 8 * 3600000LL, 17 * 3600000LL + 1800000};
    for (int attempt = 0; attempt < 400 && cases.size() < 8; ++attempt) {
        uint32_t source = rng() % graph.num_nodes, target = source;
        for (int step = 0, steps = 40 + int(rng() % 80); step < steps; ++step) {  // random walk: a nearby target keeps the query short
            uint32_t degree = graph.first_edge[target + 1] - graph.first_edge[target];
            target = graph.edge_target[graph.first_edge[target] + rng() % degree];
        }
        if (target == source) continue;
        Problem problem{&graph, &profile, Objective::Time, source, target, deps[attempt % 3], HeuristicConfig{}};
        ParetoResult result = seq_pareto_astar(problem, seq_workspace);
        if (result.counters.expansions >= 1500 && result.counters.expansions <= kStressMaxExpansions) cases.push_back({problem, as_points(result)});
    }
    CHECK(cases.size() >= 4, "stress: only %zu suitable queries found", cases.size());
    for (int num_threads : {2, 4, 8, 16}) {
        if (num_threads > max_threads) continue;
        ThreadPool pool(num_threads, {});
        ParetoWorkspace<SpinLock> workspace;
        for (auto& [problem, ref] : cases) {
            for (int rep = 0; rep < 3; ++rep) {
                ParetoResult result = par_pareto_astar<L>(problem, pool, workspace);
                CHECK(well_formed(result) && as_points(result) == ref, "stress %s threads=%d s=%u d=%u", name, num_threads, problem.source, problem.target);
            }
        }
    }
}

template <class F>
static void stage(const char* name, F&& body) {
    auto start = std::chrono::steady_clock::now();
    std::printf("[stage] %-32s ...", name);
    std::fflush(stdout);
    int before = g_fail.load();
    body();
    std::printf(" %.1f s (failures so far: %d)\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), g_fail.load());
    (void)before;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: %s snapshot profile [stress_snapshot]\n", argv[0]); return 2; }
    SpeedProfile profile; profile.load(argv[2]);
    Graph graph; graph.load(argv[1], profile);
    std::printf("graph: %u nodes, %u edges\n", graph.num_nodes, graph.num_edges);
    const int cores = int(std::max(2u, std::thread::hardware_concurrency()));
    stage("pareto sequential vs reference", [&] { test_sequential(graph, profile); });
    stage("pareto par coarse_ttas", [&] { test_parallel<CoarseList<lock_ttas_pause>>(graph, profile, "coarse_ttas", 8); });
    stage("pareto par coarse_ticket (<=cores)", [&] { test_parallel<CoarseList<lock_ticket>>(graph, profile, "coarse_ticket", cores); });
    stage("pareto par coarse_std_mutex", [&] { test_parallel<CoarseList<std::mutex>>(graph, profile, "coarse_std_mutex", 8); });
    stage("pareto par fine_ttas", [&] { test_parallel<FineList<lock_ttas_pause>>(graph, profile, "fine_ttas", 8); });
    stage("pareto par fine_std_mutex", [&] { test_parallel<FineList<std::mutex>>(graph, profile, "fine_std_mutex", 8); });
    for (auto [divisor, batch] : std::vector<std::pair<int, int>>{{2, 1}, {16, 4}, {1024, 16}}) {
        window_divisor().store(divisor); window_batch().store(batch);
        std::string label = "pareto par win_coarse_ttas div=" + std::to_string(divisor) + " batch=" + std::to_string(batch);
        stage(label.c_str(), [&] { test_parallel<WindowedList<CoarseList<lock_ttas_pause>>>(graph, profile, "win_coarse_ttas", 8); });
    }
    for (auto [divisor, batch] : std::vector<std::pair<int, int>>{{2, 1}, {16, 4}, {1024, 16}}) {
        window_divisor().store(divisor); window_batch().store(batch);
        std::string label = "pareto par win_fine_ttas div=" + std::to_string(divisor) + " batch=" + std::to_string(batch);
        stage(label.c_str(), [&] { test_parallel<WindowedList<FineList<lock_ttas_pause>>>(graph, profile, "win_fine_ttas", 8); });
    }
    for (int slack : {-1, 0, 4}) {
        window_divisor().store(16); window_batch().store(4); window_slack().store(slack);
        std::string label = "pareto par win_coarse_ttas slack=" + std::to_string(slack);
        stage(label.c_str(), [&] { test_parallel<WindowedList<CoarseList<lock_ttas_pause>>>(graph, profile, "win_coarse_ttas", 8); });
        label = "pareto par win_fine_ttas slack=" + std::to_string(slack);
        stage(label.c_str(), [&] { test_parallel<WindowedList<FineList<lock_ttas_pause>>>(graph, profile, "win_fine_ttas", 8); });
    }
    window_slack().store(-1);
    window_skip_empty().store(true); window_slack().store(1);
    stage("pareto par win_coarse_ttas skip_empty slack=1", [&] { test_parallel<WindowedList<CoarseList<lock_ttas_pause>>>(graph, profile, "win_coarse_ttas", 8); });
    stage("pareto par win_fine_ttas skip_empty slack=1", [&] { test_parallel<WindowedList<FineList<lock_ttas_pause>>>(graph, profile, "win_fine_ttas", 8); });
    window_skip_empty().store(false); window_slack().store(-1);
    if (argc > 3) {
        Graph stress_graph; stress_graph.load(argv[3], profile);
        std::printf("stress graph: %u nodes\n", stress_graph.num_nodes);
        stage("pareto stress coarse_ttas", [&] { test_stress<CoarseList<lock_ttas_pause>>(stress_graph, profile, "coarse_ttas", 16); });
        stage("pareto stress coarse_std_mutex", [&] { test_stress<CoarseList<std::mutex>>(stress_graph, profile, "coarse_std_mutex", 16); });
        window_divisor().store(1024); window_batch().store(8);
        stage("pareto stress win_coarse_ttas", [&] { test_stress<WindowedList<CoarseList<lock_ttas_pause>>>(stress_graph, profile, "win_coarse_ttas", 16); });
    }
    std::printf("%d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
