// Self-contained test program (no framework).  Usage: test_astar <snapshot_dir> <profile.csv>
// Layers (docs/SPEC_3_reference.md): data invariants -> list tests -> search vs an independent
// in-file Dijkstra -> parallel stress with many repetitions.  Cross-language checks against the
// Python oracle are done by tests/run_tests.sh.
#include <cstdio>
#include <random>
#include <set>

#include "astar/search.h"
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

// Independent reference: textbook Dijkstra with the same integer cost function.
static int64_t ref_dijkstra(const Problem& problem) {
    const Graph& graph = *problem.graph;
    std::vector<int64_t> d(graph.num_nodes, INT64_MAX);
    using QueueEntry = std::pair<int64_t, uint32_t>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> pq;
    d[problem.source] = 0;
    pq.push({0, problem.source});
    while (!pq.empty()) {
        auto [dv, v] = pq.top();
        pq.pop();
        if (dv > d[v]) continue;
        if (v == problem.target) return dv;
        for (uint32_t e = graph.first_edge[v]; e < graph.first_edge[v + 1]; ++e) {
            int64_t nd = dv + problem.edge_cost(e, dv);
            if (nd < d[graph.edge_target[e]]) { d[graph.edge_target[e]] = nd; pq.push({nd, graph.edge_target[e]}); }
        }
    }
    return -1;
}

// The CAS-min primitive under heavy contention: final value must be the global minimum and the
// pred must belong to a thread that offered that minimum.
// Workload sizes.  Tuned so that a plain-store mutant of try_lower_cost is caught reliably on 2 cores.
// (Same sizes under ThreadSanitizer: it only costs a few seconds once the start-flag wait yields.)
constexpr int kHammerRounds = 3000, kSearchQueries = 40, kSearchRepeats = 3, kListOpsPerThread = 5000, kSequentialListOps = 20000;

static void test_try_lower_cost() {
    for (int num_threads : {2, 4, 8, 16}) {
        for (int round = 0; round < kHammerRounds; ++round) {
            std::atomic<uint64_t> word{pack_cost_and_pred(kInfiniteCost, 0)};
            std::vector<uint32_t> best(num_threads, kInfiniteCost);
            std::vector<std::thread> th;
            std::atomic<int> go{0};
            for (int t = 0; t < num_threads; ++t)
                th.emplace_back([&, t] {
                    std::mt19937 rng(round * 31 + t);
                    uint64_t cf = 0;
                    while (!go.load()) std::this_thread::yield();  // yield: pure spinning starves the starter when threads > cores
                    for (int i = 0; i < 400; ++i) {
                        uint32_t v = 1000 + rng() % 100000;
                        best[t] = std::min(best[t], v);
                        try_lower_cost(word, v, uint32_t(t), cf);
                    }
                });
            go.store(1);
            for (auto& x : th) x.join();
            uint32_t gmin = *std::min_element(best.begin(), best.end());
            uint64_t w = word.load();
            CHECK(cost_of(w) == gmin, "try_lower_cost lost update: got %u want %u (num_threads=%d)", cost_of(w), gmin, num_threads);
            CHECK(pred_of(w) < uint32_t(num_threads) && best[pred_of(w)] == gmin, "try_lower_cost pred mismatch");
        }
    }
}

static void test_data(const Graph& graph, const SpeedProfile& profile) {
    std::mt19937_64 rng(11);
    // FIFO: arrival time must be non-decreasing in departure time for every edge.
    for (int i = 0; i < 200000; ++i) {
        uint32_t e = rng() % graph.num_edges;
        int64_t t = rng() % (2 * kDayMs);
        int64_t a0 = t + time_dependent_cost(profile, graph.edge_road_class[e], graph.edge_free_flow_ms[e], t);
        int64_t a1 = t + 1 + time_dependent_cost(profile, graph.edge_road_class[e], graph.edge_free_flow_ms[e], t + 1);
        CHECK(a1 >= a0, "FIFO violated edge %u t=%lld", e, (long long)t);
        CHECK(time_dependent_cost(profile, graph.edge_road_class[e], graph.edge_free_flow_ms[e], t) >= int64_t(graph.edge_free_flow_ms[e]), "cost below free flow");
    }
    // Heuristic consistency (exact mode), both objectives.
    for (Objective obj : {Objective::Time, Objective::Distance}) {
        for (int k = 0; k < 20; ++k) {
            uint32_t tgt = rng() % graph.num_nodes;
            Heuristic heuristic(graph, tgt, obj, HeuristicConfig{});
            for (uint32_t v = 0; v < graph.num_nodes; v += 3) {
                for (uint32_t e = graph.first_edge[v]; e < graph.first_edge[v + 1]; ++e) {
                    int64_t lower = obj == Objective::Distance ? graph.edge_length_dm[e] : graph.edge_free_flow_ms[e];
                    CHECK(heuristic(v, 0) <= lower + heuristic(graph.edge_target[e], 0), "h inconsistent");
                }
            }
        }
    }
}

template <class L>
static void test_list(const char* name, int max_threads = 16) {  // see test_stress for why FIFO locks get a cap
    // sequential semantics against std::set
    {
        L l;
        std::set<std::tuple<int64_t, uint32_t, uint32_t>> ref;
        std::mt19937 rng(5);
        for (int i = 0; i < kSequentialListOps; ++i) {
            OpenItem x{int64_t(rng() % 200), uint32_t(rng() % 20), uint32_t(rng() % 3)};
            auto key = std::make_tuple(x.f_value, x.node, x.g_cost);
            int op = rng() % 4;
            if (op == 0 || op == 1) CHECK(l.add(x) == ref.insert(key).second, "%s add", name);
            else if (op == 2) CHECK(l.remove(x) == (ref.erase(key) > 0), "%s remove", name);
            else if (op == 3) {
                OpenItem y;
                bool got = l.pop_min(y);
                CHECK(got == !ref.empty(), "%s pop_min presence", name);
                if (got) {
                    CHECK(std::make_tuple(y.f_value, y.node, y.g_cost) == *ref.begin(), "%s pop_min order", name);
                    ref.erase(ref.begin());
                }
            }
            CHECK(l.contains(x) == (ref.count(key) > 0), "%s contains", name);
        }
    }
    // concurrent: every inserted item is popped exactly once, none lost or duplicated
    for (int num_threads : {2, 4, 8}) {
        if (num_threads > max_threads) continue;
        L l;
        const int per = kListOpsPerThread;
        std::atomic<int> popped{0};
        std::vector<std::atomic<uint8_t>> seen(size_t(num_threads) * per);
        for (auto& s : seen) s.store(0);
        std::vector<std::thread> th;
        for (int t = 0; t < num_threads; ++t)
            th.emplace_back([&, t] {
                std::mt19937 rng(100 + t);
                for (int i = 0; i < per; ++i) {
                    uint32_t id = uint32_t(t * per + i);
                    l.add({int64_t(rng() % 1000), id, 0});
                    OpenItem y;
                    if (l.pop_min(y)) {
                        CHECK(seen[y.node].exchange(1) == 0, "%s duplicate pop", name);
                        popped++;
                    }
                }
            });
        for (auto& x : th) x.join();
        OpenItem y;
        int64_t last = -1;
        while (l.pop_min(y)) {
            CHECK(y.f_value >= last, "%s final drain not sorted", name);
            last = y.f_value;
            CHECK(seen[y.node].exchange(1) == 0, "%s duplicate in drain", name);
            popped++;
        }
        CHECK(popped == num_threads * per, "%s lost items: %d of %d (num_threads=%d)", name, popped.load(), num_threads * per, num_threads);
    }
}

template <class L>
static void test_search(const Graph& graph, const SpeedProfile& profile, const char* name, int max_threads = 16) {
    std::mt19937_64 rng(2024);
    const int64_t deps[] = {3 * 3600000LL, 8 * 3600000LL, 17 * 3600000LL + 1800000};
    SeqWorkspace seq_workspace;
    for (Objective obj : {Objective::Time, Objective::Distance}) {
        for (int num_threads : {1, 2, 4, 8}) {
            if (num_threads > max_threads) continue;
            ThreadPool pool(num_threads, {});
            ParWorkspace par_workspace;
            for (int q = 0; q < kSearchQueries; ++q) {
                uint32_t s = rng() % graph.num_nodes, d = (q == 0) ? s : rng() % graph.num_nodes;
                Problem problem{&graph, &profile, obj, s, d, deps[q % 3], HeuristicConfig{}};
                int64_t ref = ref_dijkstra(problem);
                SearchResult seq_result = seq_astar(problem, seq_workspace);
                CHECK(seq_result.found && seq_result.reported_cost == ref && seq_result.checked_cost == ref, "seq %s q=%d ref=%lld got=%lld", name, q,
                      (long long)ref, (long long)seq_result.reported_cost);
                if (num_threads == 1) {  // same sorted list, no lock, one thread: must expand exactly like the heap version
                    SearchResult list_result = seq_astar_list<CoarseList<NullLock>>(problem, seq_workspace);
                    CHECK(list_result.found && list_result.reported_cost == ref && list_result.checked_cost == ref &&
                          list_result.counters.expansions == seq_result.counters.expansions,
                          "seqlist %s q=%d ref=%lld got=%lld exp=%llu vs %llu", name, q, (long long)ref, (long long)list_result.reported_cost,
                          (unsigned long long)list_result.counters.expansions, (unsigned long long)seq_result.counters.expansions);
                }
                for (int rep = 0; rep < kSearchRepeats; ++rep) {  // repeat: interleavings differ run to run
                    SearchResult par_result = par_astar<L>(problem, pool, par_workspace);
                    CHECK(par_result.found && par_result.reported_cost == ref && par_result.checked_cost == ref,
                          "par %s threads=%d obj=%d q=%d ref=%lld got=%lld checked=%lld", name, num_threads, int(obj), q, (long long)ref,
                          (long long)par_result.reported_cost, (long long)par_result.checked_cost);
                    CHECK(par_result.list_stats.pops + par_result.list_stats.pop_empty > 0 && par_result.list_stats.adds >= par_result.list_stats.pops, "list op counters");
                }
            }
        }
    }
    // Relaxed mode (noisy / inflated h): result must still be a valid path no cheaper than optimal.
    ThreadPool pool(std::min(4, max_threads), {});
    ParWorkspace par_workspace;
    HeuristicConfig noisy; noisy.noise_permille = 250; noisy.inflation_percent = 120;
    for (int q = 0; q < kSearchQueries; ++q) {
        uint32_t s = rng() % graph.num_nodes, d = rng() % graph.num_nodes;
        Problem problem{&graph, &profile, Objective::Time, s, d, deps[q % 3], noisy};
        Problem exact_problem = problem; exact_problem.heuristic_config = HeuristicConfig{};
        int64_t ref = ref_dijkstra(exact_problem);
        SearchResult par_result = par_astar<L>(problem, pool, par_workspace);
        CHECK(par_result.found && par_result.checked_cost >= ref, "relaxed %s q=%d ref=%lld checked=%lld", name, q, (long long)ref, (long long)par_result.checked_cost);
        SearchResult seq_result = seq_astar(problem, seq_workspace);
        CHECK(seq_result.found && seq_result.checked_cost >= ref, "relaxed seq q=%d", q);
    }
}

// Contention stress: long queries on a larger graph, oversubscribed threads, many repetitions.
// Designed to expose lost updates (non-atomic min on g) and premature termination.
// max_threads: FIFO locks (ticket) collapse when threads outnumber cores -- the next ticket holder is
// descheduled and every waiter spins in vain (a "lock convoy": 188 s vs 11 s for TTAS on 2 cores).
// That is a performance effect, not a bug, so the stress test keeps FIFO locks at or below the core count.
template <class L>
static void test_stress(const Graph& graph, const SpeedProfile& profile, const char* name, int max_threads) {
    std::mt19937_64 rng(77);
    for (int num_threads : {2, 4, 8, 16}) {
        if (num_threads > max_threads) continue;
        ThreadPool pool(num_threads, {});
        ParWorkspace par_workspace;
        for (int q = 0; q < 12; ++q) {
            uint32_t s = rng() % graph.num_nodes, d = rng() % graph.num_nodes;
            Problem problem{&graph, &profile, Objective::Time, s, d, 8 * 3600000LL, HeuristicConfig{}};
            int64_t ref = ref_dijkstra(problem);
            for (int rep = 0; rep < 8; ++rep) {
                SearchResult par_result = par_astar<L>(problem, pool, par_workspace);
                CHECK(par_result.found && par_result.reported_cost == ref && par_result.checked_cost == ref, "stress %s num_threads=%d q=%d rep=%d ref=%lld got=%lld", name, num_threads,
                      q, rep, (long long)ref, (long long)par_result.reported_cost);
            }
        }
    }
}

// Runs one test stage and prints how long it took (flushed, so a slow or stuck stage is visible while it runs).
template <class Fn>
static void stage(const char* name, Fn fn) {
    auto start = std::chrono::steady_clock::now();
    std::printf("[stage] %-26s ...", name);
    std::fflush(stdout);
    fn();
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf(" %.1f s (failures so far: %d)\n", seconds, g_fail.load());
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: %s snapshot profile\n", argv[0]); return 2; }
    SpeedProfile profile; profile.load(argv[2]);
    Graph graph; graph.load(argv[1], profile);
    std::printf("graph: %u nodes, %u edges, vmax=%u km/h\n", graph.num_nodes, graph.num_edges, graph.max_speed_kmh);
    const int cores = int(std::max(2u, std::thread::hardware_concurrency()));
    stage("try_lower_cost hammer", [&] { test_try_lower_cost(); });
    stage("data invariants", [&] { test_data(graph, profile); });
    stage("list coarse_ttas", [&] { test_list<CoarseList<lock_ttas_pause>>("coarse_ttas"); });
    stage("list coarse_ticket (<=cores)", [&] { test_list<CoarseList<lock_ticket>>("coarse_ticket", cores); });
    stage("list coarse_std_mutex", [&] { test_list<CoarseList<std::mutex>>("coarse_std_mutex"); });
    stage("search coarse_ttas", [&] { test_search<CoarseList<lock_ttas_pause>>(graph, profile, "coarse_ttas"); });
    stage("search coarse_ticket (<=cores)", [&] { test_search<CoarseList<lock_ticket>>(graph, profile, "coarse_ticket", cores); });
    stage("search coarse_std_mutex", [&] { test_search<CoarseList<std::mutex>>(graph, profile, "coarse_std_mutex"); });
    if (argc > 3) {
        Graph stress_graph; stress_graph.load(argv[3], profile);
        std::printf("stress graph: %u nodes\n", stress_graph.num_nodes);
        stage("stress coarse_ttas", [&] { test_stress<CoarseList<lock_ttas_pause>>(stress_graph, profile, "coarse_ttas", 16); });
        stage("stress coarse_std_mutex", [&] { test_stress<CoarseList<std::mutex>>(stress_graph, profile, "coarse_std_mutex", 16); });
        stage("stress coarse_ticket (<=cores)", [&] { test_stress<CoarseList<lock_ticket>>(stress_graph, profile, "coarse_ticket", cores); });
    }
    std::printf("%d checks, %d failures\n", g_checks.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
