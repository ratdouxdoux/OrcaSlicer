#include "SeamAlignmentSearch.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <list>
#include <stdexcept>

namespace Slic3r {
namespace {
thread_local SeamAlignmentSearch* current_search = nullptr;
using Clock                                      = std::chrono::steady_clock;
using SeamNeighborQuery                          = std::array<float, 4>;

bool cacheable(const SeamNeighborQuery& query)
{
    return query[3] > 0 && std::isfinite(query[3] * query[3]) &&
           std::all_of(query.begin(), query.end(), [](float value) { return std::isfinite(value); });
}
double   elapsed(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
uint32_t bits(float f)
{
    uint32_t value;
    std::memcpy(&value, &f, sizeof(value));
    return value;
}
struct Key
{
    size_t                  layer;
    std::array<uint32_t, 4> query;
    bool                    operator==(const Key& rhs) const { return layer == rhs.layer && query == rhs.query; }
};
struct Hash
{
    size_t operator()(const Key& key) const
    {
        size_t h = key.layer;
        for (auto v : key.query)
            h ^= size_t(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
Key key_for(size_t layer, const SeamNeighborQuery& q) { return {layer, {bits(q[0]), bits(q[1]), bits(q[2]), bits(q[3])}}; }
} // namespace
struct SeamAlignmentSearch::Impl
{
    const std::vector<PrintObjectSeamData::LayerSeams>& layers;
    struct Entry
    {
        std::vector<size_t>      indices;
        std::list<Key>::iterator order;
    };
    std::unordered_map<Key, Entry, Hash> cache;
    std::list<Key>                       order;
    std::vector<size_t>                  scratch;
    bool                                 detailed_timing = false;
    std::string                          mode;
    size_t                               bytes = 0, peak_bytes = 0;
    uint64_t                             calls = 0, hits = 0, cpu_queries = 0, evictions = 0;
    uint64_t          ineligible = 0;
    double                               cpu_ms = 0, lookup_ms = 0;
    Clock::time_point start = Clock::now();
    explicit Impl(const std::vector<PrintObjectSeamData::LayerSeams>& input) : layers(input)
    {
        const char* value = std::getenv("ORCA_SEAM_ALIGNMENT");
        mode              = value ? value : "cache";
        if (mode != "cpu" && mode != "profile" && mode != "cache")
            throw std::runtime_error("ORCA_SEAM_ALIGNMENT must be cpu, profile, or cache");
        detailed_timing = mode == "profile";
    }
    std::vector<size_t> reference(size_t layer, const SeamNeighborQuery& q)
    {
        auto t      = detailed_timing ? Clock::now() : Clock::time_point{};
        auto result = find_nearby_points(*layers[layer].points_tree, Vec3f(q[0], q[1], q[2]), q[3]);
        if (detailed_timing)
            cpu_ms += elapsed(t);
        ++cpu_queries;
        return result;
    }
    void store(const Key& key, std::vector<size_t> indices)
    {
        if (cache.find(key) != cache.end())
            return;
        const size_t     needed = 192 + indices.capacity() * sizeof(size_t);
        constexpr size_t LIMIT  = 64u * 1024 * 1024;
        if (needed > LIMIT)
            return;
        while (!order.empty() && (bytes + needed > LIMIT || cache.size() >= 131072)) {
            auto it = cache.find(order.front());
            bytes -= 192 + it->second.indices.capacity() * sizeof(size_t);
            cache.erase(it);
            order.pop_front();
            ++evictions;
        }
        order.push_back(key);
        cache.emplace(key, Entry{std::move(indices), std::prev(order.end())});
        bytes += needed;
        peak_bytes = std::max(peak_bytes, bytes);
    }
};
SeamAlignmentSearch::SeamAlignmentSearch(const std::vector<PrintObjectSeamData::LayerSeams>& layers)
    : m_impl(std::make_unique<Impl>(layers)), m_previous(current_search)
{
    if (m_impl->mode != "cpu")
        current_search = this;
}
SeamAlignmentSearch* SeamAlignmentSearch::active() { return current_search; }
const std::vector<size_t>& SeamAlignmentSearch::neighbors(size_t layer, const Vec3f& center, float radius)
{
    auto& p = *m_impl;
    const auto started = p.detailed_timing ? Clock::now() : Clock::time_point{};
    auto       finish  = [&](const std::vector<size_t>& result) -> const std::vector<size_t>& {
        if (p.detailed_timing)
            p.lookup_ms += elapsed(started);
        return result;
    };
    ++p.calls;
    const SeamNeighborQuery query{center.x(), center.y(), center.z(), radius};
    const auto              key = key_for(layer, query);
    if (p.layers[layer].points.empty()) {
        p.scratch.clear();
        return finish(p.scratch);
    }
    // Degenerate searches retain the original CPU semantics.
    if (!cacheable(query)) {
        ++p.ineligible;
        p.scratch = p.reference(layer, query);
        return finish(p.scratch);
    }
    auto it = p.cache.find(key);
    if (it != p.cache.end()) {
        ++p.hits;
        p.order.splice(p.order.end(), p.order, it->second.order);
        if (p.mode == "profile") {
            const auto expected = p.reference(layer, query);
            if (it->second.indices != expected)
                throw std::runtime_error("Cached seam neighborhood mismatch");
        }
        return finish(it->second.indices);
    }
    p.scratch = p.reference(layer, query);
    p.store(key, p.scratch);
    return finish(p.scratch);
}
SeamAlignmentSearch::~SeamAlignmentSearch()
{
    current_search = m_previous;
    try {
        const auto& p = *m_impl;
        // CPU mode only times the whole scope; individual lookups stay uninstrumented.
        nlohmann::json report = {{"mode", p.mode},
                                 {"calls", p.calls},
                                 {"hits", p.hits},
                                 {"ineligible", p.ineligible},
                                 {"cpu_queries", p.cpu_queries},
                                 {"evictions", p.evictions},
                                 {"peak_cache_bytes", p.peak_bytes},
                                 {"detailed_timing", p.detailed_timing},
                                 {"cpu_ms", p.cpu_ms},
                                 {"lookup_ms", p.lookup_ms},
                                 {"alignment_ms", elapsed(p.start)}};
        BOOST_LOG_TRIVIAL(info) << "Seam alignment compute " << report.dump();
        if (const char* path = std::getenv("ORCA_SEAM_ALIGNMENT_REPORT"); path && *path) {
            std::ofstream output(std::filesystem::u8path(path), std::ios::app);
            output << report.dump() << '\n';
        }
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "Failed to report seam alignment counters";
    }
}
} // namespace Slic3r
