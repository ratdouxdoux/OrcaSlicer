// See SkyLanePacker.hpp for provenance and license.
#include "SkyLanePacker.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Slic3r::SkyLane {
namespace {
constexpr double EPS = 1e-9;

class Lanes
{
public:
    Lanes(const Parameters& params, double depth, Plan& result)
        : p(params), depth(depth), full_depth(depth), lane_depth(depth / p.lanes), heights(p.lanes, 0.), result(result)
    {}

    bool place(const Demand& e, size_t index, int active)
    {
        if (!have_base) {
            base_z    = e.z;
            have_base = true;
        }
        if (round_height > 0. && (active != 1 || (p.solid_base && !base_done && e.z > base_z + .0001)))
            close_round(index);
        if (p.solid_base && !base_done && e.z > base_z + .0001) {
            for (int c = 0; c < p.lanes; ++c)
                while (heights[c] < base_z - EPS)
                    if (!fill(index, c, std::min(base_z - heights[c], p.max_height), 1., true))
                        return false;
            base_done = true;
        }

        if (active == 1) {
            if (!merged) {
                const double top = *std::max_element(heights.begin(), heights.end());
                for (int c = 0; c < p.lanes; ++c)
                    while (heights[c] < top - EPS) {
                        const double h = std::min(top - heights[c], p.max_height);
                        if (!fill(index, c, h, p.fill_density))
                            return false;
                    }
                merged = true;
            }
            double       floor   = heights.front();
            const double reserve = std::min(p.merged_reserve, p.max_dip / 2.);
            if (round_height > 0. && e.z - (floor + round_height) > std::max(p.max_dip - p.max_height, 0.) + EPS)
                close_round(index);
            floor     = heights.front();
            auto cell = [&](double h) {
                result.passes.push_back(
                    {index, -1, 0, floor, h, shift, shift + depth, p.merged_density, false, true, true, shift, shift + depth});
                floor += h;
                std::fill(heights.begin(), heights.end(), floor);
            };
            if (round_height <= 0.)
                while (e.z - floor > p.max_dip + EPS)
                    cell(std::max(std::min(e.z - p.max_dip - floor, p.max_height), p.min_height));
            double remaining = e.volume;
            for (size_t guard = 0; remaining > EPS; ++guard) {
                if (guard > 10000)
                    return false;
                bool opened = false;
                if (round_height <= 0.) {
                    const double gap = e.z - floor;
                    if (gap < p.min_height - EPS)
                        return false;
                    round_height = std::min(gap, p.max_height);
                    round_y      = 0.;
                    opened       = true;
                }
                const double room = depth - round_y;
                const double need = remaining / (p.width * round_height * p.purge_density);
                if (!opened && room < std::min(need, 2. * p.line_width)) {
                    close_round(index);
                    floor = heights.front();
                    continue;
                }
                const double span = std::min(std::max(need, p.line_width), room);
                result.passes.push_back({index, int(index), 0, floor, round_height, shift + round_y, shift + round_y + span,
                                         p.purge_density * std::min(1., need / span), false, true, opened, shift, shift + depth});
                remaining -= p.width * span * round_height * p.purge_density;
                round_y += span;
                if (depth - round_y < p.line_width) {
                    close_round(index);
                    floor = heights.front();
                }
            }
            const double rise  = e.layer_height > 0. ? std::min(e.layer_height, p.max_height) : p.max_height;
            const double limit = std::min(reserve + p.max_height, std::max(p.max_dip - rise, reserve));
            if (round_height <= 0. && e.z - floor > limit + EPS)
                while (e.z - floor > reserve + p.min_height - EPS)
                    cell(std::min(e.z - reserve - floor, p.max_height));
            return true;
        }

        merged = false;
        for (int c = 0; c < p.lanes; ++c)
            while (e.z - heights[c] > p.max_dip + EPS) {
                const double h = std::max(std::min(e.z - p.max_dip - heights[c], p.max_height), p.min_height);
                if (!fill(index, c, h, p.fill_density))
                    return false;
            }
        double remaining = e.volume;
        for (size_t guard = 0; remaining > EPS; ++guard) {
            if (guard > 10000)
                return false;
            int c = -1;
            for (int j = 0; j < p.lanes; ++j)
                if (e.z - heights[j] >= p.min_height - EPS && (c < 0 || heights[j] < heights[c]))
                    c = j;
            if (c < 0)
                return false;
            const double h = std::min(
                {e.z - heights[c], p.max_height, std::max(remaining / (p.width * lane_depth * p.purge_density), p.min_height)});
            auto [y0, y1] = band(c, heights[c] + h);
            if (y1 - y0 < p.line_width)
                return false;
            const double density = std::min(p.purge_density, remaining / (p.width * (y1 - y0) * h));
            push(index, int(index), c, h, y0, y1, density);
            remaining -= p.width * (y1 - y0) * h * p.purge_density;
        }
        const double rise = e.layer_height > 0. ? std::min(e.layer_height, p.max_height) : p.max_height;
        const double goal = e.z - std::max(p.max_dip - rise, 0.);
        for (int c = 0; c < p.lanes; ++c)
            while (heights[c] < goal - EPS && heights[c] + p.min_height <= e.z + EPS) {
                const double h = std::min(std::max(std::min(goal - heights[c], p.max_height), p.min_height), e.z - heights[c]);
                if (!fill(index, c, h, p.fill_density))
                    return false;
            }
        return true;
    }

    bool resize(double next_depth, size_t owner)
    {
        if (std::abs(next_depth - depth) < EPS)
            return true;
        close_round(owner);
        if (*std::max_element(heights.begin(), heights.end()) - *std::min_element(heights.begin(), heights.end()) > EPS)
            return false;
        depth      = next_depth;
        lane_depth = depth / p.lanes;
        shift      = (full_depth - depth) / 2.;
        return true;
    }

private:
    std::pair<double, double> band(int c, double top) const
    {
        double       y0 = shift + c * lane_depth, y1 = shift + (c + 1) * lane_depth;
        const double slope = std::tan(p.nozzle_cone_angle * 3.14159265358979323846 / 180.);
        if (c > 0 && heights[c - 1] > top)
            y0 += p.tip_radius - p.line_width / 2. + (heights[c - 1] - top) * slope;
        if (c + 1 < p.lanes && heights[c + 1] > top)
            y1 -= p.tip_radius - p.line_width / 2. + (heights[c + 1] - top) * slope;
        return {y0, y1};
    }
    bool fill(size_t owner, int lane, double h, double density, bool solid = false)
    {
        const auto [y0, y1] = band(lane, heights[lane] + h);
        if (y1 - y0 < p.line_width)
            return false;
        push(owner, -1, lane, h, y0, y1, density, solid);
        return true;
    }
    void push(size_t owner, int demand, int lane, double h, double y0, double y1, double density, bool solid = false)
    {
        solid = solid || (p.solid_base && heights[lane] < base_z - EPS);
        result.passes.push_back({owner, demand, lane, heights[lane], h, y0, y1, density, solid, false, false, shift, shift + depth});
        heights[lane] += h;
    }
    void close_round(size_t owner)
    {
        if (round_height <= 0.)
            return;
        if (depth - round_y >= p.line_width)
            result.passes.push_back({owner, -1, 0, heights.front(), round_height, shift + round_y, shift + depth, p.merged_density,
                                     p.solid_base && heights.front() < base_z - EPS, true, false, shift, shift + depth});
        const double top = heights.front() + round_height;
        std::fill(heights.begin(), heights.end(), top);
        round_height = round_y = 0.;
    }
    const Parameters&   p;
    double              depth, full_depth, lane_depth, shift = 0.;
    std::vector<double> heights;
    Plan&               result;
    bool                have_base = false, base_done = false, merged = false;
    double              base_z = 0., round_height = 0., round_y = 0.;
};

void validate(const std::vector<Demand>& demands, const Parameters& p)
{
    for (double value : {p.width, p.max_dip, p.min_height, p.max_height, p.tip_radius, p.nozzle_cone_angle, p.line_width, p.fill_density,
                         p.merged_density, p.merged_reserve, p.purge_density})
        if (!std::isfinite(value))
            throw std::invalid_argument("SkyLane parameters must be finite");
    if (p.tip_radius < p.line_width / 2. || p.nozzle_cone_angle < 0. || p.nozzle_cone_angle >= 90. || p.fill_density <= 0. ||
        p.fill_density > 1. || p.merged_density <= 0. || p.merged_density > 1. || p.merged_reserve < 0.)
        throw std::invalid_argument("Invalid SkyLane nozzle geometry or support density");
    if (!std::isfinite(p.width) || p.width <= 4. * p.line_width || p.lanes < 2 || p.lanes > 6 || !std::isfinite(p.max_dip) ||
        p.max_dip < p.max_height || p.min_height <= 0. || p.max_height < p.min_height || p.line_width <= 0. || p.purge_density <= 0. ||
        p.purge_density > 1.)
        throw std::invalid_argument("Invalid SkyLane dimensions or layer heights");
    double last_z     = 0.;
    size_t last_layer = 0;
    for (const auto& e : demands) {
        if (!std::isfinite(e.z) || !std::isfinite(e.volume) || !std::isfinite(e.layer_height) || e.z < p.min_height || e.z < last_z - EPS ||
            e.volume < 0. || e.layer_height <= 0. || e.layer < last_layer)
            throw std::invalid_argument("SkyLane demands must have finite nonnegative volumes and increasing heights");
        last_z     = e.z;
        last_layer = e.layer;
    }
}
} // namespace

static Plan pack_steps(const std::vector<Demand>&                    demands,
                       double                                        depth,
                       const Parameters&                             p,
                       const std::vector<std::pair<size_t, double>>& steps)
{
    validate(demands, p);
    if (!std::isfinite(depth) || depth <= 0.)
        throw std::invalid_argument("Invalid SkyLane depth");
    if (demands.empty())
        return {true, p.lanes, depth, 0, {}, p.width};
    std::vector<size_t> layers;
    for (const auto& e : demands)
        if (layers.empty() || layers.back() != e.layer)
            layers.push_back(e.layer);
    std::vector<int> active(layers.size(), 1);
    // Each failure promotes at least one layer; thus at most N+1 attempts.
    for (size_t attempt = 0; attempt <= layers.size(); ++attempt) {
        Plan   result{false, p.lanes, depth, 0, {}, p.width};
        Lanes  lanes(p, depth, result);
        size_t li = 0, index = 0;
        for (; index < demands.size(); ++index) {
            while (li + 1 < layers.size() && layers[li] != demands[index].layer)
                ++li;
            double cell_depth = depth;
            for (const auto& step : steps)
                if (demands[index].layer >= step.first)
                    cell_depth = step.second;
            if ((!steps.empty() && cell_depth < depth && active[li] != 1) || !lanes.resize(cell_depth, index) ||
                !lanes.place(demands[index], index, active[li]))
                break;
        }
        if (index == demands.size()) {
            result.ok = true;
            return result;
        }
        result.failed_demand = index;
        // A tapered suffix must stay merged. Promoting it to split lanes can
        // never make this trial valid, so avoid replaying every earlier layer.
        if (!steps.empty() && demands[index].layer >= steps.front().first)
            return result;
        if (active[li] == 1) {
            active[li] = p.lanes;
            continue;
        }
        while (li > 0 && active[li - 1] != 1)
            --li;
        if (li == 0)
            return result;
        active[li - 1] = p.lanes;
    }
    return {};
}

Plan pack_scheduled(const std::vector<Demand>& demands, double depth, const Parameters& p) { return pack_steps(demands, depth, p, {}); }

Plan plan(const std::vector<Demand>& demands, const Parameters& params)
{
    validate(demands, params);
    if (demands.empty())
        return {true, 2, 0., 0, {}, params.width};
    Plan best;
    for (int count = 2; count <= 6; ++count) {
        Parameters p = params;
        p.lanes      = count;
        double lo = 1., hi = 400.;
        if (!pack_scheduled(demands, hi, p).ok)
            continue;
        for (int iteration = 0; iteration < 35; ++iteration) {
            const double mid = (lo + hi) * .5;
            if (pack_scheduled(demands, mid, p).ok)
                hi = mid;
            else
                lo = mid;
        }
        if (count > 2 && hi * 1.15 / count < 4.)
            continue;
        auto candidate = pack_scheduled(demands, hi * 1.15, p);
        if (candidate.ok && (!best.ok || candidate.depth < best.depth - .05))
            best = std::move(candidate);
    }
    return best;
}
Plan plan_square(const std::vector<Demand>& demands, const Parameters& params)
{
    validate(demands, params);
    if (demands.empty())
        return {true, 2, params.width, 0, {}, params.width};
    Plan best;
    for (int count = 2; count <= 6; ++count) {
        Parameters p = params;
        p.lanes      = count;
        double lo = 4. * p.line_width, hi = std::max(400., params.width);
        p.width = hi;
        if (!pack_scheduled(demands, hi, p).ok)
            continue;
        for (int iteration = 0; iteration < 35; ++iteration) {
            const double side = (lo + hi) * .5;
            p.width           = side;
            if (pack_scheduled(demands, side, p).ok)
                hi = side;
            else
                lo = side;
        }
        // Applying 15% to both axes would reserve 32% extra area.
        const double side = std::ceil(std::max(params.width, hi * std::sqrt(1.15)) / p.line_width) * p.line_width;
        if (count > 2 && side / count < 4.)
            continue;
        p.width        = side;
        auto candidate = pack_scheduled(demands, side, p);
        if (candidate.ok && (!best.ok || side < best.depth - .05))
            best = std::move(candidate);
    }
    return best;
}

Plan taper(const std::vector<Demand>& demands, const Parameters& params, const Plan& full)
{
    if (!full.ok || demands.size() < 2)
        return full;
    Parameters p = params;
    p.lanes      = full.lanes;
    std::vector<size_t> layers;
    for (const auto& d : demands)
        if ((layers.empty() || layers.back() != d.layer) && d.z > demands.front().z + .0001)
            layers.push_back(d.layer);
    std::vector<std::pair<size_t, double>> steps;
    Plan                                   result   = full;
    size_t                                 earliest = 0;
    for (double fraction : {.85, .70, .55, .40}) {
        const double depth = full.depth * fraction;
        if (depth < 4. * p.line_width)
            break;
        size_t lo = earliest, hi = layers.size();
        // A smaller footprint must support the entire suffix, not just its
        // first layer. Replaying also closes the previous round before stepping in.
        while (lo < hi) {
            size_t mid   = (lo + hi) / 2;
            auto   trial = steps;
            trial.emplace_back(layers[mid], depth);
            if (pack_steps(demands, full.depth, p, trial).ok)
                hi = mid;
            else
                lo = mid + 1;
        }
        if (lo == layers.size())
            break;
        steps.emplace_back(layers[lo], depth);
        auto candidate = pack_steps(demands, full.depth, p, steps);
        if (!candidate.ok)
            break;
        result   = std::move(candidate);
        earliest = lo;
    }
    return result;
}
} // namespace Slic3r::SkyLane
