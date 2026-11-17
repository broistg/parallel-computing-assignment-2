#pragma once
// Graph snapshot loader (CSR) + speed profile + time-dependent edge cost.
// See docs/SPEC_1_objectives.md.  All quantities are integers:
//   length in decimetres (dm), time in milliseconds (ms), speed multipliers in permille.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace astar {

constexpr double kEarthRadiusM = 6371008.8;  // must equal EARTH_RADIUS_M in tools/snapshot_io.py
constexpr int64_t kDayMs = 86400000;
constexpr int kRoadClasses = 4;
constexpr uint32_t kInfiniteCost = 0xFFFFFFFFu;  // "no path found yet" (costs are stored in 32 bits)

inline std::string read_file(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) throw std::runtime_error("cannot open " + path);
    std::string contents;
    char buffer[1 << 16];
    size_t bytes_read;
    while ((bytes_read = std::fread(buffer, 1, sizeof buffer, file)) > 0) contents.append(buffer, bytes_read);
    std::fclose(file);
    return contents;
}

// FNV-1a 64-bit hash that ignores '\r', so a CRLF checkout of the same file hashes identically.
// tools/oracle.py implements the same function (used to tie a golden file to its inputs).
inline uint64_t fnv1a(const std::string& text, uint64_t hash = 1469598103934665603ULL) {
    for (unsigned char ch : text) {
        if (ch == '\r') continue;
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::string to_hex16(uint64_t value) {
    char buffer[17];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

// Parses a CSV of integers (one header line) into a flat vector, `columns` values per row.
inline std::vector<int64_t> parse_int_csv(const std::string& text, int columns) {
    std::vector<int64_t> values;
    const char* cursor = text.c_str();
    const char* end = cursor + text.size();
    while (cursor < end && *cursor != '\n') ++cursor;  // skip header line
    while (cursor < end) {
        while (cursor < end && (*cursor == '\n' || *cursor == '\r')) ++cursor;
        if (cursor >= end) break;
        for (int column = 0; column < columns; ++column) {
            char* parsed_end;
            values.push_back(std::strtoll(cursor, &parsed_end, 10));
            if (parsed_end == cursor) throw std::runtime_error("bad csv field");
            cursor = parsed_end;
            if (column + 1 < columns) {
                if (*cursor != ',') throw std::runtime_error("bad csv separator");
                ++cursor;
            }
        }
    }
    return values;
}

// Time-of-day speed multipliers per road class.  The number of buckets is read from the file
// (rows / 4), so changing the resolution (96 x 15 min, 288 x 5 min, ...) needs no code change.
// multiplier 1000 = free flow, larger = slower; values are linearly interpolated between bucket starts.
struct SpeedProfile {
    int num_buckets = 0;
    int64_t bucket_ms = 0;
    std::vector<int32_t> multiplier_permille;  // index: road_class * num_buckets + bucket
    int32_t max_drop_permille = 0;             // steepest decrease between consecutive buckets
    uint64_t content_hash = 0;

    int32_t multiplier(int road_class, int bucket) const { return multiplier_permille[road_class * num_buckets + bucket]; }

    void load(const std::string& path) {
        std::string text = read_file(path);
        content_hash = fnv1a(text);
        std::vector<int64_t> rows = parse_int_csv(text, 3);  // road_class, bucket, multiplier
        if (rows.empty() || (rows.size() / 3) % kRoadClasses != 0) throw std::runtime_error("profile.csv: bad row count");
        num_buckets = int(rows.size() / 3 / kRoadClasses);
        if (kDayMs % num_buckets != 0) throw std::runtime_error("profile.csv: buckets must divide 24 h evenly");
        bucket_ms = kDayMs / num_buckets;
        multiplier_permille.assign(size_t(kRoadClasses) * num_buckets, 0);
        for (size_t i = 0; i < rows.size(); i += 3) {
            int64_t road_class = rows[i], bucket = rows[i + 1], value = rows[i + 2];
            if (road_class < 0 || road_class >= kRoadClasses || bucket < 0 || bucket >= num_buckets || value < 1000)
                throw std::runtime_error("profile.csv value (multiplier must be >= 1000)");
            multiplier_permille[road_class * num_buckets + bucket] = int32_t(value);
        }
        max_drop_permille = 0;
        for (int road_class = 0; road_class < kRoadClasses; ++road_class)
            for (int bucket = 0; bucket < num_buckets; ++bucket)
                max_drop_permille = std::max(max_drop_permille, multiplier(road_class, bucket) - multiplier(road_class, (bucket + 1) % num_buckets));
    }
};

struct Graph {
    uint32_t num_nodes = 0, num_edges = 0;
    // CSR: the outgoing edges of node v are the indices [first_edge[v], first_edge[v + 1]).
    std::vector<uint32_t> first_edge, edge_target, edge_length_dm, edge_free_flow_ms;
    std::vector<uint8_t> edge_road_class;
    std::vector<double> lat_rad, lon_rad, cos_lat;
    uint32_t max_speed_kmh = 0;
    uint64_t content_hash = 0;  // over nodes.csv + edges.csv (platform independent)
    std::string snapshot_sha256;

    void load(const std::string& dir, const SpeedProfile& profile) {
        std::string nodes_text = read_file(dir + "/nodes.csv"), edges_text = read_file(dir + "/edges.csv");
        content_hash = fnv1a(edges_text, fnv1a(nodes_text));
        {
            std::string meta = read_file(dir + "/meta.json");
            size_t key = meta.find("\"sha256\"");
            if (key != std::string::npos) {
                size_t start = meta.find('"', meta.find(':', key) + 1) + 1;
                snapshot_sha256 = meta.substr(start, meta.find('"', start) - start);
            }
        }
        std::vector<int64_t> node_rows = parse_int_csv(nodes_text, 4);  // idx, osmid, lat_e7, lon_e7
        num_nodes = uint32_t(node_rows.size() / 4);
        lat_rad.resize(num_nodes);
        lon_rad.resize(num_nodes);
        cos_lat.resize(num_nodes);
        const double degrees_to_radians = M_PI / 180.0;
        for (uint32_t node = 0; node < num_nodes; ++node) {
            if (node_rows[4 * node] != int64_t(node)) throw std::runtime_error("nodes.csv: idx not 0..N-1");
            lat_rad[node] = double(node_rows[4 * node + 2]) / 1e7 * degrees_to_radians;
            lon_rad[node] = double(node_rows[4 * node + 3]) / 1e7 * degrees_to_radians;
            cos_lat[node] = std::cos(lat_rad[node]);
        }
        std::vector<int64_t> edge_rows = parse_int_csv(edges_text, 5);  // u, v, length_dm, speed_kmh, class
        num_edges = uint32_t(edge_rows.size() / 5);
        first_edge.assign(num_nodes + 1, 0);
        edge_target.resize(num_edges);
        edge_length_dm.resize(num_edges);
        edge_free_flow_ms.resize(num_edges);
        edge_road_class.resize(num_edges);
        for (uint32_t row = 0; row < num_edges; ++row) first_edge[edge_rows[5 * row] + 1]++;
        for (uint32_t node = 0; node < num_nodes; ++node) first_edge[node + 1] += first_edge[node];
        std::vector<uint32_t> next_slot(first_edge.begin(), first_edge.end() - 1);
        max_speed_kmh = 0;
        int64_t longest_free_flow_ms = 0;
        for (uint32_t row = 0; row < num_edges; ++row) {
            uint32_t from = uint32_t(edge_rows[5 * row]), to = uint32_t(edge_rows[5 * row + 1]);
            int64_t length_dm = edge_rows[5 * row + 2], speed_kmh = edge_rows[5 * row + 3], road_class = edge_rows[5 * row + 4];
            if (from >= num_nodes || to >= num_nodes || length_dm <= 0 || speed_kmh <= 0 || road_class < 0 || road_class >= kRoadClasses)
                throw std::runtime_error("edges.csv: bad row");
            if (length_dm < int64_t(std::floor(great_circle_m(from, to) * 10.0)))
                throw std::runtime_error("edges.csv: length shorter than great-circle distance");
            uint32_t slot = next_slot[from]++;
            edge_target[slot] = to;
            edge_length_dm[slot] = uint32_t(length_dm);
            edge_free_flow_ms[slot] = uint32_t((length_dm * 360 + speed_kmh - 1) / speed_kmh);  // ceil(dm * 0.36 / kmh * 1000 / 1000)
            edge_road_class[slot] = uint8_t(road_class);
            max_speed_kmh = std::max<uint32_t>(max_speed_kmh, uint32_t(speed_kmh));
            longest_free_flow_ms = std::max<int64_t>(longest_free_flow_ms, edge_free_flow_ms[slot]);
        }
        // FIFO guard: the slope of cost(t) must stay below 1/2 in magnitude for every edge.
        if (longest_free_flow_ms * profile.max_drop_permille >= profile.bucket_ms * 1000 / 2)
            throw std::runtime_error("FIFO guard: an edge is too long for the profile's steepest drop");
    }

    // Great-circle (haversine) distance in metres between two nodes.
    double great_circle_m(uint32_t a, uint32_t b) const {
        double dlat = lat_rad[b] - lat_rad[a], dlon = lon_rad[b] - lon_rad[a];
        double sin_half_dlat = std::sin(dlat / 2), sin_half_dlon = std::sin(dlon / 2);
        double h = sin_half_dlat * sin_half_dlat + cos_lat[a] * cos_lat[b] * sin_half_dlon * sin_half_dlon;
        return 2.0 * kEarthRadiusM * std::asin(std::min(1.0, std::sqrt(h)));
    }
};

// Time-dependent travel time (ms) of an edge entered at absolute time `entry_ms` (ms since midnight of day 0).
inline int64_t time_dependent_cost(const SpeedProfile& profile, int road_class, uint32_t free_flow_ms, int64_t entry_ms) {
    int64_t time_of_day = entry_ms % kDayMs;
    int64_t bucket = time_of_day / profile.bucket_ms;
    int64_t into_bucket = time_of_day - bucket * profile.bucket_ms;
    int64_t multiplier_here = profile.multiplier(road_class, int(bucket));
    int64_t multiplier_next = profile.multiplier(road_class, int((bucket + 1) % profile.num_buckets));
    int64_t interpolated = multiplier_here * profile.bucket_ms + (multiplier_next - multiplier_here) * into_bucket;  // permille * ms
    int64_t denominator = profile.bucket_ms * 1000;
    return (int64_t(free_flow_ms) * interpolated + denominator - 1) / denominator;  // ceil
}

}  // namespace astar
