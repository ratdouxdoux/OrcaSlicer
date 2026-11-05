// SkyLane packer, see SkyLanePacker.hpp.
// Adapted from SKYLANE_PACK_RADU_2, standalone SkyLane (AGPL).
// Placement is INCREMENTAL (Lanes::place, one demand at a time) so the very same
// code sizes the tower offline (pack) and places every TCR at emission time (SkyLaneTower.cpp),
// where the real emitted sequence (repeats, unemitted slots) is what lands on the lanes.
#include "SkyLanePacker.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Slic3r { namespace SkyLane {

static const Params& checked_parameters(const Params& p, double depth)
{
    for (double value : {depth, p.width, p.max_pit, p.hmin, p.hmax, p.r_tip, p.cone_deg, p.line_w, p.fill_density, p.merged_density,
                         p.purge_density, p.merged_reserve})
        if (!std::isfinite(value))
            throw std::invalid_argument("SkyLane parameters must be finite");
    if (depth <= 0. || p.width <= 4. * p.line_w || p.lanes < 2 || p.lanes > 6 || p.hmin <= 0. || p.hmax < p.hmin || p.max_pit < p.hmax ||
        p.line_w <= 0. || p.r_tip < p.line_w / 2. || p.cone_deg < 0. || p.cone_deg >= 90. || p.fill_density <= 0. || p.fill_density > 1. ||
        p.merged_density <= 0. || p.merged_density > 1. || p.purge_density <= 0. || p.purge_density > 1.)
        throw std::invalid_argument("Invalid SkyLane dimensions, density or nozzle geometry");
    return p;
}

Lanes::Lanes(const Params& p, double depth)
    : m_p(checked_parameters(p, depth))
    , m_depth(depth)
    , m_d(depth / p.lanes)
    , m_tg(std::tan(p.cone_deg * M_PI / 180.0))
    , m_H(size_t(p.lanes), 0.)
{}

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
    if (m_depth - m_round_y >= m_p.line_w) {
        Pass ps{-1, idx, 0, Hm, m_round_h, m_y0 + m_round_y, m_y0 + m_depth};
        ps.merged  = true;
        ps.cell_y0 = m_y0;
        ps.cell_y1 = m_y0 + m_depth;
        ps.solid   = m_p.solid_base && m_have_z0 && Hm < m_z0 - 1e-6;
        m_fill += m_p.width * (m_depth - m_round_y) * m_round_h * (ps.solid ? 1. : m_p.merged_density);
        out.push_back(ps);
    }
    std::fill(m_H.begin(), m_H.end(), Hm + m_round_h);
    m_round_h = 0.;
    m_round_y = 0.;
}

bool Lanes::place(const Demand& e, int idx, std::vector<Pass>& out)
{
    if (!std::isfinite(e.z) || e.z <= 0.f || !std::isfinite(e.volume) || e.volume < 0.f || !std::isfinite(e.layer_h) || e.layer_h < 0.f)
        throw std::invalid_argument("Invalid SkyLane tower visit");
    const int    k = m_p.lanes;
    const double z = e.z;
    const double W = m_p.width;
    if (!m_have_z0) {
        m_z0      = z;
        m_have_z0 = true;
    }

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
        auto   cell = [&](double h) { // full-cell open grid pass, with its perimeter
            Pass ps{-1, idx, 0, Hm, h, m_y0, m_y0 + m_depth};
            ps.merged  = true;
            ps.wall    = true;
            ps.cell_y0 = m_y0;
            ps.cell_y1 = m_y0 + m_depth;
            out.push_back(ps);
            Hm += h;
            std::fill(m_H.begin(), m_H.end(), Hm);
        };
        if (m_round_h <= 0.)
            while (z - Hm > m_p.max_pit + 1e-9) { // safety net: never purge deeper than the pit
                const double h = std::max(std::min(z - m_p.max_pit - Hm, m_p.hmax), m_p.hmin);
                m_fill += W * m_depth * h * m_p.merged_density;
                cell(h);
            }
        double V     = e.volume;
        int    guard = 0;
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
                m_round_y = 0.;
                opens     = true;
            }
            const double room = m_depth - m_round_y;
            const double need = V / (W * m_round_h * m_p.purge_density);
            if (!opens && room < std::min(need, 2. * m_p.line_w)) { // round full: next one
                close_round(out, idx);
                Hm = m_H[0];
                continue;
            }
            const double s = std::min(std::max(need, m_p.line_w), room);
            Pass         ps{idx, idx, 0, Hm, m_round_h, m_y0 + m_round_y, m_y0 + m_round_y + s};
            ps.merged  = true;
            ps.wall    = opens;
            ps.cell_y0 = m_y0;
            ps.cell_y1 = m_y0 + m_depth;
            ps.density = m_p.purge_density * std::min(1., need / s);
            out.push_back(ps);
            V -= W * s * m_round_h * m_p.purge_density;
            m_round_y += s;
            if (m_depth - m_round_y < m_p.line_w) { // nothing left to close: the round is done
                close_round(out, idx);
                Hm = m_H[0];
            }
        }
        // then the open grid: once the cell lags a full pass beyond its reserve, raise it back
        // to the reserve in tall passes (few visits print anything; the pit limit still rules)
        if (m_round_h <= 0. && z - Hm > limit + 1e-9)
            while (z - Hm > reserve + m_p.hmin - 1e-9) {
                const double h = std::min(z - reserve - Hm, m_p.hmax);
                m_fill += W * m_depth * h * m_p.merged_density;
                cell(h);
            }
        return true;
    }

    // Back to lanes after a merged stretch: they all start level, at the cell's height.
    m_merged = false;

    // Lanes in use from here on.
    const int ka = e.active > 0 ? std::min(e.active, k) : k;

    // 1b) Safety net: a lane deeper than max_pit below the nozzle (no visit for a while, or an
    //     emission order the plan did not foresee) is filled up to the pit limit BEFORE purging.
    for (int c = 0; c < ka; ++c)
        while (z - m_H[size_t(c)] > m_p.max_pit + 1e-9) {
            const double h = std::max(std::min(z - m_p.max_pit - m_H[size_t(c)], m_p.hmax), m_p.hmin);
            double       y0, y1;
            if (band(c, m_H[size_t(c)] + h, y0, y1) < m_p.line_w)
                return false;
            m_fill += W * (y1 - y0) * h * m_p.fill_density;
            push(out, -1, idx, c, h, y0, y1, false);
        }

    // 2) Purge: lowest lane with room, tall pass.
    double V     = e.volume;
    int    tries = 0;
    while (V > 1e-9) {
        int c = -1; // lowest lane with room (first on ties)
        for (int j = 0; j < ka; ++j)
            if (z - m_H[size_t(j)] >= m_p.hmin - 1e-9 && (c < 0 || m_H[size_t(j)] < m_H[size_t(c)]))
                c = j;
        if (c < 0) {
            if (!m_p.allow_over_z)
                return false;
            c                 = int(std::min_element(m_H.begin(), m_H.begin() + ka) - m_H.begin());
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
        V -= W * u * h * m_p.purge_density;
    }

    // 3) Level lagging lanes so that, one layer higher (~layer_h), no lane is deeper than
    //    max_pit below the nozzle. Never above the current nozzle Z (printed now). Fills respect
    //    the cone margin of taller neighbours too.
    const double rise   = e.layer_h > 0.f ? std::min(double(e.layer_h), m_p.hmax) : m_p.hmax;
    const double target = z - std::max(m_p.max_pit - rise, 0.);
    const double goal   = target + (e.volume <= 0.f ? std::clamp(m_p.catch_up, 0., 1.) : 0.) * (z - target);
    for (int c = 0; c < ka; ++c) {
        if (!(m_H[size_t(c)] < target - 1e-9))
            continue; // only lanes that hit the limit
        while (m_H[size_t(c)] < goal - 1e-9 && m_H[size_t(c)] + m_p.hmin <= z + 1e-9) {
            const double h = std::min(std::max(std::min(goal - m_H[size_t(c)], m_p.hmax), m_p.hmin), z - m_H[size_t(c)]);
            double       y0, y1;
            if (band(c, m_H[size_t(c)] + h, y0, y1) < m_p.line_w)
                return false;
            m_fill += W * (y1 - y0) * h * m_p.fill_density;
            push(out, -1, idx, c, h, y0, y1, false);
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
        if (p.allow_over_z)
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
