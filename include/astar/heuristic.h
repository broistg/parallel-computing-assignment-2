#pragma once
// Heuristic h(v, t): estimate of the remaining cost from v to the target, possibly dynamic
// (depends on the arrival-time bucket) and uncertain (noisy / inflated).
//   noise_permille = 0, inflation_percent = 100  -> admissible and consistent ("exact mode")
//   noise_permille > 0       deterministic noise in [-noise, +noise] permille per (node, 15-min bucket, seed)
//   inflation_percent > 100  weighted A* inflation
// With noise or inflation active, h may overestimate: the search stays correct (nodes are reopened)
// but only "relaxed" (quality is measured against the golden file).  See docs/SPEC_1_objectives.md.

#include "graph.h"

namespace astar {

enum class Objective { Time, Distance };

constexpr int64_t kNoiseBucketMs = 900000;  // noise changes every 15 min, independent of the speed profile

struct HeuristicConfig {
    int noise_permille = 0;
    int inflation_percent = 100;
    uint64_t noise_seed = 1;
    bool is_exact() const { return noise_permille == 0 && inflation_percent <= 100; }
};

inline uint64_t mix_bits(uint64_t x) {  // 64-bit finalizer used as a cheap deterministic hash
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

class Heuristic {
public:
    Heuristic(const Graph& graph, uint32_t target, Objective objective, HeuristicConfig config)
        : graph_(&graph), target_(target), objective_(objective), config_(config) {}

    // arrival_ms = absolute arrival time at `node` in ms (ignored for Objective::Distance)
    int64_t operator()(uint32_t node, int64_t arrival_ms) const {
        if (node == target_) return 0;
        int64_t estimate = straight_line_dm(node);
        if (objective_ == Objective::Time) estimate = estimate * 360 / int64_t(graph_->max_speed_kmh);  // ms at top speed, free flow
        if (config_.is_exact()) return estimate;
        estimate = estimate * config_.inflation_percent / 100;
        if (config_.noise_permille > 0) {
            uint64_t bucket = objective_ == Objective::Time ? uint64_t((arrival_ms % kDayMs) / kNoiseBucketMs) : 0;
            uint64_t hash = mix_bits((uint64_t(node) << 20) ^ (bucket << 48) ^ config_.noise_seed);
            int64_t noise = int64_t(hash % uint64_t(2 * config_.noise_permille + 1)) - config_.noise_permille;
            estimate = estimate * (1000 + noise) / 1000;
        }
        return estimate < 0 ? 0 : estimate;
    }

private:
    // floor of the great-circle distance in dm, with a small guard against rounding up
    int64_t straight_line_dm(uint32_t node) const { return int64_t(graph_->great_circle_m(node, target_) * 10.0 * (1.0 - 1e-9)); }

    const Graph* graph_;
    uint32_t target_;
    Objective objective_;
    HeuristicConfig config_;
};

}  // namespace astar
