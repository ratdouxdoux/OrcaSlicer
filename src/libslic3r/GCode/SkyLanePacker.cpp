// SkyLane packer, see SkyLanePacker.hpp.
// Adapted from SKYLANE_PACK_RADU_2, standalone SkyLane (AGPL).
// Placement is INCREMENTAL (Lanes::place, one demand at a time) so the very same
// code sizes the tower offline (pack) and places every TCR at emission time (SkyLaneTower.cpp),
// where the real emitted sequence (repeats, unemitted slots) is what lands on the lanes.
#include "SkyLanePacker.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace Slic3r { namespace SkyLane {

static const Params& checked_parameters(const Params& p, double depth)
{
    for (double value : {depth, p.width, p.max_pit, p.hmin, p.hmax, p.r_tip, p.cone_deg, p.line_w, p.fill_density, p.merged_density,
                         p.purge_density, p.merged_reserve})
        if (!std::isfinite(value))
            throw std::invalid_argument("SkyLane parameters must be finite");
    if (!p.block_lane_ends.empty() &&
        (p.block_lane_ends.front() <= 0 || p.block_lane_ends.back() != p.lanes || p.block_last_z.size() != p.block_lane_ends.size() ||
         std::adjacent_find(p.block_lane_ends.begin(), p.block_lane_ends.end(), std::greater_equal<int>()) != p.block_lane_ends.end()))
        throw std::invalid_argument("Invalid SkyLane material block boundaries");
    if (depth <= 0. || p.width <= 4. * p.line_w || p.lanes < 2 || (p.lanes > 6 && p.block_lane_ends.empty()) || p.hmin <= 0. ||
        p.hmax < p.hmin || p.max_pit < p.hmax || p.line_w <= 0. || p.r_tip < p.line_w / 2. || p.cone_deg < 0. || p.cone_deg >= 90. ||
        p.fill_density <= 0. || p.fill_density > 1. || p.merged_density <= 0. || p.merged_density > 1. || p.purge_density <= 0. ||
        p.purge_density > 1.)
        throw std::invalid_argument("Invalid SkyLane dimensions, density or nozzle geometry");
    return p;
}

Lanes::Lanes(const Params& p, double depth)
    : m_p(checked_parameters(p, depth))
    , m_depth(depth)
    , m_d(depth / p.lanes)
    , m_tg(std::tan(p.cone_deg * M_PI / 180.0))
    , m_H(size_t(p.lanes), 0.)
{
    if (!p.block_rounds)
        return;
    Params cell       = p;
    cell.block_rounds = false;
    cell.block_lane_ends.clear();
    cell.block_last_z.clear();
    cell.tool_block.clear();
    cell.lanes = 2;
    // Each cell can have an open round while its neighbour is higher. Leave
    // the same tip/cone clearance as band() at the greatest permitted dip.
    const double gap   = p.r_tip - p.line_w / 2 + p.max_pit * m_tg;
    int          first = 0;
    for (const int end : p.block_lane_ends) {
        const double y0 = first * m_d + (first > 0 ? gap / 2 : 0.);
        const double y1 = end * m_d - (end < p.lanes ? gap / 2 : 0.);
        if (y1 - y0 < 2 * p.line_w)
            break;
        auto& block = m_block_cells.emplace_back(cell, y1 - y0);
        block.m_y0  = y0;
        first       = end;
    }
}

double Lanes::band(int c, double t, double& y0, double& y1) const
{
    const int k = m_p.lanes;
    y0          = m_y0 + c * m_d;
    y1          = m_y0 + (c + 1) * m_d;
    if (c - 1 >= 0 && m_H[c - 1] > t)
        y0 += m_p.r_tip - m_p.line_w / 2 + (m_H[c - 1] - t) * m_tg;
    if (c + 1 < k && m_H[c + 1] > t)
        y1 -= m_p.r_tip - m_p.line_w / 2 + (m_H[c + 1] - t) * m_tg;
    return y1 - y0;
}

void Lanes::push(std::vector<Pass>& out, int demand, int owner, int c, double h, double y0, double y1, bool solid)
{
    Pass ps{demand, owner, c, m_H[size_t(c)], h, y0, y1};
    ps.solid = solid;
    out.push_back(ps);
    m_H[size_t(c)] += h;
}

// ends the merged cell's round: the part no purge reached is raised to the round top
// (open grid; solid while the round is the first layer, it is the tower's base) and the whole
// cell is level again.
void Lanes::close_round(std::vector<Pass>& out, int idx)
{
    if (m_round_h <= 0.)
        return;
    const double Hm = m_H[0];
    if (m_depth - m_round_y - m_round_tail >= m_p.line_w) {
        Pass ps{-1, idx, 0, Hm, m_round_h, m_y0 + m_round_y, m_y0 + m_depth - m_round_tail};
        ps.merged     = true;
        ps.cell_y0    = m_y0;
        ps.cell_y1    = m_y0 + m_depth;
        ps.solid      = m_p.solid_base && m_have_z0 && Hm < m_z0 - 1e-6;
        ps.tool       = m_round_tool;
        ps.ends_round = true;
        m_fill += m_p.width * (m_depth - m_round_y - m_round_tail) * m_round_h * (ps.solid ? 1. : m_p.merged_density);
        out.push_back(ps);
    }
    std::fill(m_H.begin(), m_H.end(), Hm + m_round_h);
    m_round_h    = 0.;
    m_round_y    = 0.;
    m_round_tail = 0.;
    m_round_tools.clear();
}

bool Lanes::place(const Demand& e, int idx, std::vector<Pass>& out)
{
    if (!std::isfinite(e.z) || e.z <= 0.f || !std::isfinite(e.volume) || e.volume < 0.f || !std::isfinite(e.layer_h) || e.layer_h < 0.f ||
        !std::isfinite(e.ramming_volume) || e.ramming_volume < 0.f || e.ramming_volume > e.volume)
        throw std::invalid_argument("Invalid SkyLane tower visit");
    const int    k = m_p.lanes;
    const double z = e.z;
    const double W = m_p.width;
    if (!m_have_z0) {
        m_z0      = z;
        m_have_z0 = true;
    }
    if (!m_p.block_lane_ends.empty())
        return place_material_blocks(e, idx, out);

    // a round left open by an earlier visit ends before the cell stops being a cell
    // (back to lanes) or the first-layer base is closed: both need the footprint level.
    if (m_round_h > 0. && (!(e.active == 1 && k > 1) || (m_p.solid_base && !m_base_done && z > m_z0 + 1e-4)))
        close_round(out, idx);

    // 1) First visit above the first layer: close every lane solid up to the first-layer Z,
    //    so the whole footprint grips the bed (not only where the first purges landed).
    if (m_p.solid_base && !m_base_done && z > m_z0 + 1e-4) {
        for (int c = 0; c < k; ++c)
            if (m_H[size_t(c)] < m_z0 - 1e-6) {
                const double h = m_z0 - m_H[size_t(c)];
                m_fill += W * m_d * h;
                push(out, -1, idx, c, h, m_y0 + c * m_d, m_y0 + (c + 1) * m_d, true);
                out.back().tool = e.old_tool;
            }
        m_base_done = true;
        m_base_now  = true;
    } else
        m_base_now = false;

    // ---- simple zone: all lanes fused into one cell -------------------------------------------
    if (e.active == 1 && k > 1) {
        if (!m_merged) {
            // bring every lane up to the highest one (ordinary lane fills, still with their margins)
            const double top = *std::max_element(m_H.begin(), m_H.end());
            for (int c = 0; c < k; ++c)
                while (m_H[size_t(c)] < top - 1e-6) {
                    const double h = std::min(top - m_H[size_t(c)], m_p.hmax);
                    double       y0, y1;
                    if (band(c, m_H[size_t(c)] + h, y0, y1) < m_p.line_w)
                        return false;
                    m_fill += W * (y1 - y0) * h * m_p.fill_density;
                    push(out, -1, idx, c, h, y0, y1, false);
                    out.back().tool = e.old_tool;
                }
            m_merged = true;
        }
        // the cell is built in ROUNDS, like a classic tower layer: a round is a slab of
        // the whole footprint, as tall as the room under the nozzle allows; each purge fills a
        // strip of it (W x s x round height, solid) next to the previous one, and whatever no
        // purge reached is closed with open grid when the round ends. Before, every purge was
        // spread over the WHOLE cell: 45 mm3 over 30 x 43 mm is a 0.035 mm film, i.e. a min-height
        // pass per toolchange — 2.25 m of line each (5.5 h of purge in MIXED-FS-FD), a perimeter
        // per toolchange, and 9 toolchanges on one layer stacked 9 passes whatever the depth.
        const double reserve = std::min(m_p.merged_reserve, m_p.max_pit / 2);
        const double rise    = e.layer_h > 0.f ? std::min(double(e.layer_h), m_p.hmax) : m_p.hmax;
        const double limit   = std::min(reserve + m_p.hmax, std::max(m_p.max_pit - rise, reserve));
        // An open round keeps taking purges while its top stays within the limit below the
        // nozzle; once it lags more (one layer further and a purge would dip past the pit) it
        // ends, and the next purge opens a taller one. (The part nobody purged is only ever
        // printed by the closing pass, from the round top: its depth is not a dip.)
        // the threshold is NOT `limit`: the closing pass is printed from the round top at
        // the NEXT visit, one layer step higher, and the step the demand carries is not reliable
        // (a thin effective height on a 0.2 layer: ALLIN closed a round 0.52 below the nozzle with
        // a 0.4 pit). Worst step assumed = hmax, so the closing pass never dips past the pit.
        if (m_round_h > 0. && z - (m_H[0] + m_round_h) > std::max(m_p.max_pit - m_p.hmax, 0.) + 1e-9)
            close_round(out, idx);
        // taper: between rounds the cell is level, so it may go on shallower from here.
        // What is left outside stays as it is: a lower neighbour is harmless.
        if (m_round_h <= 0. && e.depth > 0.f && double(e.depth) < m_depth - 1e-6) {
            m_y0 += (m_depth - double(e.depth)) / 2;
            m_depth = double(e.depth);
            m_d     = m_depth / k;
        }
        double Hm   = m_H[0];
        auto   cell = [&](double h, size_t tool) { // full-cell open grid pass, with its perimeter
            Pass ps{-1, idx, 0, Hm, h, m_y0, m_y0 + m_depth};
            ps.merged     = true;
            ps.wall       = true;
            ps.cell_y0    = m_y0;
            ps.cell_y1    = m_y0 + m_depth;
            ps.tool       = tool;
            ps.ends_round = true;
            out.push_back(ps);
            Hm += h;
            std::fill(m_H.begin(), m_H.end(), Hm);
        };
        if (m_round_h <= 0.)
            while (z - Hm > m_p.max_pit + 1e-9) { // safety net: never purge deeper than the pit
                const double h = std::max(std::min(z - m_p.max_pit - Hm, m_p.hmax), m_p.hmin);
                m_fill += W * m_depth * h * m_p.merged_density;
                cell(h, e.old_tool);
            }
        for (bool ramming : {true, false}) {
            double       V    = ramming ? e.ramming_volume : e.volume - e.ramming_volume;
            const size_t tool = ramming ? e.old_tool : e.new_tool;
            // The first material grows from the far end, all others from the
            // near end. It can therefore return alongside its earlier deposit.
            // A second material returning would split a region: close first.
            if (V > 1e-9 && m_round_h > 0. && tool != m_round_tool && !m_round_tools.empty() && tool != m_round_tools.front() &&
                std::find(m_round_tools.begin(), m_round_tools.end(), tool) != m_round_tools.end()) {
                close_round(out, idx);
                Hm = m_H[0];
            }
            int guard = 0;
            while (V > 1e-9) {
                if (++guard > 10000)
                    return false;
                bool opens = false;
                if (m_round_h <= 0.) { // open a round: as tall as the room under the nozzle
                    const double g = z - Hm;
                    if (g < m_p.hmin - 1e-9) {
                        if (!m_p.allow_over_z)
                            return false;
                        const double over = Hm + m_p.hmin - z;
                        if (over > m_p.hmax + 1e-9)
                            return false;
                        if (m_over_z++ == 0)
                            m_first_over = idx;
                        m_over_z_max = std::max(m_over_z_max, over);
                        m_round_h    = m_p.hmin;
                    } else
                        m_round_h = std::min(g, m_p.hmax);
                    m_round_y    = 0.;
                    m_round_tail = 0.;
                    opens        = true;
                }
                const double room = m_depth - m_round_y - m_round_tail;
                const double need = V / (W * m_round_h * m_p.purge_density);
                if (!opens && room < std::min(need, 2. * m_p.line_w)) { // round full: next one
                    close_round(out, idx);
                    Hm = m_H[0];
                    continue;
                }
                double s = std::min(std::max(need, m_p.line_w), room);
                if (room - s < m_p.line_w)
                    s = room;
                const bool   tail = m_round_tools.empty() || tool == m_round_tools.front();
                const double y0   = m_y0 + (tail ? m_depth - m_round_tail - s : m_round_y);
                Pass         ps{idx, idx, 0, Hm, m_round_h, y0, y0 + s};
                ps.merged     = true;
                ps.wall       = opens;
                ps.cell_y0    = m_y0;
                ps.cell_y1    = m_y0 + m_depth;
                ps.density    = m_p.purge_density * std::min(1., need / s);
                ps.tool       = tool;
                ps.ramming    = ramming;
                ps.ends_round = room - s < m_p.line_w;
                m_round_tool  = tool;
                if (std::find(m_round_tools.begin(), m_round_tools.end(), tool) == m_round_tools.end())
                    m_round_tools.push_back(tool);
                out.push_back(ps);
                V -= W * s * m_round_h * m_p.purge_density;
                if (tail)
                    m_round_tail += s;
                else
                    m_round_y += s;
                if (ps.ends_round) { // nothing left to close: the round is done
                    close_round(out, idx);
                    Hm = m_H[0];
                }
            }
        }
        // A switch with no incoming purge must not leave a round whose tail
        // can only be completed by a tool that has already been parked.
        if (e.old_tool != e.new_tool && e.volume == e.ramming_volume) {
            close_round(out, idx);
            Hm = m_H[0];
        }
        // then the open grid: once the cell lags a full pass beyond its reserve, raise it back
        // to the reserve in tall passes (few visits print anything; the pit limit still rules)
        if (m_round_h <= 0. && z - Hm > limit + 1e-9)
            while (z - Hm > reserve + m_p.hmin - 1e-9) {
                const double h = std::min(z - reserve - Hm, m_p.hmax);
                m_fill += W * m_depth * h * m_p.merged_density;
                cell(h, e.new_tool);
            }
        return true;
    }

    // Back to lanes after a merged stretch: they all start level, at the cell's height.
    m_merged = false;

    // Lanes in use from here on.
    const int ka = e.active > 0 ? std::min(e.active, k) : k;
    return place_lanes(e, idx, 0, ka, out);
}

bool Lanes::place_material_blocks(const Demand& e, int idx, std::vector<Pass>& out)
{
    if (e.old_tool >= m_p.tool_block.size() || e.new_tool >= m_p.tool_block.size())
        return false;
    const int old_block = m_p.tool_block.at(e.old_tool), new_block = m_p.tool_block.at(e.new_tool);
    if (old_block < 0 || new_block < 0 || size_t(old_block) >= m_p.block_lane_ends.size() || size_t(new_block) >= m_p.block_lane_ends.size())
        return false;
    const bool above_bed = e.z > m_z0 + 1e-4;
    m_base_now           = m_p.solid_base && above_bed && !m_base_done;
    m_base_done |= m_base_now;

    auto place_block = [&](int block, size_t before, size_t after, float volume, float ram) {
        const int first = block == 0 ? 0 : m_p.block_lane_ends[block - 1];
        const int end   = m_p.block_lane_ends[block];
        Demand    part;
        part.z              = e.z;
        part.layer_h        = e.layer_h;
        part.old_tool       = before;
        part.new_tool       = after;
        part.volume         = volume;
        part.ramming_volume = ram;
        if (m_p.block_rounds) {
            if (m_block_cells.size() != m_p.block_lane_ends.size())
                return false;
            Lanes& cell                = m_block_cells[block];
            cell.m_have_z0             = true;
            cell.m_z0                  = m_z0;
            part.active                = 1;
            const size_t start         = out.size();
            const double previous_fill = cell.m_fill;
            if (!cell.place(part, idx, out))
                return false;
            m_fill += cell.m_fill - previous_fill;
            for (size_t i = start; i < out.size(); ++i) {
                Pass& ps = out[i];
                if (ps.z_base + ps.h > part.z + 1e-5)
                    return false;
                ps.lane        = first;
                ps.block_round = ps.merged;
                ps.merged = ps.wall = false;
                // Only the current tool is mounted, including when it closes
                // an earlier colour's round with a few lines of structural grid.
                ps.tool = before;
                std::fill(m_H.begin() + first, m_H.begin() + end, std::max(m_H[first], ps.z_base + ps.h));
            }
            return true;
        }
        if (m_p.solid_base && above_bed) {
            for (int lane = first; lane < end; ++lane) {
                const double h = m_z0 - m_H[lane];
                if (h <= 1e-6)
                    continue;
                double y0, y1;
                if (band(lane, m_z0, y0, y1) < m_p.line_w)
                    return false;
                m_fill += m_p.width * (y1 - y0) * h;
                push(out, -1, idx, lane, h, y0, y1, true);
                out.back().tool = before;
            }
        }
        return place_lanes(part, idx, first, end, out);
    };
    // If a category visits at this height, its own filament supplies the grid.
    // Across an absence the active tool carries the empty block with sparse
    // support until the category returns (as on the conventional tower).
    for (size_t block = 0; block < m_p.block_lane_ends.size(); ++block) {
        if (int(block) == old_block || int(block) == new_block || e.z > m_p.block_last_z[block] + 1e-4 ||
            std::find(e.present_blocks.begin(), e.present_blocks.end(), int(block)) != e.present_blocks.end())
            continue;
        if (!place_block(int(block), e.old_tool, e.old_tool, 0.f, 0.f))
            return false;
    }
    if (old_block == new_block && (!m_p.block_rounds || e.old_tool == e.new_tool))
        return place_block(old_block, e.old_tool, e.new_tool, e.volume, e.ramming_volume);
    return place_block(old_block, e.old_tool, e.old_tool, e.ramming_volume, e.ramming_volume) &&
           place_block(new_block, e.new_tool, e.new_tool, e.volume - e.ramming_volume, 0.f);
}

bool Lanes::place_lanes(const Demand& e, int idx, int first_lane, int end_lane, std::vector<Pass>& out)
{
    const double z = e.z, W = m_p.width;

    // 1b) Safety net: a lane deeper than max_pit below the nozzle (no visit for a while, or an
    //     emission order the plan did not foresee) is filled up to the pit limit BEFORE purging.
    for (int c = first_lane; c < end_lane; ++c)
        while (z - m_H[size_t(c)] > m_p.max_pit + 1e-9) {
            const double h = std::max(std::min(z - m_p.max_pit - m_H[size_t(c)], m_p.hmax), m_p.hmin);
            double       y0, y1;
            if (band(c, m_H[size_t(c)] + h, y0, y1) < m_p.line_w)
                return false;
            m_fill += W * (y1 - y0) * h * m_p.fill_density;
            push(out, -1, idx, c, h, y0, y1, false);
            out.back().tool = e.old_tool;
        }

    // 2) Purge: lowest lane with room, tall pass.
    double ram_left       = e.ramming_volume;
    auto   split_material = [&]() {
        Pass&        ps     = out.back();
        const double volume = W * (ps.y1 - ps.y0) * ps.h * ps.density;
        const double ram    = std::min(ram_left, volume);
        ps.tool             = ram > 1e-9 ? e.old_tool : e.new_tool;
        ps.ramming          = ram > 1e-9;
        if (ram > 1e-9 && ram < volume - 1e-9) {
            Pass         incoming = ps;
            const double cut      = ps.y0 + (ps.y1 - ps.y0) * ram / volume;
            ps.y1                 = cut;
            incoming.y0           = cut;
            incoming.tool         = e.new_tool;
            incoming.ramming      = false;
            out.push_back(incoming);
        }
        ram_left -= ram;
    };
    double V     = e.volume;
    int    tries = 0;
    while (V > 1e-9) {
        int c = -1; // lowest lane with room (first on ties)
        for (int j = first_lane; j < end_lane; ++j)
            if (z - m_H[size_t(j)] >= m_p.hmin - 1e-9 && (c < 0 || m_H[size_t(j)] < m_H[size_t(c)]))
                c = j;
        if (c < 0) {
            if (!m_p.allow_over_z)
                return false;
            c                 = int(std::min_element(m_H.begin() + first_lane, m_H.begin() + end_lane) - m_H.begin());
            const double over = m_H[size_t(c)] + m_p.hmin - z;
            if (over > m_p.hmax + 1e-9)
                return false; // more than one pass above the nozzle: refuse
            if (m_over_z++ == 0)
                m_first_over = idx;
            m_over_z_max = std::max(m_over_z_max, over);
            double y0, y1;
            if (band(c, m_H[size_t(c)] + m_p.hmin, y0, y1) < m_p.line_w)
                return false;
            V -= W * (y1 - y0) * m_p.hmin * m_p.purge_density;
            push(out, idx, idx, c, m_p.hmin, y0, y1, false);
            out.back().density = m_p.purge_density;
            split_material();
            continue;
        }
        const double g = z - m_H[size_t(c)];
        const double h = std::min({g, m_p.hmax, std::max(V / (W * m_d * m_p.purge_density), m_p.hmin)});
        double       y0, y1;
        const double u = band(c, m_H[size_t(c)] + h, y0, y1);
        if (u < m_p.line_w)
            return false;
        m_fill += W * (m_d - u) * h * m_p.fill_density; // lost strip
        push(out, idx, idx, c, h, y0, y1, false);
        out.back().density = std::min(m_p.purge_density, V / (W * u * h));
        split_material();
        V -= W * u * h * m_p.purge_density;
    }
    // 3) Level lagging lanes so that, one layer higher (~layer_h), no lane is deeper than
    //    max_pit below the nozzle. Never above the current nozzle Z (printed now). Fills respect
    //    the cone margin of taller neighbours too.
    const double rise   = e.layer_h > 0.f ? std::min(double(e.layer_h), m_p.hmax) : m_p.hmax;
    const double target = z - std::max(m_p.max_pit - rise, 0.);
    const double goal   = target + (e.volume <= 0.f ? std::clamp(m_p.catch_up, 0., 1.) : 0.) * (z - target);
    for (int c = first_lane; c < end_lane; ++c) {
        if (!(m_H[size_t(c)] < target - 1e-9))
            continue; // only lanes that hit the limit
        while (m_H[size_t(c)] < goal - 1e-9 && m_H[size_t(c)] + m_p.hmin <= z + 1e-9) {
            const double h = std::min(std::max(std::min(goal - m_H[size_t(c)], m_p.hmax), m_p.hmin), z - m_H[size_t(c)]);
            double       y0, y1;
            if (band(c, m_H[size_t(c)] + h, y0, y1) < m_p.line_w)
                return false;
            m_fill += W * (y1 - y0) * h * m_p.fill_density;
            push(out, -1, idx, c, h, y0, y1, false);
            out.back().tool = e.new_tool;
        }
    }
    return true;
}

Plan pack(const std::vector<Demand>& ev, double D, const Params& p)
{
    Plan out;
    out.depth = D;
    Lanes L(p, D);
    for (size_t i = 0; i < ev.size(); ++i)
        if (!L.place(ev[i], int(i), out.passes)) {
            out.fail_index = int(i);
            return out;
        }
    out.ok          = true;
    out.fill_volume = L.fill_volume();
    out.over_z      = L.over_z();
    out.over_z_max  = L.over_z_max();
    out.first_over  = L.first_over();
    return out;
}

Plan pack_scheduled(std::vector<Demand>& ev, double D, const Params& p)
{
    if (!p.block_lane_ends.empty()) {
        Plan plan = pack(ev, D, p);
        plan.active.assign(ev.size(), p.lanes);
        return plan;
    }
    // Every layer starts in the simple zone (1 = lanes merged into one cell). Where a demand
    // finds no room, that layer becomes a lanes layer (all lanes); if it already was one, its
    // block of lanes layers is extended one layer DOWN, so the lanes have time to build up
    // their headroom before the busy spot. Blocks can appear anywhere: the tower merges on the
    // quiet stretches in between and splits again for the next busy zone.
    std::vector<int> layers; // distinct layer ids, in order
    for (const Demand& e : ev)
        if (layers.empty() || layers.back() != e.layer)
            layers.push_back(e.layer);
    std::vector<int> act(layers.size(), 1);
    auto             apply = [&]() {
        size_t li = 0;
        for (Demand& e : ev) {
            while (li + 1 < layers.size() && layers[li] != e.layer)
                ++li;
            e.active = act[li];
        }
    };
    for (int guard = 0; guard < 200000; ++guard) {
        apply();
        Plan plan = pack(ev, D, p);
        if (plan.ok) {
            plan.active.reserve(ev.size());
            for (const Demand& e : ev)
                plan.active.push_back(e.active);
            return plan;
        }
        const int fl = ev[size_t(std::max(plan.fail_index, 0))].layer;
        size_t    li = 0;
        while (li + 1 < layers.size() && layers[li] != fl)
            ++li;
        if (act[li] == 1) {
            act[li] = p.lanes;
            continue;
        }
        while (li > 0 && act[li - 1] != 1)
            --li; // start of this lanes block
        if (li == 0)
            return plan; // lanes from the bed up and still no room
        act[li - 1] = p.lanes;
    }
    return Plan{};
}

Plan pack_min_depth(const std::vector<Demand>& ev_in, const Params& p)
{
    std::vector<Demand> ev = ev_in;
    double              lo = 1.0, hi = 400.0;
    if (!pack_scheduled(ev, hi, p).ok) {
        // Strictly impossible (Z order not monotonic): retry allowing min passes above the nozzle.
        if (p.allow_over_z || !p.block_lane_ends.empty())
            return Plan{};
        Params q       = p;
        q.allow_over_z = true;
        return pack_min_depth(ev_in, q);
    }
    for (int it = 0; it < 35; ++it) {
        const double mid = (lo + hi) / 2;
        if (pack_scheduled(ev, mid, p).ok)
            hi = mid;
        else
            lo = mid;
    }
    return pack_scheduled(ev, hi, p);
}

Plan pack_taper(std::vector<Demand>& ev, double D, const Params& p, double margin)
{
    // taper. With the tower's depth and its lanes / merged schedule settled (ev[i].active),
    // the merged cell is made shallower wherever the REST of the print still fits in less (like
    // the classic tower, whose layers are only as deep as the ones above them need). Steps of
    // 15 % of the depth; each one starts at the earliest merged layer from which the whole print
    // still packs. A step of depth d must pack at d and at d / margin (the same slack the whole
    // tower is built with), and never with more passes above the nozzle than the plain plan.
    for (Demand& e : ev)
        e.depth = 0.f;
    const Plan base = pack(ev, D, p);
    if (!base.ok || p.taper_min <= 0. || p.lanes < 2)
        return Plan{};
    std::vector<size_t> starts; // first demand of every merged layer
    for (size_t i = 0; i < ev.size(); ++i)
        if (ev[i].active == 1 && (i == 0 || ev[i - 1].layer != ev[i].layer))
            starts.push_back(i);
    if (starts.empty())
        return Plan{};
    auto packs = [&](const std::vector<Demand>& t) {
        const Plan pl = pack(t, D, p);
        return pl.ok && pl.over_z <= base.over_z;
    };
    auto fits = [&](size_t from, double d) {
        std::vector<Demand> t = ev;
        for (size_t i = from; i < t.size(); ++i)
            t[i].depth = float(d);
        if (!packs(t))
            return false;
        if (margin <= 1.)
            return true;
        for (size_t i = from; i < t.size(); ++i)
            t[i].depth = float(d / margin);
        return packs(t);
    };
    size_t pos = 0; // index into starts: steps only go up
    bool   any = false;
    for (double d = D * 0.85; d >= p.taper_min - 1e-9 && pos < starts.size(); d -= D * 0.15) {
        if (!fits(starts.back(), d))
            break;
        size_t lo_i = pos, hi_i = starts.size() - 1; // earliest start that fits (assumed monotonic; the result is re-checked)
        while (lo_i < hi_i) {
            const size_t mid = (lo_i + hi_i) / 2;
            if (fits(starts[mid], d))
                hi_i = mid;
            else
                lo_i = mid + 1;
        }
        for (size_t i = starts[hi_i]; i < ev.size(); ++i)
            ev[i].depth = float(d);
        pos = hi_i;
        any = true;
    }
    Plan tapered = any ? pack(ev, D, p) : Plan{};
    if (!tapered.ok || tapered.over_z > base.over_z) {
        for (Demand& e : ev)
            e.depth = 0.f;
        return Plan{};
    }
    tapered.active.reserve(ev.size());
    tapered.cell_depth.reserve(ev.size());
    for (const Demand& e : ev) {
        tapered.active.push_back(e.active);
        tapered.cell_depth.push_back(e.depth);
    }
    return tapered;
}

}} // namespace Slic3r::SkyLane
