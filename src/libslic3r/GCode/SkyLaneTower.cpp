// SkyLane prime tower.
// Adapted from SKYLANE_PACK_RADU_2, standalone SkyLane (AGPL).
// See doc/SkyLane.md for the FullSpectrum integration and deliberate differences.
//
// The stock WipeTower2 plans and writes every tool change as usual. With SkyLane on:
//   - plan() runs right after WipeTower2::generate() (Print.cpp). It only SIZES the tower: it packs
//     the planned purges offline (SkyLane::pack_min_depth) and keeps a 15 % margin, because GCode
//     does not always emit exactly the plan. It does not touch any tool change result (TCR).
//   - place() is called for every tool change GCode emits on the tower (GCode.cpp,
//     WipeTowerIntegration::tool_change). It places that TCR on the lanes as they really are at
//     that moment (SkyLane::Lanes, the same incremental code the sizing used) and returns a copy:
//       the purge in the lowest lane as a tall pass, tcr.skylane_z = top of its first pass (the
//       nozzle travels down into the tower and back up), lagging lanes filled, first layer
//       closed solid + brim, the tool change skeleton (CP TOOLCHANGE markers,
//       [change_filament_gcode], deretraction, retracts, temperatures) kept verbatim, and the
//       stock tower's own wall / grid / brim dropped.
// With SkyLane off nothing here runs and the G-code is the stock one.
#include "SkyLaneTower.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <boost/log/trivial.hpp>
#include "libslic3r/PrintConfig.hpp"
#include "SkyLanePacker.hpp"
#include "libslic3r/GCode/WipeTower2.hpp" // get_wipe_tower_cone_base (Cone)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <tuple>
#include <stdexcept>
#include "libslic3r/ClipperUtils.hpp"

namespace Slic3r {

namespace {

#define SKY_LOG(msg) BOOST_LOG_TRIVIAL(debug) << "SkyLane: " << msg

// A number read the same in any UI language (std::stod follows LC_NUMERIC). `fallback` when
// there is no number at the start of `str`.
double to_double(std::string_view str, double fallback)
{
    size_t       used = 0;
    const double v    = string_to_double_decimal_point(str, &used);
    return used > 0 ? v : fallback;
}

// The width the cone base is computed for: the tower plus one line on each side.
double extra_width(const PrintConfig& cfg)
{
    const double nozzle = cfg.nozzle_diameter.values.empty() ? 0.4 : cfg.nozzle_diameter.values.front();
    return 2. * nozzle * 1.25;
}

std::vector<std::string> split_lines(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream       in(s);
    std::string              l;
    while (std::getline(in, l))
        out.push_back(l);
    return out;
}

bool starts(const std::string& l, const char* p) { return l.rfind(p, 0) == 0; }
bool has(const std::string& l, const char* p) { return l.find(p) != std::string::npos; }

double num_after(const std::string& l, char c, double fallback)
{
    // first occurrence of " <c>" or "G1 <c>" style token
    for (size_t i = 0; i + 1 < l.size(); ++i)
        if (l[i] == c && (i == 0 || l[i - 1] == ' ')) {
            return to_double(std::string_view(l).substr(i + 1), fallback);
        }
    return fallback;
}

bool is_xy_move(const std::string& l) { return starts(l, "G1 ") && (has(l, " X") || has(l, " Y")); }

// What we learn from one original TCR.
struct TcrInfo
{
    bool   is_tc = false; // has a real toolchange block
    double purge = 0.;    // mm3 extruded in UNLOAD + WIPE
    double xmin  = std::numeric_limits<double>::max();
    double xmax  = std::numeric_limits<double>::lowest();
};

TcrInfo inspect(const WipeTower::ToolChangeResult& tcr, double fil_area)
{
    TcrInfo r;
    bool    purge = false;
    for (const std::string& l : split_lines(tcr.gcode)) {
        if (has(l, "CP TOOLCHANGE START"))
            r.is_tc = true;
        if (has(l, "CP TOOLCHANGE UNLOAD") || has(l, "CP TOOLCHANGE WIPE"))
            purge = true;
        else if (has(l, "WIPE_TOWER_END") || has(l, "CP EMPTY GRID") || has(l, "BRIM") || has(l, "CP TOOLCHANGE END"))
            purge = false;
        if (!purge || !starts(l, "G1 "))
            continue;
        const double e = num_after(l, 'E', 0.);
        if (e > 0.)
            r.purge += e * fil_area;
        if (has(l, " X")) {
            const double x = num_after(l, 'X', 0.);
            r.xmin         = std::min(r.xmin, x);
            r.xmax         = std::max(r.xmax, x);
        }
    }
    return r;
}

struct Fmt
{
    std::ostringstream o;
    Fmt() { o << std::fixed; }
};

// "Wall type = Rib" for SkyLane: one arm per corner, at 45 degrees (the
// SkyLane footprint is thin, so the rectangle's own diagonals would brace almost nothing in Y).
// An arm is the outline of a strip of half width hw around the corner's outward diagonal, capped
// `e` beyond the corner. Corners 0..3 run counter-clockwise from (x0, y0).
struct RibArm
{
    Vec2d p_in, t_in, t_out, p_out, u;
};

RibArm rib_arm(double x0, double y0, double x1, double y1, int i, double hw, double e)
{
    static const double dirs[4][4] = {{0, -1, 1, 0}, {1, 0, 0, 1}, {0, 1, -1, 0}, {-1, 0, 0, -1}};
    const Vec2d         c(i == 0 || i == 3 ? x0 : x1, i < 2 ? y0 : y1);
    const Vec2d         din(dirs[i][0], dirs[i][1]), dout(dirs[i][2], dirs[i][3]);
    const double        s = hw * std::sqrt(2.);
    RibArm              a;
    a.u     = (din - dout) / std::sqrt(2.);
    a.p_in  = c - din * s;  // where the arm leaves the side that arrives at the corner
    a.p_out = c + dout * s; // where it rejoins the side that leaves the corner
    a.t_in  = a.p_in + a.u * (e + hw);
    a.t_out = a.p_out + a.u * (e + hw);
    return a;
}

// Closed outline (the last point joins the first) of the rectangle with its four arms, starting
// on the bottom side next to corner 0. No arms (hw <= 0 or nothing sticking out) = the rectangle.
std::vector<Vec2d> rib_loop(double x0, double y0, double x1, double y1, double hw, double e)
{
    if (hw <= 0. || e + hw < 1e-6)
        return {Vec2d(x0, y0), Vec2d(x1, y0), Vec2d(x1, y1), Vec2d(x0, y1)};
    std::vector<Vec2d> out;
    const RibArm       a0 = rib_arm(x0, y0, x1, y1, 0, hw, e);
    out.push_back(a0.p_out);
    for (int i = 1; i < 4; ++i) {
        const RibArm a = rib_arm(x0, y0, x1, y1, i, hw, e);
        out.insert(out.end(), {a.p_in, a.t_in, a.t_out, a.p_out});
    }
    out.insert(out.end(), {a0.p_in, a0.t_in, a0.t_out});
    return out;
}

// "Wall gap": the perimeter printed during a toolchange skips the
// stretch in front of the point where that toolchange came in (the classic tower does it for
// every toolchange of the layer: contrust_gap_for_skip_points). A ray leaves `entry` along X
// towards the nearer side of the tower and `gap` mm of the loop are dropped around the point it
// hits. On a long straight side the gap slides to stay `keep` mm clear of both corners.
// Returns the loop as an open path, from one edge of the gap all the way round to the other
// (empty = the ray missed the loop: print it closed, as without the option).
std::vector<Vec2d> open_loop_at(const std::vector<Vec2d>& loop, const Vec2d& entry, bool to_far_x, double gap, double keep)
{
    const size_t n = loop.size();
    if (n < 3 || gap <= 0.)
        return {};
    std::vector<double> at(n + 1, 0.); // arc length at every vertex (at[n] = the whole loop)
    for (size_t i = 0; i < n; ++i)
        at[i + 1] = at[i] + (loop[(i + 1) % n] - loop[i]).norm();
    const double total = at[n];
    if (total < 2. * gap)
        return {};
    // (an entry standing on a corner of the loop, to float precision, must still hit its side)
    double ymin = loop.front().y(), ymax = ymin;
    for (const Vec2d& q : loop) {
        ymin = std::min(ymin, q.y());
        ymax = std::max(ymax, q.y());
    }
    const double ey   = std::clamp(entry.y(), ymin + 1e-3, ymax - 1e-3);
    double       best = std::numeric_limits<double>::max(), s_hit = 0.;
    size_t       seg = n;
    for (size_t i = 0; i < n; ++i) {
        const Vec2d &a = loop[i], &b = loop[(i + 1) % n];
        if ((a.y() - ey) * (b.y() - ey) > 0. || std::abs(b.y() - a.y()) < 1e-9)
            continue;
        const double t = (ey - a.y()) / (b.y() - a.y());
        const double x = a.x() + t * (b.x() - a.x());
        const double d = to_far_x ? x - entry.x() : entry.x() - x;
        if (d < -1e-6 || d >= best)
            continue;
        best  = d;
        seg   = i;
        s_hit = at[i] + t * (at[i + 1] - at[i]);
    }
    if (seg == n)
        return {};
    double lo = s_hit - gap / 2;
    if (at[seg + 1] - at[seg] >= gap + 2. * keep) // room on this side: keep the gap off its corners
        lo = std::clamp(lo, at[seg] + keep, at[seg + 1] - keep - gap);
    const double hi    = lo + gap;
    auto         point = [&](double s) {
        s = std::fmod(s, total);
        if (s < 0.)
            s += total;
        size_t i = 0;
        while (i + 1 < n && at[i + 1] < s)
            ++i;
        const double len = at[i + 1] - at[i];
        const Vec2d &a = loop[i], &b = loop[(i + 1) % n];
        return len > 1e-9 ? Vec2d(a + (b - a) * ((s - at[i]) / len)) : a;
    };
    std::vector<Vec2d> out{point(hi)};
    for (int turn = 0; turn < 2; ++turn)
        for (size_t i = 0; i < n; ++i)
            if (const double s = at[i] + turn * total; s > hi + 1e-6 && s < lo + total - 1e-6)
                out.push_back(loop[i]);
    out.push_back(point(lo + total));
    return out;
}

// Keep only the part of an existing wall inside a material band. This does not
// add dividing walls: neighbouring bands meet at exactly the same endpoints.
std::vector<std::vector<Vec2d>> clip_wall_band(const std::vector<Vec2d>& path, bool closed, double low, double high)
{
    std::vector<std::vector<Vec2d>> paths;
    if (path.size() < 2)
        return paths;
    for (size_t i = 1; i < path.size() + (closed ? 1 : 0); ++i) {
        const Vec2d& a  = path[i - 1];
        const Vec2d& b  = path[i % path.size()];
        const double dy = b.y() - a.y();
        double       t0 = 0., t1 = 1.;
        if (std::abs(dy) < 1e-9) {
            if (a.y() < low || a.y() >= high)
                continue;
        } else {
            double u = (low - a.y()) / dy, v = (high - a.y()) / dy;
            if (u > v)
                std::swap(u, v);
            t0 = std::max(t0, u);
            t1 = std::min(t1, v);
            if (t1 - t0 < 1e-9)
                continue;
        }
        const Vec2d from = a + (b - a) * t0, to = a + (b - a) * t1;
        if (paths.empty() || (paths.back().back() - from).squaredNorm() > 1e-12)
            paths.push_back({from});
        paths.back().push_back(to);
    }
    if (paths.size() > 1 && (paths.back().back() - paths.front().front()).squaredNorm() < 1e-12) {
        auto last = std::move(paths.back());
        paths.pop_back();
        last.insert(last.end(), paths.front().begin() + 1, paths.front().end());
        paths.front() = std::move(last);
    }
    return paths;
}

// "Fillet wall": every corner of the path becomes an arc tangent to both sides, cut `d` mm back
// from the corner (same 2 mm as the classic rib wall; less on short sides). No sharp turns: the
// head does not brake at each corner and the wall has no stress points.
std::vector<Vec2d> rib_round(const std::vector<Vec2d>& pts, bool closed, double d)
{
    const int n = int(pts.size());
    if (n < 3)
        return pts;
    std::vector<Vec2d> out;
    for (int i = 0; i < n; ++i) {
        const Vec2d& b = pts[size_t(i)];
        if (!closed && (i == 0 || i == n - 1)) {
            out.push_back(b);
            continue;
        }
        const Vec2d  ab = b - pts[size_t((i + n - 1) % n)], bc = pts[size_t((i + 1) % n)] - b;
        const double lab = ab.norm(), lbc = bc.norm();
        if (lab < 1e-6 || lbc < 1e-6) {
            out.push_back(b);
            continue;
        }
        const Vec2d  u = ab / lab, v = bc / lbc;
        const double cross = u.x() * v.y() - u.y() * v.x();
        const double turn  = std::atan2(std::abs(cross), u.dot(v));
        const double t     = std::min({d, lab / 2.1, lbc / 2.1});
        const double r     = t / std::tan(turn / 2.);
        // nearly straight, a U-turn, or an arc too small to be worth its segments: keep the corner
        if (turn < 0.17 || turn > M_PI - 0.17 || r * turn < 0.3) {
            out.push_back(b);
            continue;
        }
        const double sg     = cross > 0. ? 1. : -1.;
        const Vec2d  left   = b - u * t;
        const Vec2d  center = left + Vec2d(-u.y(), u.x()) * (sg * r);
        const int    segs   = std::max(2, std::min(int(std::ceil(turn / (M_PI / 12.))), int(std::ceil(r * turn / 0.3))));
        for (int j = 0; j <= segs; ++j) {
            const double a = sg * turn * j / segs, ca = std::cos(a), sa = std::sin(a);
            const Vec2d  q = left - center;
            out.push_back(center + Vec2d(ca * q.x() - sa * q.y(), sa * q.x() + ca * q.y()));
        }
    }
    return out;
}

// "Wall type = Cone" for SkyLane: the classic tower's stabilization
// cone. The wall bulges out of the two long sides along an ellipse centred on the tower (semi-axes
// a along X, b along Y) that shrinks with height: b = tan(apex angle / 2) x (tower height - z).
// Part of that ellipse on side sgn (+1 = far Y, -1 = near Y), from angle t0 to t1, ~1 mm segments.
std::vector<Vec2d> cone_span(double cx, double cy, double a, double b, int sgn, double t0, double t1)
{
    const int          n = std::clamp(int(std::ceil(std::abs(t1 - t0) * std::max(a, b))), 2, 90);
    std::vector<Vec2d> out;
    for (int i = 0; i <= n; ++i) {
        const double t = t0 + (t1 - t0) * i / n;
        out.emplace_back(cx + a * std::cos(t), cy + sgn * b * std::sin(t));
    }
    return out;
}

// The arc of side sgn that lies at least d away from the tower's long axis, left to right.
// Empty when the ellipse does not reach that far.
std::vector<Vec2d> cone_arc(double cx, double cy, double a, double b, int sgn, double d)
{
    if (b <= d + 1e-6)
        return {};
    const double t = std::asin(d / b);
    return cone_span(cx, cy, a, b, sgn, M_PI - t, t);
}

// Closed outline, counter-clockwise from (x0, y0), of the rectangle with the two arcs standing on
// its long sides. No cone (b too small, or a chord wider than the side) = the rectangle.
std::vector<Vec2d> cone_loop(double x0, double y0, double x1, double y1, double a, double b)
{
    const double             cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, d = (y1 - y0) / 2;
    const std::vector<Vec2d> lo = cone_arc(cx, cy, a, b, -1, d), hi = cone_arc(cx, cy, a, b, 1, d);
    const bool               fits = !lo.empty() && lo.back().x() < x1 - 1e-3;
    std::vector<Vec2d>       out{Vec2d(x0, y0)};
    if (fits)
        out.insert(out.end(), lo.begin(), lo.end());
    out.emplace_back(x1, y0);
    out.emplace_back(x1, y1);
    if (fits)
        out.insert(out.end(), hi.rbegin(), hi.rend());
    out.emplace_back(x0, y1);
    return out;
}

// purge ramp ("Purge ramp"). A toolchange starts its purge at a quarter of the flow the
// incoming filament admits and gets all of it once `ramp` mm3 are out: the limit follows the
// PLASTIC laid since the toolchange, not the line count (Classic's 33 / 37.5 / 46 / 87.5 % per line
// costs 1.1 s per toolchange on 19 mm lines and 2.0 s on 30 mm ones, and with a small purge the
// top speed is never reached). A pass that never asks for that much flow is not slowed at all.
double purge_flow_cap(double vol, double fmax, double ramp) { return ramp <= 0. || vol >= ramp ? fmax : fmax * (0.25 + 0.75 * vol / ramp); }
// Seconds to lay the purge volume [v0, v1] of a toolchange on a pass whose own ceiling is q_pass mm3/s.
double purge_seconds(double v0, double v1, double fmax, double ramp, double q_pass)
{
    double       t    = 0.;
    const double step = ramp / 16.;
    while (ramp > 0. && v0 < std::min(v1, ramp) - 1e-9) {
        const double dv = std::min(step, std::min(v1, ramp) - v0);
        t += dv / std::min(q_pass, purge_flow_cap(v0, fmax, ramp));
        v0 += dv;
    }
    return v1 > v0 ? t + (v1 - v0) / std::min(q_pass, fmax) : t;
}

// Outgoing-tool paths are allocated before emission, so a tool change never
// divides a purge row. The endpoint also tells the integration where to park.
struct OutgoingBlock
{
    std::string gcode;
    Vec2f       end     = Vec2f::Zero();
    double      z       = 0.;
    bool        printed = false;
};

} // namespace

struct SkyLaneTower::Runtime
{
    SkyLane::Params                 p;
    std::unique_ptr<SkyLane::Lanes> lanes;
    double                          depth = 0., xmin = 0., xmax = 0., z_off = 0., brim = 0.;
    float                           purge = 0.f; // mm3 per toolchange ("Prime volume")
    std::vector<double>             fil_area;    // per tool, mm2
    std::vector<double>             H;           // lane tops as already WRITTEN (safe travel heights)
    std::vector<int>                lane_pass;   // per lane: alternate X / Y lines on every pass
    std::vector<char>               on_grid;     // per lane: its top is the merged cell's open grid
    std::vector<char>               lane_x;      // per lane: the next pass runs along X (always the other way than the last one)
    // "Profile speeds": the tower takes its limits from the profile (filament max
    // volumetric speed, max wipe tower speed, initial layer speed) and ramps every purge up.
    std::map<float, Polylines> outlines;
    std::vector<float>         used_filament;
    bool                       profile_speeds = false;
    std::vector<double>        fil_flow;                              // per tool, mm3/s (0 = unset)
    double                     first_speed = 30., purge_speed = 250.; // mm/s
    double                     ramp        = 0.;                      // "Purge ramp", mm3 (0 = none; only with profile_speeds)
    int                        ramp_logged = 0;
    bool                       brim_done   = false;
    double                     wall_h = 0., wall_top = 0.; // base wall ring: target height, printed top
    bool                       wall_ended    = false;      // the ring stops for good at the first merged zone
    bool                       wall_gap      = false;      // "Wall gap" (open_loop_at)
    bool                       band_wall_gap = false, band_wall_transition = false, band_wall_roots = false;
    Vec2d                      band_wall_entry = Vec2d::Zero();
    // ramming of the OUTGOING tool (filament "Enable ramming for multi-tool setups"):
    // per tool, mm3 (0 = that filament does not ram) and mm3/s.
    std::vector<double> ram_vol, ram_flow;
    double              ram(size_t tool) const { return tool < ram_vol.size() ? ram_vol[tool] : 0.; }
    // Wall type = Rib: corner arms (rib_arm). rib_top = printed top of the arm parts that
    // lie beyond the fence line (the only parts a lanes zone may raise: nozzle-cone clearance).
    bool   rib = false, rib_fillet = false, base_done = false;
    double rib_hw = 0., rib_len = 0., height = 0., rib_top = 0.;
    // Wall type = Cone: the Y semi-axis of the cone at height z (0 = no cone there: it
    // only exists while it reaches past the fence line, cone_min_b). X semi-axis = b / cone_sx.
    // It shares rib_top (printed top of the part beyond the fence line) with the Rib arms.
    bool   cone     = false;
    double cone_tan = 0., cone_sx = 1., cone_min_b = 0.;
    double cone_b(double z) const
    {
        const double b = cone ? cone_tan * std::max(height - z, 0.) : 0.;
        return b > cone_min_b ? b : 0.;
    }
    // How far an arm sticks out at height z: full length on the bed, nothing at the tower top.
    double rib_ext(double z) const { return height > 0. ? rib_len * std::clamp((height - z) / height, 0., 1.) : 0.; }
    // Lane schedule from the offline plan: segments (nozzle Z up to which, lanes in use), Z
    // ascending. 1 = lanes merged into one cell; merged and lanes zones may alternate.
    std::vector<std::pair<float, int>> schedule;
    std::map<long, std::vector<int>>   material_visits;
    int                                active_at(float z) const
    {
        for (const auto& s : schedule)
            if (s.first >= z - 1e-4f)
                return s.second;
        return schedule.empty() ? 0 : schedule.back().second;
    }
    // taper: depth of the merged cell from the offline plan (pack_taper), same layout as
    // `schedule`: segments (nozzle Z up to which, depth), Z ascending. 0 = the full depth.
    std::vector<std::pair<float, float>> depth_sched;
    // the plan in a few comment lines, written into the G-code with the first tower visit:
    // a G-code then says on its own what the tower planned (no debug log to keep next to it).
    std::string summary;
    float       depth_at(float z) const
    {
        for (const auto& s : depth_sched)
            if (s.first >= z - 1e-4f)
                return s.second;
        return depth_sched.empty() ? 0.f : depth_sched.back().second;
    }
    int placed = 0, over_z_logged = 0;
    // how far the emission strays from the plan: purges planned / emitted, and emitted ones
    // the plan did not have (a repeat, or a toolchange it never foresaw). Key = (Z in microns, old, new).
    using PurgeKey = std::tuple<long, int, int>;
    std::map<PurgeKey, int> planned_keys, emitted_keys;
    int                     planned_purges = 0, emitted_purges = 0, extra_purges = 0;
    static PurgeKey purge_key(const SkyLane::Demand& d) { return {std::lround(double(d.z) * 1000.), int(d.old_tool), int(d.new_tool)}; }
    double          deepest = 0.;
    double          area(size_t t) const { return t < fil_area.size() ? fil_area[t] : M_PI * 1.75 * 1.75 / 4.; }
};

bool SkyLaneTower::plan(const PrintConfig&                                           cfg,
                        const std::vector<std::vector<WipeTower::ToolChangeResult>>& tool_changes,
                        const std::vector<std::vector<WipeTower::ToolChangeResult>>& local_z_tool_changes,
                        float                                                        brim_width)
{
    m_rt.reset();
    m_depth = 0.f;

    // ---- parameters -------------------------------------------------------------------
    SkyLane::Params p;
    p.width = cfg.prime_tower_width.value;
    p.lanes = 2; // chosen below (the "Lanes" setting is no longer read)
    if (cfg.nozzle_diameter.values.empty())
        throw std::invalid_argument("SkyLane requires a configured nozzle");
    const double smallest_nozzle = *std::min_element(cfg.nozzle_diameter.values.begin(), cfg.nozzle_diameter.values.end());
    const double largest_nozzle  = *std::max_element(cfg.nozzle_diameter.values.begin(), cfg.nozzle_diameter.values.end());
    p.line_w                     = largest_nozzle * 1.25;
    p.hmax                       = smallest_nozzle * .75;
    p.r_tip                      = std::max(.5, largest_nozzle) + (largest_nozzle - smallest_nozzle) * 1.25 / 2.;
    double pit                   = cfg.skylane_max_dip.value;
    if (pit == 0.) {
        pit = p.hmax;
        for (double hop : cfg.z_hop.values)
            pit = std::max(pit, hop);
    } else {
        p.hmax = std::min(p.hmax, pit);
    }
    p.max_pit = pit;
    if (!std::isfinite(pit) || pit < p.hmin || pit > 2. || !std::isfinite(cfg.skylane_ramp_volume.value) ||
        cfg.skylane_ramp_volume.value < 0.)
        throw std::invalid_argument("Invalid SkyLane dip or ramp volume");
    if (cfg.wipe_tower_no_sparse_layers || cfg.enable_tower_interface_features)
        throw std::invalid_argument("SkyLane requires No sparse layers and tower interface features to be disabled");
    p.solid_base = true;
    // Hybrid: lagging lanes are raised with an open grid (like the classic tower's
    // sparse layers; the X/Y alternation makes it a crossed grid the next purge bridges), and
    // lanes the rest of the print no longer needs are RETIRED (pack_scheduled): the tower tapers
    // to one lane on single-colour stretches.
    p.fill_density = 0.2;
    p.catch_up     = 0.; // tall jumps on quiet layers cost depth (59 mm on multi-test): off
    // "Wipe tower purge lines spacing", the classic tower's own knob (120 % in the Snapmaker
    // profiles; SkyLane has no separate one): same volume over lines further apart =
    // a deeper tower.
    p.purge_density = 1. / (std::clamp(cfg.wipe_tower_extra_spacing.value, 100., 300.) / 100.);

    auto rt      = std::make_shared<Runtime>();
    rt->z_off    = cfg.z_offset.value;
    rt->brim     = brim_width;
    rt->wall_h   = std::max(0., cfg.skylane_wall_height.value);
    rt->wall_gap = cfg.wipe_tower_wall_gap.value;
    for (double d : cfg.filament_diameter.values)
        rt->fil_area.push_back(M_PI * d * d / 4.);
    rt->profile_speeds = cfg.skylane_profile_speeds.value;
    rt->ramp           = rt->profile_speeds ? std::max(0., cfg.skylane_ramp_volume.value) : 0.;
    // Resolve this fork's filament mappings rather than indexing physical tools.
    // Ramming disabled means the outgoing tool arrives dry.
    for (size_t tool = 0; tool < rt->fil_area.size(); ++tool) {
        rt->fil_flow.push_back(get_value_at(cfg, cfg.filament_max_volumetric_speed, ConfigFlowDomain::Filament, unsigned(tool)));
        rt->ram_vol.push_back(get_value_at(cfg, cfg.filament_multitool_ramming, ConfigFlowDomain::Filament, unsigned(tool)) ?
                                  get_value_at(cfg, cfg.filament_multitool_ramming_volume, ConfigFlowDomain::Filament, unsigned(tool)) :
                                  0.);
        rt->ram_flow.push_back(get_value_at(cfg, cfg.filament_multitool_ramming_flow, ConfigFlowDomain::Filament, unsigned(tool)));
    }
    const double first_speed = get_value_at(cfg, cfg.initial_layer_speed, ConfigFlowDomain::Process, 0);
    if (first_speed > 0.)
        rt->first_speed = first_speed;
    if (cfg.wipe_tower_max_purge_speed.value > 0.)
        rt->purge_speed = cfg.wipe_tower_max_purge_speed.value;

    // ONE purge knob for SkyLane: Print settings > Prime tower > "Prime volume", for every tool
    // change. The volumes WipeTower2 planned with (purging volumes, "Minimal purge on wipe tower")
    // only shape the TCRs it hands us; SkyLane re-lays the purge anyway, so it uses its own figure.
    const float purge_volume = float(std::max(0., cfg.prime_volume.value));
    rt->purge                = purge_volume;

    // ---- planned demand (sizing only), in tool_changes order ----------------------------------
    std::vector<SkyLane::Demand> demand;
    double                       xmin = std::numeric_limits<double>::max(), xmax = std::numeric_limits<double>::lowest();
    auto                         add = [&](const WipeTower::ToolChangeResult& t, int layer) {
        TcrInfo info = inspect(t, rt->area(size_t(std::max(t.new_tool, 0))));
        if (info.xmin <= info.xmax) {
            xmin = std::min(xmin, info.xmin);
            xmax = std::max(xmax, info.xmax);
        }
        SkyLane::Demand d;
        d.z        = t.print_z;
        d.layer    = layer;
        d.layer_h  = t.layer_height;
        d.old_tool = size_t(std::max(t.initial_tool, 0));
        d.new_tool = size_t(std::max(t.new_tool, 0));
        d.volume = info.is_tc ? purge_volume + float(rt->ram(d.old_tool)) : 0.f; // the ramming is laid in the tower too
        d.ramming_volume = info.is_tc ? float(rt->ram(d.old_tool)) : 0.f;
        demand.push_back(d);
    };
    for (size_t li = 0; li < tool_changes.size(); ++li) {
        // "Subdivide Mix Layer" tool changes of the layer: GCode emits them before the layer's own.
        if (li < local_z_tool_changes.size())
            for (const auto& t : local_z_tool_changes[li])
                add(t, int(li));
        for (const auto& t : tool_changes[li])
            add(t, int(li));
    }
    if (demand.empty())
        return false;
    // Use the same filament-indexed categories (and missing-entry default)
    // as WipeTower2::get_filament_category. Only tools on this plate count.
    const auto category = [&](size_t tool) {
        const auto& categories = cfg.filament_adhesiveness_category.values;
        return tool < categories.size() ? categories[tool] : 0;
    };
    std::map<int, int> material_blocks;
    for (const auto& d : demand) {
        material_blocks.emplace(category(d.old_tool), 0);
        material_blocks.emplace(category(d.new_tool), 0);
    }
    std::vector<double> peak_load;
    if (material_blocks.size() > 1) {
        int index = 0;
        for (auto& entry : material_blocks)
            entry.second = index++;
        peak_load.assign(index, 0.);
        p.block_last_z.assign(index, 0.);
        std::map<std::pair<int, int>, double> layer_volume;
        for (const auto& d : demand) {
            auto& present = rt->material_visits[std::lround(d.z * 10000.)];
            for (size_t tool : {d.old_tool, d.new_tool}) {
                const int block = material_blocks.at(category(tool));
                p.tool_block.resize(std::max(p.tool_block.size(), tool + 1), -1);
                p.tool_block[tool]    = block;
                p.block_last_z[block] = std::max(p.block_last_z[block], double(d.z));
                if (std::find(present.begin(), present.end(), block) == present.end())
                    present.push_back(block);
            }
            layer_volume[{d.layer, p.tool_block[d.old_tool]}] += d.ramming_volume;
            layer_volume[{d.layer, p.tool_block[d.new_tool]}] += d.volume - d.ramming_volume;
        }
        for (const auto& [key, volume] : layer_volume)
            peak_load[key.second] = std::max(peak_load[key.second], volume);
        for (auto& d : demand)
            d.present_blocks = rt->material_visits.at(std::lround(d.z * 10000.));
    }
    for (const SkyLane::Demand& d : demand)
        if (d.volume > 0.f) {
            ++rt->planned_keys[Runtime::purge_key(d)];
            ++rt->planned_purges;
        }
    if (!(xmin < xmax)) {
        xmin = p.line_w;
        xmax = p.width;
    }
    for (const SkyLane::Demand& d : demand)
        rt->height = std::max(rt->height, double(d.z));

    // Full demand dump (one line per planned TCR), at trace level: lets an offline checker replay
    // exactly what was sized.
    for (size_t i = 0; i < demand.size(); ++i)
        BOOST_LOG_TRIVIAL(trace) << "SkyLane: DEMAND " << i << " li=" << demand[i].layer << " z=" << demand[i].z
                                 << " lh=" << demand[i].layer_h << " V=" << demand[i].volume << " T" << demand[i].old_tool << ">T"
                                 << demand[i].new_tool;
    // the number of lanes is SkyLane's own business: it plans with 2..6 and keeps the
    // shallowest tower (the best count depends on the print: BIGTEST 54.6 / 30.2 / 30.9 / 31.7 /
    // 31.5 mm for 2..6 lanes, the same print scaled in Z 23.8 / 24.2 / 24.1 / 24.0 / 24.7). On a
    // tie, the fewer lanes. About a second for the five plans on 2 154 toolchanges.
    // no lanes narrower than the nozzle can work in: a shallow tower split in 3 gave
    // 2.7 mm lanes (FSFD_stairs_multipass, 35 x 8.1): hardly any band left between the cone
    // margins, edge lines over nothing, near misses. So counts that leave lanes under 4 mm are
    // out (2 lanes are always allowed).
    const double min_lane = 4.0; // mm
    // and between towers of nearly the same depth, the FASTER one. A purge is laid at
    // min(flow limit, speed limit x line x pass height), so with the profile's 90 mm/s a thin pass
    // is slow: 15 mm3 take 3.3 s at 0.10 mm and 1.3 s at 0.27 mm. Narrower lanes give taller passes.
    // The estimate below is the same sum the G-code makes (purge with its ramp, grid,
    // perimeters); against two real slices of BIGTEST scaled in Z it gave 2 lanes -> 3 lanes
    // = -0.17 h where the G-code says -0.16 h. Rule: among the counts within 3 % of the
    // shallowest depth, the fastest, and more lanes only for at least 2 % less tower time.
    const double est_flow = [&] {
        double f = 15.;
        if (rt->profile_speeds) {
            f = std::numeric_limits<double>::max();
            for (double v : rt->fil_flow)
                if (v > 0.)
                    f = std::min(f, v);
            if (f == std::numeric_limits<double>::max())
                f = 15.;
        }
        return f;
    }();
    const double est_vmax      = rt->profile_speeds ? rt->purge_speed : 250.;
    auto         tower_seconds = [&](const SkyLane::Plan& pl, const SkyLane::Params& q, double depth) {
        double t = 0., rv = 0.;
        int    last = -2; // rv = purge mm3 of the current toolchange so far
        for (const SkyLane::Pass& ps : pl.passes) {
            const double box = q.width * (ps.y1 - ps.y0) * ps.h;
            if (ps.demand >= 0) {
                if (ps.demand != last) {
                    rv   = 0.;
                    last = ps.demand;
                }
                const double vol = box * ps.density;
                t += purge_seconds(rv, rv + vol, est_flow, rt->ramp, est_vmax * q.line_w * ps.h);
                rv += vol;
            } else {
                const double dens = ps.solid ? 1. : ((ps.merged || ps.block_round) ? q.merged_density : q.fill_density);
                t += box * dens / (q.line_w * ps.h) / std::min(ps.solid ? rt->first_speed : 80., est_flow / (q.line_w * ps.h));
            }
            if (ps.wall)
                t += (2. * (q.width + depth) + 60.) / std::min(60., est_flow / (q.line_w * ps.h));
        }
        return t;
    };
    struct Cand
    {
        SkyLane::Plan   plan;
        SkyLane::Params p;
        double          secs = 0.;
    };
    std::vector<Cand> cands;
    const int         min_lanes = std::max(2, int(peak_load.size()));
    const int         max_lanes = std::max(6, 6 * int(peak_load.size()));
    auto              try_lanes = [&](int k, bool block_rounds) {
        SkyLane::Params q = p;
        q.lanes           = k;
        q.block_rounds    = block_rounds;
        if (!peak_load.empty()) {
            std::vector<int> counts(peak_load.size(), 1);
            for (int spare = k - int(counts.size()); spare > 0; --spare) {
                size_t busiest = counts.size();
                for (size_t b = 0; b < counts.size(); ++b)
                    if (counts[b] < 6 && (busiest == counts.size() || peak_load[b] / counts[b] > peak_load[busiest] / counts[busiest]))
                        busiest = b;
                ++counts[busiest];
            }
            int end = 0;
            for (int count : counts)
                q.block_lane_ends.push_back(end += count);
        }
        SkyLane::Plan cand = SkyLane::pack_min_depth(demand, q);
        if (!peak_load.empty() && !block_rounds && cand.ok && k > min_lanes)
            cand.depth = std::max(cand.depth, min_lane * k / 1.15);
        const bool narrow = peak_load.empty() && cand.ok && k > min_lanes && cand.depth * 1.15 / k < min_lane;
        double     secs   = 0.;
        if (cand.ok && !narrow) { // timed on the tower as built: + 15 % of depth, zones planned again
            SkyLane::Params qb                 = q;
            qb.allow_over_z                    = cand.over_z > 0;
            std::vector<SkyLane::Demand> dem   = demand;
            const SkyLane::Plan          built = SkyLane::pack_scheduled(dem, cand.depth * 1.15, qb);
            if (!peak_load.empty() && (!built.ok || built.over_z > 0))
                return;
            secs = tower_seconds(built.ok ? built : cand, q, cand.depth * 1.15);
        }
        SKY_LOG("LANES_AUTO lanes=" << k << " ok=" << cand.ok << " depth=" << cand.depth << " lane_width=" << cand.depth * 1.15 / k
                                                 << " fill_mm3=" << cand.fill_volume << " tower_h=" << secs / 3600.
                                                 << (narrow ? " (lanes under 4 mm: out)" : ""));
        if (!cand.ok || cand.over_z > 0 || narrow)
            return;
        cands.push_back({std::move(cand), q, secs});
    };
    if (!peak_load.empty())
        for (int k = min_lanes; k <= max_lanes; ++k)
            try_lanes(k, true);
    // Schedules with overlapping Local-Z visits may require independent lane
    // heights even within a category. The material boundaries still apply.
    if (cands.empty())
        for (int k = min_lanes; k <= max_lanes; ++k)
            try_lanes(k, false);
    SkyLane::Plan plan;
    if (!cands.empty()) {
        double dmin = std::numeric_limits<double>::max();
        for (const Cand& c : cands)
            dmin = std::min(dmin, c.plan.depth);
        size_t best = cands.size();
        for (size_t i = 0; i < cands.size(); ++i) { // ascending lane count
            if (cands[i].plan.depth > dmin * 1.03)
                continue;
            if (best == cands.size() || cands[i].secs < cands[best].secs * 0.98)
                best = i;
        }
        SKY_LOG("LANES_PICK lanes=" << cands[best].p.lanes << " depth=" << cands[best].plan.depth << " (shallowest " << dmin
                                    << ") tower_h=" << cands[best].secs / 3600.);
        p    = cands[best].p;
        plan = std::move(cands[best].plan);
    }
    if (!plan.ok || plan.over_z > 0) {
        SKY_LOG("FAIL no feasible depth (demands=" << demand.size() << ")");
        return false;
    }
    rt->p              = p;
    rt->p.allow_over_z = false;             // Reject an impossible placement instead of emitting a partial purge.
    rt->depth          = plan.depth * 1.15; // margin for emissions the plan did not foresee
    // the lane schedule (which layers are merged, which are lanes) must be the one of the
    // depth the tower is BUILT with, not of the minimum depth it was sized with: a deeper cell
    // fills its rounds differently, so the zones of the minimum-depth plan no longer fit it. The
    // planned demand itself, replayed on the x1.15 tower with the old schedule, put passes above
    // the nozzle: 9 on BIGTEST (the "inherited" over_z at z=11.48) and 44 on BIGTEST scaled in Z
    // (up to 0.16 mm, 19 nozzle collisions around z=32). Re-planned at the real depth: 0 and 0.
    {
        SkyLane::Params q                 = p;
        q.allow_over_z                    = plan.over_z > 0;
        std::vector<SkyLane::Demand> dem  = demand;
        SkyLane::Plan                real = SkyLane::pack_scheduled(dem, rt->depth, q);
        SKY_LOG("RESCHEDULE depth=" << rt->depth << " ok=" << real.ok << " over_z=" << real.over_z << " over_z_max=" << real.over_z_max
                                    << " fill_mm3=" << real.fill_volume);
        if (real.ok)
            plan.active = std::move(real.active);
    }
    // taper: with the depth and the zones settled, the merged cell gets shallower (centred)
    // wherever the rest of the print fits in less, like the layers of the classic tower. Rectangle
    // and Cone; with Rib the tower is left as it was (its arms leave from the full footprint).
    // Never under 8 mm, nor under min_lane per lane (the lanes of a later busy zone live in it).
    std::vector<float> cell_depth;
    if (p.block_lane_ends.empty() && cfg.wipe_tower_wall_type.value != WipeTowerWallType::wtwRib && plan.active.size() == demand.size()) {
        SkyLane::Params q                = p;
        q.allow_over_z                   = plan.over_z > 0;
        q.taper_min                      = std::max(8., min_lane * p.lanes);
        std::vector<SkyLane::Demand> dem = demand;
        for (size_t i = 0; i < dem.size(); ++i)
            dem[i].active = plan.active[i];
        const auto          t0      = std::chrono::steady_clock::now();
        const SkyLane::Plan tapered = SkyLane::pack_taper(dem, rt->depth, q);
        const double        ms      = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (tapered.ok)
            cell_depth = tapered.cell_depth;
        SKY_LOG("TAPER ok=" << tapered.ok << " full_depth=" << rt->depth << " min=" << q.taper_min << " ms=" << ms
                            << " over_z=" << tapered.over_z << " fill_mm3=" << tapered.fill_volume);
        float last = 0.f;
        for (size_t i = 0; i < cell_depth.size(); ++i)
            if (cell_depth[i] != last) {
                SKY_LOG("TAPER step from z=" << demand[i].z << " (demand " << i << ", li=" << demand[i].layer << "): cell depth "
                                             << cell_depth[i]);
                last = cell_depth[i];
            }
    }
    rt->xmin = xmin;
    rt->xmax = xmax;
    // Rib arms: same knobs as the classic tower ("Rib width", "Extra rib length"). The
    // width is capped so both arm walls leave from the tower's own sides.
    rt->rib     = cfg.wipe_tower_wall_type.value == WipeTowerWallType::wtwRib;
    rt->rib_len = std::max(0., cfg.wipe_tower_extra_rib_length.value);
    rt->rib_hw  = std::min(std::max(0., cfg.wipe_tower_rib_width.value) / 2.,
                           0.45 * std::min(xmax - xmin, rt->depth - p.line_w) / std::sqrt(2.));
    if (rt->rib_hw < p.line_w)
        rt->rib = false;
    rt->rib_fillet = cfg.wipe_tower_fillet_wall.value;
    // Cone: same apex angle and same X squeeze as the classic tower (and as the footprint
    // the slicer already reserves, Print::first_layer_wipe_tower_corners). The cone only exists
    // where it reaches one line past the fence line (the nozzle-cone clearance around the lanes).
    if (cfg.wipe_tower_wall_type.value == WipeTowerWallType::wtwCone && cfg.wipe_tower_cone_angle.value > 0.) {
        const double fence                = p.line_w + p.r_tip + p.max_pit * std::tan(p.cone_deg * M_PI / 180.); // = rib_g of place()
        const auto [cone_R, cone_x_scale] = WipeTower2::get_wipe_tower_cone_base(cfg.prime_tower_width.value + extra_width(cfg), rt->height,
                                                                                 rt->depth, cfg.wipe_tower_cone_angle.value);
        rt->cone                          = true;
        rt->cone_tan                      = std::tan(cfg.wipe_tower_cone_angle.value * M_PI / 360.);
        rt->cone_sx                       = std::max(1., cone_x_scale);
        rt->cone_min_b                    = rt->depth / 2 - p.line_w / 2 + fence + p.line_w;
        // the arcs must stand on the long sides: squeeze X further if the chord would be wider
        const double b0 = cone_R, d0 = rt->depth / 2 - p.line_w / 2, half = (xmax - xmin) / 2 - 2 * p.line_w;
        if (b0 > d0 && half > 0.) {
            const double chord = b0 * std::sqrt(1. - (d0 / b0) * (d0 / b0));
            rt->cone_sx        = std::max(rt->cone_sx, chord / half);
        }
        if (rt->cone_b(0.) <= 0.)
            rt->cone = false; // tower too deep / too short for this angle: plain rectangle
        SKY_LOG("CONE on=" << rt->cone << " angle=" << cfg.wipe_tower_cone_angle.value << " base_radius=" << cone_R
                           << " needs_more_than=" << rt->cone_min_b << " x_scale=" << rt->cone_sx << " tower_height=" << rt->height);
    }
    // Lane schedule for the emission: one segment per run of equal counts. Kept monotone in Z
    // (sublayer demands can be a hair below the previous one) with a running max.
    {
        float zmax = 0.f;
        for (size_t i = 0; i < demand.size() && i < plan.active.size(); ++i) {
            zmax = std::max(zmax, demand[i].z);
            if (rt->schedule.empty() || plan.active[i] != rt->schedule.back().second)
                rt->schedule.emplace_back(zmax, plan.active[i]);
            else
                rt->schedule.back().first = zmax;
        }
        zmax = 0.f;
        for (size_t i = 0; i < demand.size() && i < cell_depth.size(); ++i) {
            zmax = std::max(zmax, demand[i].z);
            if (rt->depth_sched.empty() || cell_depth[i] != rt->depth_sched.back().second)
                rt->depth_sched.emplace_back(zmax, cell_depth[i]);
            else
                rt->depth_sched.back().first = zmax;
        }
    }
    rt->used_filament.assign(rt->fil_area.size(), 0.f);
    rt->lanes = std::make_unique<SkyLane::Lanes>(rt->p, rt->depth);
    rt->H.assign(size_t(p.lanes), 0.);
    rt->lane_pass.assign(size_t(p.lanes), 0);
    rt->on_grid.assign(size_t(p.lanes), 0);
    rt->lane_x.assign(size_t(p.lanes), 1);
    m_rt    = rt;
    m_depth = float(rt->depth);
    SKY_LOG("PLAN lanes=" << p.lanes << " pit=" << p.max_pit << " purge_mm3=" << purge_volume << " planned_depth=" << plan.depth
                          << " depth(+15%)=" << rt->depth << " planned_passes=" << plan.passes.size()
                          << " planned_fill_mm3=" << plan.fill_volume << " demands=" << demand.size() << " over_z=" << plan.over_z
                          << " over_z_max=" << plan.over_z_max << " first_over=" << plan.first_over);
    SKY_LOG("SPEED profile=" << rt->profile_speeds << " first_layer=" << rt->first_speed << " purge_max=" << rt->purge_speed
                             << " ramp_mm3=" << rt->ramp << " filament_flow=[" <<
            [&] {
                std::ostringstream f;
                for (double v : rt->fil_flow)
                    f << " " << v;
                return f.str();
            }() << " ]");
    SKY_LOG("RIB on=" << rt->rib << " fillet=" << rt->rib_fillet << " half_width=" << rt->rib_hw << " length=" << rt->rib_len
                      << " tower_height=" << rt->height);
    {
        std::ostringstream sc;
        for (const auto& e : rt->schedule)
            sc << e.second << " lanes up to z=" << e.first << "; ";
        SKY_LOG("SCHEDULE " << sc.str());
        std::ostringstream sm;
        sm << "; SKYLANE plan: lanes=" << p.lanes << " depth=" << rt->depth << " planned_depth=" << plan.depth << " width=" << p.width
           << " prime_volume=" << purge_volume << " max_dip=" << p.max_pit << " demands=" << demand.size()
           << " purges=" << rt->planned_purges << " over_z=" << plan.over_z << "\n";
        sm << "; SKYLANE zones: " << sc.str() << "\n";
        if (!p.block_lane_ends.empty()) {
            sm << "; SKYLANE material packing: "
               << (p.block_rounds ? "shared rounds within each category" : "independent lanes within each category") << "\n";
            int first = 0;
            for (const auto& [category_id, block] : material_blocks) {
                const int end = p.block_lane_ends[block];
                sm << "; SKYLANE material block: category=" << category_id << " y0=" << rt->depth * first / p.lanes
                   << " y1=" << rt->depth * end / p.lanes << "\n";
                first = end;
            }
        }
        if (rt->depth_sched.empty())
            sm << "; SKYLANE taper: none\n";
        else {
            sm << "; SKYLANE taper:";
            float from = 0.f;
            for (const auto& e : rt->depth_sched) {
                sm << " z " << from << " to " << e.first << " = " << (e.second > 0.f ? double(e.second) : rt->depth) << " mm;";
                from = e.first;
            }
            sm << "\n";
        }
        rt->summary = sm.str();
    }
    return true;
}

void SkyLaneTower::begin_emission() const
{
    if (!m_rt || !m_rt->lanes)
        return;
    auto& rt = *m_rt;
    rt.lanes = std::make_unique<SkyLane::Lanes>(rt.p, rt.depth);
    rt.H.assign(size_t(rt.p.lanes), 0.);
    rt.lane_pass.assign(size_t(rt.p.lanes), 0);
    rt.on_grid.assign(size_t(rt.p.lanes), 0);
    rt.lane_x.assign(size_t(rt.p.lanes), 1);
    rt.brim_done     = false;
    rt.base_done     = false;
    rt.rib_top       = 0.;
    rt.band_wall_gap = rt.band_wall_transition = rt.band_wall_roots = false;
    rt.band_wall_entry                                              = Vec2d::Zero();
    rt.wall_top                                                     = 0.;
    rt.wall_ended                                                   = false;
    rt.placed                                                       = 0;
    rt.over_z_logged                                                = 0;
    rt.ramp_logged                                                  = 0;
    rt.deepest                                                      = 0.;
    rt.emitted_keys.clear();
    rt.emitted_purges = 0;
    rt.extra_purges   = 0;
    rt.outlines.clear();
    rt.used_filament.assign(rt.fil_area.size(), 0.f);
}

void SkyLaneTower::report() const
{
    if (!m_rt || !m_rt->lanes)
        return;
    const auto&        rt = *m_rt;
    const auto&        H  = rt.lanes->heights();
    std::ostringstream lanes;
    for (double h : H)
        lanes << h << " ";
    SKY_LOG("DONE emissions=" << rt.placed << " deepest_dip=" << rt.deepest << " over_z=" << rt.lanes->over_z()
                              << " over_z_max=" << rt.lanes->over_z_max() << " fill_mm3=" << rt.lanes->fill_volume()
                              << " depth=" << rt.depth << " lane_tops=[ " << lanes.str() << "]");
    SKY_LOG("PLAN_VS_EMISSION planned_purges=" << rt.planned_purges << " emitted_purges=" << rt.emitted_purges
                                               << " extra=" << rt.extra_purges
                                               << " planned_not_emitted=" << rt.planned_purges - (rt.emitted_purges - rt.extra_purges));
}

WipeTower::ToolChangeResult SkyLaneTower::place(const WipeTower::ToolChangeResult& src) const
{
    if (!m_rt || !m_rt->lanes)
        return src;
    Runtime&               rt    = *m_rt;
    const SkyLane::Params& p     = rt.p;
    const double           lw    = p.line_w;
    const double           z_off = rt.z_off;
    const double           xmin = rt.xmin, xmax = rt.xmax;
    size_t                 tool = size_t(std::max(src.new_tool, 0));
    // "Profile speeds" on: the flow ceiling is the incoming filament's "Max volumetric
    // speed", the purge speed ceiling is "Maximum wipe tower print speed", the first layer uses
    // "Initial layer speed", and every purge ramps up with the plastic laid (ramp_vol,
    // purge_flow_cap). Off: the fixed figures below, as before.
    const bool prof        = rt.profile_speeds;
    double     max_flow    = 15.;
    auto       select_tool = [&](size_t selected) {
        tool     = selected;
        max_flow = prof && tool < rt.fil_flow.size() && rt.fil_flow[tool] > 0. ? rt.fil_flow[tool] : 15.;
    };
    select_tool(tool);
    const double first_speed = prof ? rt.first_speed : 30.;
    double       ramp_vol    = 0.;  // mm3 laid by this toolchange so far, grid included
    double       ramp_secs   = 0.;  // seconds of it spent below the pass's own speed
    double       ramp_slow   = 0.;  // mm3 laid below the pass's own speed
    const double base_speed  = 80.; // mm/s
    // purge passes are limited by FLOW, not by base_speed: a 0.04 mm pass at 80 mm/s is
    // 1.6 mm3/s, and 45 mm3 spread that thin is 2.25 m of line (MIXED-FS-FD: 1 544 m of purge,
    // 5.5 h of a 7.6 h print, against 3.9 h on the Classic tower). The grid fills and the solid
    // base keep base_speed: they are structure.
    const double purge_vmax = prof ? rt.purge_speed : 250.; // mm/s

    const TcrInfo   info = inspect(src, rt.area(tool));
    SkyLane::Demand d;
    d.z              = src.print_z;
    d.layer_h        = src.layer_height;
    d.old_tool       = size_t(std::max(src.initial_tool, 0));
    d.new_tool       = tool;
    d.volume         = info.is_tc ? rt.purge + float(rt.ram(d.old_tool)) : 0.f; // same figure as the plan
    d.ramming_volume = info.is_tc ? float(rt.ram(d.old_tool)) : 0.f;
    d.active         = rt.active_at(d.z);
    d.depth          = rt.depth_at(d.z); // taper (applied by the packer when the next round opens)
    if (auto it = rt.material_visits.find(std::lround(d.z * 10000.)); it != rt.material_visits.end())
        d.present_blocks = it->second;
    const int idx = rt.placed++;
    if (d.volume > 0.f) { // emission vs plan
        ++rt.emitted_purges;
        const auto key = Runtime::purge_key(d);
        const auto it  = rt.planned_keys.find(key);
        if (++rt.emitted_keys[key] > (it == rt.planned_keys.end() ? 0 : it->second) && rt.extra_purges++ < 20)
            SKY_LOG("EXTRA emission " << idx << " z=" << d.z << " T" << d.old_tool << "->T" << d.new_tool
                                      << (it == rt.planned_keys.end() ? " (not in the plan)" : " (more times than planned)"));
    }
    const int                  over_before = rt.lanes->over_z();
    std::vector<SkyLane::Pass> passes;
    if (!rt.lanes->place(d, idx, passes))
        throw std::runtime_error("SkyLane cannot place this tower visit within the planned clearance; export stopped");
    if (rt.lanes->over_z() > over_before && rt.over_z_logged++ < 20)
        SKY_LOG("OVER_Z emission " << idx << " z=" << d.z << " T" << d.old_tool << "->T" << d.new_tool
                                   << " (a min pass above the nozzle: lanes already at its Z)");

    // Outgoing fills and all purge bands belong inside the tool-change block;
    // only incoming levelling fills may follow it.
    size_t n_wipe = 0;
    for (size_t k = 0; k < passes.size(); ++k)
        if (passes[k].demand >= 0 || (passes[k].tool == d.old_tool && d.old_tool != d.new_tool))
            n_wipe = k + 1;
    const SkyLane::Pass* lead  = passes.empty() ? nullptr : &passes.front();
    const float          new_z = lead ? float(lead->z_base + lead->h) : src.print_z;
    if (double(src.print_z) - new_z > rt.deepest)
        rt.deepest = double(src.print_z) - new_z;

    // fence line: how far from the tower a taller neighbour must stay (nozzle cone). The
    // ring stands there, and the rib arms are split there: the part beyond it ("far") may be
    // raised in a lanes zone, the part inside it ("root") only together with the merged cell.
    const double wall_off = lw / 2 + p.r_tip + p.max_pit * std::tan(p.cone_deg * M_PI / 180.);
    const double rib_g    = lw / 2 + wall_off; // tower outer bead centre -> fence line
    const double fx0 = xmin - rib_g, fx1 = xmax + rib_g, fy0 = lw / 2 - rib_g, fy1 = rt.depth - lw / 2 + rib_g;
    auto         rib_far_ext = [&](double z) { return rt.rib_ext(z) - rib_g * std::sqrt(2.); };
    // Is there anything of the arms beyond the fence line at height z? (the cap sticks out half a
    // rib width past the arm length: once forgotten, and the last stretch printed in the air)
    auto rib_far_ok = [&](double z) { return rib_far_ext(z) + rt.rib_hw >= lw; };
    // Cone: `butt` = the tower has buttresses (Rib arms or the cone) that follow the fence
    // rules. wall_loop = the rectangle (x0..y1) with the cone at height z; `off` grows the cone
    // with the rectangle (brim loops). cone_far = the arc of one side beyond the fence line.
    const bool   butt = rt.rib || rt.cone;
    const double ccx = (xmin + xmax) / 2, ccy = rt.depth / 2;
    const double cone_d    = rt.depth / 2 - lw / 2; // long axis -> the tower's outer bead
    const double fence_d   = cone_d + rib_g;        // long axis -> fence line
    auto         far_ok    = [&](double z) { return rt.cone ? rt.cone_b(z) > 0. : rib_far_ok(z); };
    auto         wall_loop = [&](double x0, double y0, double x1, double y1, double z, double off) {
        const double b = rt.cone_b(z);
        return cone_loop(x0, y0, x1, y1, b > 0. ? b / rt.cone_sx + off : 0., b > 0. ? b + off : 0.);
    };
    auto cone_far = [&](double z, int sgn) {
        const double b = rt.cone_b(z);
        return cone_arc(ccx, ccy, b / rt.cone_sx, b, sgn, fence_d);
    };
    // "Fillet wall" (Rib only): rounds the corners of a path before it is travelled to and printed.
    auto shape = [&](const std::vector<Vec2d>& pts, bool closed) { return rt.rib && rt.rib_fillet ? rib_round(pts, closed, 2.) : pts; };
    auto record_outline = [&](double z, const std::vector<Vec2d>& points, bool brim = false) {
        Polyline line;
        for (const auto& point : points)
            line.points.emplace_back(scale_(point.x()), scale_(point.y()));
        if (!line.points.empty() && line.points.front() != line.points.back())
            line.points.push_back(line.points.front());
        auto& outlines = rt.outlines[float(z)];
        if (outlines.empty())
            outlines.emplace_back();
        if (brim)
            outlines.push_back(std::move(line));
        else
            outlines.front() = std::move(line);
    };
    // Extrudes along pts (the nozzle already stands on pts.front(), at the right Z).
    auto emit_path = [&](std::ostringstream& o, const std::vector<Vec2d>& pts, bool closed, double e_mm, double feed, Vec2f& cur_xy) {
        if (e_mm > 0.)
            feed = std::min(feed, max_flow / (e_mm * rt.area(tool)) * 60.);
        Vec2d prev = pts.front();
        for (size_t k = 1; k < pts.size() + (closed ? 1 : 0); ++k) {
            const Vec2d& q = pts[k % pts.size()];
            o << std::setprecision(3) << "G1 X" << q.x() << " Y" << q.y() << " E" << std::setprecision(5) << (q - prev).norm() * e_mm
              << std::setprecision(0) << " F" << feed << "\n";
            prev = q;
        }
        cur_xy = Vec2f(float(prev.x()), float(prev.y()));
    };
    // Raises the far part of the four arms from rt.rib_top to `target`, in passes of <= hmax,
    // corner by corner (lift, travel, drop). Leaves the nozzle on the last arm, at `target`.
    auto raise_far = [&](std::ostringstream& o, double target, double& cur_z, Vec2f& cur_xy) {
        if (!butt || target - rt.rib_top < 0.01)
            return false;
        if (!far_ok(target)) {
            rt.rib_top = target;
            return false;
        } // tapered away up here
        const int    n    = std::max(1, int(std::ceil((target - rt.rib_top) / p.hmax - 1e-6)));
        const double h    = (target - rt.rib_top) / n;
        const double hi   = std::max({*std::max_element(rt.H.begin(), rt.H.end()), target, double(cur_z - z_off)});
        const double e_mm = lw * h / rt.area(tool);
        const double feed = std::min(60., max_flow / (lw * h)) * 60.;
        o << ";WIDTH:" << std::setprecision(3) << lw << "\n;HEIGHT:" << std::setprecision(4) << h << "\n";
        if (rt.cone) { // the two arcs beyond the fence line, one after the other
            for (int sgn : {-1, 1}) {
                o << std::setprecision(3) << "G1 Z" << hi + 0.2 + z_off << " F600\n";
                for (int k = 0; k < n; ++k) {
                    const double       top = rt.rib_top + h * (k + 1);
                    std::vector<Vec2d> pts = cone_far(top, sgn);
                    if (pts.size() < 2)
                        continue;
                    if (k % 2)
                        std::reverse(pts.begin(), pts.end());
                    o << std::setprecision(3) << "G1 X" << pts.front().x() << " Y" << pts.front().y() << " F12000\n";
                    o << "G1 Z" << top + z_off << " F600\n";
                    emit_path(o, pts, false, e_mm, feed, cur_xy);
                }
            }
        } else
            for (int i = 0; i < 4; ++i) {
                o << std::setprecision(3) << "G1 Z" << hi + 0.2 + z_off << " F600\n";
                for (int k = 0; k < n; ++k) {
                    const double       top = rt.rib_top + h * (k + 1);
                    const RibArm       a   = rib_arm(fx0, fy0, fx1, fy1, i, rt.rib_hw, rib_far_ext(top));
                    std::vector<Vec2d> pts = shape({a.p_in, a.t_in, a.t_out, a.p_out}, false);
                    if (k % 2)
                        std::reverse(pts.begin(), pts.end());
                    o << std::setprecision(3) << "G1 X" << pts.front().x() << " Y" << pts.front().y() << " F12000\n";
                    o << "G1 Z" << top + z_off << " F600\n";
                    emit_path(o, pts, false, e_mm, feed, cur_xy);
                }
            }
        rt.rib_top = target;
        cur_z      = target + z_off;
        return true;
    };

    // Alternate the first arrival's X side. Later merged bands may enter from
    // either Y end; material ownership is settled by the packer before emission.
    auto lane_rows = [&](const SkyLane::Pass& ps) -> std::pair<double, double> {
        if (ps.y1 - ps.y0 >= lw)
            return {ps.y0 + lw / 2, ps.y1 - lw / 2};
        double center = (ps.y0 + ps.y1) / 2;
        if (!p.block_lane_ends.empty() && !p.block_rounds) {
            // A short ram can occupy less than one bead of an independent
            // lane. Keep its centreline half a bead inside the lane edges.
            const double depth = rt.depth / p.lanes;
            center             = std::clamp(center, ps.lane * depth + lw / 2, (ps.lane + 1) * depth - lw / 2);
        }
        return {center, center};
    };
    auto pass_start = [&](const SkyLane::Pass& ps, int pc) {
        if (ps.merged) {
            double y0 = std::max(ps.y0, ps.cell_y0 + lw) + lw / 2;
            double y1 = std::min(ps.y1, ps.cell_y1 - lw) - lw / 2;
            return Vec2f(float(pc % 2 ? xmax - lw : xmin + lw), float(y1 < y0 ? (y0 + y1) / 2 : y0));
        }
        return Vec2f(float((pc / 2) % 2 ? xmax : xmin), float(lane_rows(ps).first));
    };
    const SkyLane::Pass* previous_pass = nullptr;

    auto emit_pass = [&](std::ostringstream& o, const SkyLane::Pass& ps, double& cur_z, Vec2f& cur_xy, bool& first) {
        const double         top        = ps.z_base + ps.h;
        const bool           is_fill    = ps.demand < 0;
        const size_t         next_index = size_t(&ps - passes.data()) + 1;
        const SkyLane::Pass* next_pass  = next_index < passes.size() ? &passes[next_index] : nullptr;
        if (ps.tool != tool)
            throw std::runtime_error("SkyLane material order differs from the tool-change sequence");
        o << "; SKYLANE BAND tool=" << ps.tool << " kind=" << (ps.ramming ? "ram" : (is_fill ? "fill" : "purge"))
          << " y0=" << std::setprecision(4) << ps.y0 << " y1=" << ps.y1 << " merged=" << ps.merged << " cell0=" << ps.cell_y0
          << " cell1=" << ps.cell_y1 << "\n";
        // fills: open grid; purges: solid, or spaced out to extrude exactly the requested volume
        const double spacing = is_fill ? (ps.solid ? lw : lw / ((ps.merged || ps.block_round) ? p.merged_density : p.fill_density)) :
                                         lw / std::clamp(ps.density, 0.1, 1.);
        if (ps.h < 0.02) { // sliver (first-layer top-up over a purge, levelling before a merge): nothing printable
            rt.H[size_t(ps.lane)] = std::max(rt.H[size_t(ps.lane)], top);
            return;
        }
        auto [y0, y1] = lane_rows(ps);
        double    xa = xmin, xb = xmax;
        const int pc = rt.lane_pass[size_t(ps.lane)]++;
        Vec2f     st = pass_start(ps, pc);
        if (!first && (ps.merged || ps.block_round)) {
            // A whole-tower round has an inner wall margin. A material block's
            // rows already have their clearance. Both can start at the nearest end.
            const double low   = ps.merged ? std::max(ps.y0, ps.cell_y0 + lw) + lw / 2 : y0;
            const double high  = ps.merged ? std::min(ps.y1, ps.cell_y1 - lw) - lw / 2 : y1;
            const double inset = ps.merged ? lw : 0.;
            st.x()             = float(cur_xy.x() > (xmin + xmax) / 2 ? xmax - inset : xmin + inset);
            st.y() = float(high < low ? (low + high) / 2 : (std::abs(cur_xy.y() - high) < std::abs(cur_xy.y() - low) ? high : low));
        }
        // Only an adjacent, unprinted band on the same merged plane permits a
        // level entry. All other repositioning retains the clearance lift.
        const double hi       = *std::max_element(rt.H.begin(), rt.H.end());
        const bool   adjacent = !first && ps.merged && previous_pass && previous_pass->merged &&
                              std::abs(previous_pass->z_base + previous_pass->h - top) < 1e-6 && std::abs(cur_z - (top + z_off)) < 1e-4 &&
                              std::max({hi, rt.rib_top, rt.wall_top}) <= top + 1e-6 && cur_xy.x() >= xmin - 1e-4 &&
                              cur_xy.x() <= xmax + 1e-4 &&
                              ((std::abs(previous_pass->y1 - ps.y0) < 1e-6 && std::abs(cur_xy.y() - ps.y0) < 1e-4) ||
                               (std::abs(previous_pass->y0 - ps.y1) < 1e-6 && std::abs(cur_xy.y() - ps.y1) < 1e-4));
        if (!first) {
            if (adjacent)
                o << "; SKYLANE adjacent band travel\n";
            else if (cur_z < hi + 0.2 + z_off)
                o << std::setprecision(3) << "G1 Z" << hi + 0.2 + z_off << " F600\n";
            o << std::setprecision(3) << "G1 X" << st.x() << " Y" << st.y() << " F12000\n";
        }
        o << std::setprecision(3) << "G1 Z" << top + z_off << " F600\n";
        o << ";HEIGHT:" << std::setprecision(4) << ps.h << "\n";
        o << ";WIDTH:" << std::setprecision(3) << lw << "\n";
        // first-layer base goes slow so it grips the bed
        double       speed     = std::min(ps.solid ? first_speed : (is_fill ? base_speed : purge_vmax), max_flow / (lw * ps.h));
        const double line_flow = ps.ramming && tool < rt.ram_flow.size() && rt.ram_flow[tool] > 0. ? rt.ram_flow[tool] : max_flow;
        if (ps.ramming)
            speed = std::min(250., line_flow / (lw * ps.h));
        if (prof && top <= rt.lanes->z0() + 1e-4)
            speed = std::min(speed, first_speed); // everything on the bed
        const double e_per_mm = lw * ps.h / rt.area(tool);
        // one straight line along X (axis 'X') or Y, from `from` to `to`, the other
        // coordinate as it stands. While the purge ramp is on, the line is cut in pieces of
        // ramp / 16 mm3 (1 mm at least), each at the speed its flow limit allows; from the piece
        // that reaches the pass's own speed on, the rest goes in one move. Fills: one move.
        auto purge_line = [&](std::ostringstream& o, char axis, double from, double to, double e_mm) {
            const double len = std::abs(to - from), dir = to < from ? -1. : 1.;
            const double mm3  = e_mm * rt.area(tool); // per mm of line
            double       done = 0.;
            do {
                double seg = len - done, v = mm3 > 0. ? std::min(speed, line_flow / mm3) : speed;
                if (!is_fill && !ps.ramming && rt.ramp > 0. && ramp_vol < rt.ramp && mm3 > 0.) {
                    v = std::min(v, purge_flow_cap(std::max(ramp_vol, 0.), max_flow, rt.ramp) / mm3);
                    if (v < speed - 1e-6) {
                        seg = std::min(seg, std::max(1., rt.ramp / 16. / mm3));
                        if (len - done - seg < 0.5)
                            seg = len - done; // no crumbs at the end of the line
                        ramp_secs += seg / v;
                        ramp_slow += seg * mm3;
                    }
                }
                done += seg;
                // the ramp counts ALL the plastic laid since the toolchange: when the open
                // grid goes first (84 of 131 toolchanges of WIPE-test01: ~10 mm3 at 80 mm/s) the
                // nozzle is at pressure by the time the purge starts, and ramping it then only
                // lost time. The grid itself is never slowed: it is where the cold start belongs.
                if (!ps.ramming)
                    ramp_vol += seg * mm3;
                o << std::setprecision(3) << "G1 " << axis << (len - done < 1e-6 ? to : from + dir * done) << " E" << std::setprecision(5)
                  << seg * e_mm << std::setprecision(0) << " F" << v * 60. << "\n";
            } while (len - done > 1e-6);
        };
        // Lines alternate X / Y on every pass of the lane (crossed grid) and the start corner turns
        // every two passes (pass_start): the post-toolchange drip lands in a different corner and
        // the next pass flattens it, instead of piling up in one corner.
        bool fx = st.x() > (xa + xb) / 2, fy = st.y() > (y0 + y1) / 2;
        // Merged cell = a classic tower layer: its own perimeter (the tower wall up here, at
        // the very height of the grid it encloses) and the lines, one bead inside.
        // Rib outlines belong to the first band of a shared level, so all four
        // arms use one already-active filament without another tool change.
        // Other wall types keep their adjacent band ownership.
        const bool   owns_wall = ps.merged && (!rt.rib || ps.wall);
        const double wfeed     = std::min(speed, 60.) * 60.;
        // the wall's centreline: the cell of the pass (tapered = narrower, centred; the cone
        // then leaves from the cell's own sides, like the classic tower's layer box)
        const double cy0 = (ps.merged ? ps.cell_y0 : 0.) + lw / 2, cy1 = (ps.merged ? ps.cell_y1 : rt.depth) - lw / 2;
        record_outline(top, rt.cone ? wall_loop(xa, cy0, xb, cy1, top, 0.) :
                                      shape(rib_loop(xa, cy0, xb, cy1, rt.rib ? rt.rib_hw : 0., rt.rib_ext(top)), true));
        if (ps.merged && ps.wall) {
            rt.band_wall_gap        = rt.wall_gap && info.is_tc;
            rt.band_wall_entry      = st.cast<double>();
            rt.band_wall_transition = butt && far_ok(top) && std::abs(rt.rib_top - ps.z_base) >= 1e-3;
            rt.band_wall_roots      = false;
        }
        const double low  = rt.rib || ps.y0 <= ps.cell_y0 + 1e-6 ? -std::numeric_limits<double>::infinity() : ps.y0;
        const double high = rt.rib || ps.y1 >= ps.cell_y1 - 1e-6 ? std::numeric_limits<double>::infinity() : ps.y1;

        std::vector<std::vector<Vec2d>> wall_pieces;
        if (owns_wall && !rt.band_wall_transition) {
            const auto loop = rt.cone ? wall_loop(xmin, cy0, xmax, cy1, top, 0.) :
                                        shape(rib_loop(xmin, cy0, xmax, cy1, rt.rib ? rt.rib_hw : 0., rt.rib_ext(top)), true);
            const auto open = rt.band_wall_gap ?
                                  open_loop_at(loop, rt.band_wall_entry, rt.band_wall_entry.x() > (xmin + xmax) / 2, 2.5 * lw, lw / 2) :
                                  std::vector<Vec2d>{};
            wall_pieces     = clip_wall_band(open.empty() ? loop : open, open.empty(), low, high);
        }
        auto emit_wall_piece = [&](std::vector<Vec2d> piece) {
            if ((piece.back() - cur_xy.cast<double>()).squaredNorm() < (piece.front() - cur_xy.cast<double>()).squaredNorm())
                std::reverse(piece.begin(), piece.end());
            o << "; SKYLANE WALL\n";
            o << std::setprecision(3) << "G1 X" << piece.front().x() << " Y" << piece.front().y() << " F12000\n";
            emit_path(o, piece, false, e_per_mm, wfeed, cur_xy);
            o << "; SKYLANE WALL_END\n";
        };
        // Use a purge row to cross between the two side walls. Printing both
        // sides after all rows would require an extra full-width travel.
        auto emit_near_wall = [&](Vec2f row_start) {
            if (wall_pieces.size() < 2 || (!is_fill && !ps.ramming && ramp_vol < rt.ramp))
                return;
            const bool right   = row_start.x() > (xmin + xmax) / 2;
            bool       emitted = false;
            for (size_t i = 0; i < wall_pieces.size();) {
                const auto& piece = wall_pieces[i];
                if (std::all_of(piece.begin(), piece.end(),
                                [&](const Vec2d& v) { return right ? v.x() >= xmax - 1e-6 : v.x() <= xmin + 1e-6; })) {
                    emit_wall_piece(std::move(wall_pieces[i]));
                    wall_pieces.erase(wall_pieces.begin() + i);
                    emitted = true;
                } else
                    ++i;
            }
            if (emitted) {
                o << std::setprecision(3) << "G1 X" << row_start.x() << " Y" << row_start.y() << " F12000\n";
                cur_xy = row_start;
            }
        };
        // Rib emits the whole outline once. For the other shapes, the first/
        // last bands also own the buttresses beyond the cell's ends.
        auto emit_wall = [&] {
            if (!owns_wall)
                return;
            auto emit_pieces = [&](std::vector<std::vector<Vec2d>> pieces) {
                while (!pieces.empty()) {
                    size_t best     = 0;
                    bool   reverse  = false;
                    double distance = std::numeric_limits<double>::infinity();
                    for (size_t i = 0; i < pieces.size(); ++i)
                        for (bool back : {false, true}) {
                            const double next = ((back ? pieces[i].back() : pieces[i].front()) - cur_xy.cast<double>()).squaredNorm();
                            if (next < distance) {
                                best     = i;
                                reverse  = back;
                                distance = next;
                            }
                        }
                    auto piece = std::move(pieces[best]);
                    pieces.erase(pieces.begin() + best);
                    if (reverse)
                        std::reverse(piece.begin(), piece.end());
                    if (pieces.empty() && next_pass && next_pass->merged && std::abs(next_pass->z_base + next_pass->h - top) < 1e-6) {
                        // Leave the last side at the shared boundary, so the
                        // next band can be entered without crossing this one.
                        const double boundary = std::abs(ps.y1 - next_pass->y0) < 1e-6 ? ps.y1 :
                                                std::abs(ps.y0 - next_pass->y1) < 1e-6 ? ps.y0 :
                                                                                         std::numeric_limits<double>::infinity();
                        if (std::abs(piece.front().y() - boundary) < 1e-6 && std::abs(piece.back().y() - boundary) >= 1e-6)
                            std::reverse(piece.begin(), piece.end());
                    }
                    o << std::setprecision(3) << "G1 X" << piece.front().x() << " Y" << piece.front().y() << " F12000\n";
                    emit_path(o, piece, false, e_per_mm, wfeed, cur_xy);
                }
            };
            auto wall_path = [&](const std::vector<Vec2d>& path, bool closed) { emit_pieces(clip_wall_band(path, closed, low, high)); };
            o << "; SKYLANE WALL\n";
            if (!rt.band_wall_transition) {
                emit_pieces(std::move(wall_pieces));
                if (butt && (rt.rib || ps.ends_round))
                    rt.rib_top = top;
            } else {
                // A return from independent lanes may need the distant supports
                // raised first. Keep those nozzle-clearance rules across all bands.
                if (ps.wall && top - rt.rib_top >= p.hmin - 1e-6) {
                    rt.band_wall_roots = raise_far(o, top, cur_z, cur_xy);
                    o << ";HEIGHT:" << std::setprecision(4) << ps.h << "\n";
                }
                o << std::setprecision(3) << "G1 Z" << top + z_off << " F600\n";
                if (rt.cone) {
                    wall_path({Vec2d(xmin, cy0), Vec2d(xmax, cy0), Vec2d(xmax, cy1), Vec2d(xmin, cy1)}, true);
                    const double b = rt.cone_b(top);
                    if (rt.band_wall_roots && b > 0.) {
                        const double a = b / rt.cone_sx, t0 = std::asin(std::min(1., (cy1 - cy0) / 2 / b)), tf = std::asin(fence_d / b);
                        for (int sgn : {-1, 1})
                            for (int end = 0; end < 2; ++end)
                                wall_path(end == 0 ? cone_span(ccx, ccy, a, b, sgn, M_PI - t0, M_PI - tf) :
                                                     cone_span(ccx, ccy, a, b, sgn, t0, tf),
                                          false);
                    }
                } else {
                    const double root = rt.band_wall_roots ? rib_g * std::sqrt(2.) : 0.;
                    for (int i = 0; i < 4; ++i) {
                        const RibArm       a    = rib_arm(xmin, cy0, xmax, cy1, i, rt.rib_hw, rt.rib_ext(top));
                        const RibArm       b    = rib_arm(xmin, cy0, xmax, cy1, (i + 1) % 4, rt.rib_hw, rt.rib_ext(top));
                        std::vector<Vec2d> side = {a.p_out, b.p_in};
                        if (root > 0.05) {
                            side.insert(side.begin(), a.p_out + a.u * root);
                            side.push_back(b.p_in + b.u * root);
                        }
                        wall_path(shape(side, false), false);
                    }
                }
            }
            o << "; SKYLANE WALL_END\n";
        };
        if (ps.merged) {
            xa += lw;
            xb -= lw;
            y0 = std::max(ps.y0, ps.cell_y0 + lw) + lw / 2;
            y1 = std::min(ps.y1, ps.cell_y1 - lw) - lw / 2;
            if (y1 < y0)
                y0 = y1 = (y0 + y1) / 2;
            fx = st.x() > (xa + xb) / 2;
            fy = st.y() > (y0 + y1) / 2;
            o << std::setprecision(3) << "G1 X" << (fx ? xb : xa) << " Y" << (fy ? y1 : y0) << " F12000\n";
        }
        int n = 0;
        // A purge strip of the merged cell runs along its long side (few, long lines); everything
        // else keeps alternating X / Y per pass (crossed grid).
        // the first pass of a lane after a merged stretch lands on the cell's open grid,
        // whose carrying lines run along Y: it must cross them (along X). Along Y it fell between
        // them (BIGTEST-adaptive: 9 mm lines of a 0.045 pass over a 1 mm void at every return to lanes).
        const bool on_grid         = !ps.merged && rt.on_grid[size_t(ps.lane)];
        const bool same_lane_plane = !ps.merged && std::abs(rt.H[size_t(ps.lane)] - top) < 1e-6;
        // The outgoing and incoming portions of one lane pass share the same
        // supporting layer. Splitting the material must not rotate the second
        // portion onto (rather than across) the supporting grid below it.
        const bool lane_along_x = on_grid || (same_lane_plane ? !rt.lane_x[size_t(ps.lane)] : rt.lane_x[size_t(ps.lane)]);
        // a lane pass always crosses the one below it (lane_x), not "even passes along X":
        // after the forced X pass on the grid the parity could ask for X again, and over a sparse
        // fill (2 lines in a 3.3 mm lane) the purge lines ran between them, 35 mm over nothing
        // (FSFD_stairs_multipass, z=12.47).
        const bool along_x = y1 - y0 < lw || ps.block_round ? true :
                             (ps.merged && !is_fill)        ? (xb - xa) >= (y1 - y0) :
                             ps.merged                      ? (pc % 2 == 0) :
                                                              lane_along_x;
        // A purge pass extrudes the volume the packer counted for it (W x band x h x density):
        // the lines lose half a width at each end and one bead to the wall, which left a strip
        // 7-13 % short. Small flow correction, never on the structure (fills).
        auto purge_scale = [&](double total_len) {
            if (is_fill || total_len <= 0.)
                return 1.;
            const double want = p.width * (ps.y1 - ps.y0) * ps.h * ps.density;
            return want / (total_len * lw * ps.h);
        };
        bool crossed_fill = false;
        auto lines        = [&](bool along, double spacing) {
            if (along) {
                // lines along X, stepping in Y
                bool at_b = fx;
                // edge to edge: the step is stretched so the last line lands exactly on the far edge
                // (a floor() left up to one line width uncovered there — the wall then had no base)
                // (a purge rounds the line count UP — a 1.7 mm strip with floor() got 2 lines
                // 1.2 mm apart, a 0.7 mm slot the next round's line fell into; purge_scale trims the flow.)
                const int ny = std::max(y1 - y0 < 1e-6 ? 0 : 1,
                                        is_fill ? int(std::floor((y1 - y0) / spacing + 1e-6)) : int(std::ceil((y1 - y0) / spacing - 1e-6)));
                const double sy = ny > 0 ? (y1 - y0) / ny : 0.;
                const double ks = purge_scale((ny + 1) * (xb - xa));
                for (int j = 0; j <= ny; ++j, ++n) {
                    const double y = fy ? y1 - j * sy : y0 + j * sy;
                    if (n > 0)
                        o << std::setprecision(3) << "G1 Y" << y << " F12000\n";
                    cur_xy = Vec2f(float(at_b ? xb : xa), float(y));
                    if (j == ny)
                        emit_near_wall(cur_xy);
                    const double xe = at_b ? xa : xb;
                    purge_line(o, 'X', at_b ? xb : xa, xe, e_per_mm * ks);
                    at_b   = !at_b;
                    cur_xy = Vec2f(float(xe), float(y));
                }
            } else {
                // lines along Y, stepping in X
                bool         at_1 = fy;
                const int    nx   = std::max(1,
                                        is_fill ? int(std::floor((xb - xa) / spacing + 1e-6)) : int(std::ceil((xb - xa) / spacing - 1e-6)));
                const double sx   = (xb - xa) / nx;
                const double ks   = purge_scale((nx + 1) * (y1 - y0));
                for (int j = 0; j <= nx; ++j, ++n) {
                    const double x = fx ? xb - j * sx : xa + j * sx;
                    if (n > 0)
                        o << std::setprecision(3) << "G1 X" << x << " F12000\n";
                    cur_xy = Vec2f(float(x), float(at_1 ? y1 : y0));
                    if (j == 0)
                        emit_near_wall(cur_xy);
                    const double ye = at_1 ? y0 : y1;
                    purge_line(o, 'Y', at_1 ? y1 : y0, ye, e_per_mm * ks);
                    at_1   = !at_1;
                    cur_xy = Vec2f(float(x), float(ye));
                }
            }
        };
        // the open grid of the merged cell: the purge strips of the next round all run
        // along X, so the grid lines that carry them run along Y at the full grid spacing (bridges
        // of ~4.5 mm, as designed); a few lines along X (every third spacing) tie those fins
        // together. (f: a grid that happened to run along X left 29 mm strips over a void; with
        // half the lines each way the bridges were 9.5 mm.)
        if ((ps.merged || ps.block_round) && is_fill && !ps.solid) {
            lines(false, spacing);
            // The ends of these Y fins already meet the neighbouring purge
            // rows or the cell wall. Add ties only inside the gap, at no more
            // than three grid spacings, rather than duplicating both borders.
            const int ties = std::max(0, int(std::ceil((y1 - y0) / (3. * spacing) - 1e-6)) - 1);
            for (int j = 1; j <= ties; ++j) {
                const double y          = y0 + (y1 - y0) * j / (ties + 1);
                const bool   from_right = j % 2 == 0;
                o << std::setprecision(3) << "G1 X" << (from_right ? xb : xa) << " Y" << y << " F12000\n";
                purge_line(o, 'X', from_right ? xb : xa, from_right ? xa : xb, e_per_mm);
                cur_xy = Vec2f(float(from_right ? xa : xb), float(y));
            }
        } else if (!ps.merged && is_fill && !ps.solid && !on_grid) {
            // a lane's sparse fill is a crossed grid too (both ways at twice the spacing,
            // same material): whatever lands on it next finds lines to cross. With lines one way
            // only, the strips of the merged cell (always along X) fell between the X lines of the
            // fills that level the lanes before a merge (FSFD_stairs_multipass, z=12.85).
            lines(false, 2. * spacing);
            o << std::setprecision(3) << "G1 X" << xa << " Y" << y0 << " F12000\n";
            fx = fy = false;
            n       = 0;
            lines(true, 2. * spacing);
            crossed_fill = true;
        } else
            lines(along_x, spacing);
        // Finish any remaining wall pieces on this side of the completed band.
        emit_wall();
        cur_z = top + z_off;
        if (ps.merged)
            for (double& h : rt.H)
                h = std::max(h, top);
        else
            rt.H[size_t(ps.lane)] = std::max(rt.H[size_t(ps.lane)], top);
        if (!ps.merged) {
            rt.on_grid[size_t(ps.lane)] = 0;
            // on a crossed fill the next pass runs along X: it crosses the Y lines every two
            // spacings (along Y it would only rest on the X lines at the band edges)
            rt.lane_x[size_t(ps.lane)] = crossed_fill || !lane_along_x;
        } else if (is_fill && !ps.solid) {                                        // the grid covers [ps.y0, ps.y1]: every lane it reaches
            const double d = rt.lanes->depth() / rt.p.lanes, g0 = rt.lanes->y0(); // the lanes live in the (tapered) cell
            for (size_t c = 0; c < rt.on_grid.size(); ++c)
                if (std::min(ps.y1, g0 + (c + 1) * d) - std::max(ps.y0, g0 + c * d) > lw)
                    rt.on_grid[c] = 1;
        }
        o << "; SKYLANE BAND_END\n";
        first         = false;
        previous_pass = &ps;
    };

    // Brim around the SkyLane footprint, printed by the emission that closes the first-layer base,
    // BEFORE any of its passes: once a lane rises above the first layer the tip would scrape it.
    auto emit_brim = [&](std::ostringstream& o, double& cur_z, Vec2f& cur_xy, bool& first) {
        if (!rt.lanes->base_now() || rt.base_done || !(rt.brim > 0. || butt))
            return;
        {
            const double z0    = rt.lanes->z0();
            const int    loops = rt.brim > 0. ? std::max(1, int(std::lround(rt.brim / lw))) : 0;
            const double e_mm  = lw * z0 / rt.area(tool);
            const double bx0 = xmin, bx1 = xmax, by0 = lw / 2, by1 = rt.depth - lw / 2; // tower outer bead
            const double ext0 = rt.rib_ext(z0);
            o << "; WIPE_TOWER_BRIM_START\n;TYPE:Prime tower\n;WIDTH:" << std::setprecision(3) << lw << "\n;HEIGHT:" << std::setprecision(4)
              << z0 << "\n";
            const double hi = *std::max_element(rt.H.begin(), rt.H.end());
            const double zs = std::max(hi, z0) + 0.2 + z_off;
            o << std::setprecision(3) << "G1 Z" << zs << " F600\n";
            for (int l = 0; l < loops; ++l) {
                // Centre-to-centre from the tower's outer bead (x = xmin / xmax, y = lw/2 /
                // depth - lw/2): 0.9 lw for the first loop = glued with a 10 % overlap. (it
                // was measured from the bead's EDGE, which left a 0.4 lw gap — seen in the preview.)
                // with Rib the loops go round the arms too (the outline offset by `off`).
                const double             off  = lw * (0.9 + l);
                const std::vector<Vec2d> loop = rt.cone ? wall_loop(bx0 - off, by0 - off, bx1 + off, by1 + off, z0, off) :
                                                          shape(rib_loop(bx0 - off, by0 - off, bx1 + off, by1 + off,
                                                                         rt.rib ? rt.rib_hw + off : 0., ext0 + off - off * std::sqrt(2.)),
                                                                true);
                o << std::setprecision(3) << "G1 X" << loop.front().x() << " Y" << loop.front().y() << " F12000\n";
                record_outline(z0, loop, true);
                if (l == 0)
                    o << "G1 Z" << z0 + z_off << " F600\n";
                emit_path(o, loop, true, e_mm, first_speed * 60., cur_xy);
            }
            if (rt.brim > 0.)
                rt.brim_done = true;
            // Cone: first layer of the two arcs, and the bulge under them closed solid
            // (lines along X, from the tower side outwards), like the classic cone's base.
            if (const double b = rt.cone_b(z0); b > 0.) {
                const double a = b / rt.cone_sx;
                for (int sgn : {-1, 1}) {
                    o << std::setprecision(3) << "G1 Z" << zs << " F600\n";
                    bool at_right = false;
                    if (z0 - rt.rib_top >= 0.01) { // (a merged pass on the first layer already laid part of the wall)
                        const std::vector<Vec2d> arc = cone_arc(ccx, ccy, a, b, sgn, cone_d + lw / 2);
                        o << "G1 X" << arc.front().x() << " Y" << arc.front().y() << " F12000\n";
                        o << "G1 Z" << z0 + z_off << " F600\n";
                        emit_path(o, arc, false, lw * (z0 - rt.rib_top) / rt.area(tool), first_speed * 60., cur_xy);
                        at_right = true;
                    }
                    bool down = false;
                    for (double d = cone_d + lw; d < b - lw; d += lw) {
                        const double half = a * std::sqrt(1. - (d / b) * (d / b)) - lw; // one bead inside the arc
                        if (half < lw / 2)
                            break;
                        const double y = ccy + sgn * d, xs = at_right ? ccx + half : ccx - half, xe = at_right ? ccx - half : ccx + half;
                        o << std::setprecision(3) << "G1 X" << xs << " Y" << y << " F12000\n";
                        if (!down) {
                            o << "G1 Z" << z0 + z_off << " F600\n";
                            down = true;
                        }
                        o << "G1 X" << xe << " E" << std::setprecision(5) << 2 * half * e_mm << std::setprecision(0) << " F"
                          << std::min(first_speed, max_flow / (e_mm * rt.area(tool))) * 60. << "\n";
                        cur_xy   = Vec2f(float(xe), float(y));
                        at_right = !at_right;
                    }
                }
            }
            if (rt.rib)
                // First layer of the arm walls themselves (the solid base only covers the footprint).
                for (int i = 0; i < 4; ++i) {
                    // (a merged pass printed on the first layer already laid part of it: only the rest)
                    if (z0 - rt.rib_top < 0.01)
                        break;
                    const RibArm             a   = rib_arm(bx0, by0, bx1, by1, i, rt.rib_hw, ext0);
                    const std::vector<Vec2d> pts = shape({a.p_in + a.u * (lw / 2), a.t_in, a.t_out, a.p_out + a.u * (lw / 2)}, false);
                    o << std::setprecision(3) << "G1 Z" << zs << " F600\n";
                    o << "G1 X" << pts.front().x() << " Y" << pts.front().y() << " F12000\n";
                    o << "G1 Z" << z0 + z_off << " F600\n";
                    emit_path(o, pts, false, lw * (z0 - rt.rib_top) / rt.area(tool), first_speed * 60., cur_xy);
                }
            cur_z = z0 + z_off;
            o << "; WIPE_TOWER_BRIM_END\n";
            rt.base_done = true;
            rt.rib_top   = std::max(rt.rib_top, z0);
            rt.wall_top  = z0; // the base wall stands on the first brim loop
        }
        first = false;
    };

    std::ostringstream o;
    if (idx == 0)
        o << rt.summary;
    o << std::fixed;
    double        cur_z  = new_z + z_off;
    Vec2f         cur_xy = lead ? pass_start(*lead, rt.lane_pass[size_t(lead->lane)]) : src.start_pos;
    const Vec2f   start  = cur_xy;
    bool          first  = true;
    std::string   wipe_txt;
    OutgoingBlock outgoing;
    if (info.is_tc) {
        std::ostringstream old, incoming;
        old << std::fixed;
        incoming << std::fixed;
        size_t k = 0;
        select_tool(d.old_tool);
        if (n_wipe > 0)
            emit_brim(old, cur_z, cur_xy, first);
        // Fills that close the outgoing colour's region belong before the
        // switch too; never recolour their lines with the incoming filament.
        for (; k < n_wipe && passes[k].tool == d.old_tool && d.old_tool != d.new_tool; ++k)
            emit_pass(old, passes[k], cur_z, cur_xy, first);
        outgoing.gcode   = old.str();
        outgoing.printed = !outgoing.gcode.empty();
        outgoing.end     = cur_xy;
        outgoing.z       = cur_z;
        select_tool(d.new_tool);
        ramp_vol = 0.;
        for (; k < n_wipe; ++k)
            emit_pass(incoming, passes[k], cur_z, cur_xy, first);
        wipe_txt = incoming.str();
    }
    select_tool(d.new_tool);

    enum class Sec { Head, Unload, Wipe, Tail, After } sec = info.is_tc ? Sec::Head : Sec::After;
    if (info.is_tc) {
        for (const std::string& l : split_lines(src.gcode)) {
            if (sec == Sec::Head) {
                if (starts(l, ";HEIGHT:")) {
                    o << ";HEIGHT:" << std::setprecision(4) << (lead ? lead->h : src.layer_height) << "\n";
                    continue;
                }
                // FullSpectrum keeps unloading in a sidecar, so its main block may
                // go directly from START to the tool-change placeholder.
                if (has(l, "[change_filament_gcode]")) {
                    if (outgoing.printed)
                        o << "; SKYLANE outgoing\n" << outgoing.gcode;
                    o << l << "\n";
                    sec = Sec::Unload;
                    continue;
                }
                if (is_xy_move(l))
                    continue;
                o << l << "\n";
                if (has(l, "CP TOOLCHANGE UNLOAD"))
                    sec = Sec::Unload;
            } else if (sec == Sec::Unload) {
                if (has(l, "CP TOOLCHANGE WIPE")) {
                    o << l << "\n" << wipe_txt;
                    sec = Sec::Wipe;
                    continue;
                }
                if (outgoing.printed && has(l, "[change_filament_gcode]"))
                    o << "; SKYLANE outgoing\n" << outgoing.gcode;            // before the tool leaves
                if (has(l, WipeTower2::never_skip_tag().c_str())) {           // tool-change position: keep, but in the lane
                    const Vec2f at = outgoing.printed ? outgoing.end : start; // (the incoming tool starts where the ramming ended)
                    o << std::setprecision(5) << "G1 X" << at.x() << " Y" << at.y() << WipeTower2::never_skip_tag() << "\n";
                    continue;
                }
                if (is_xy_move(l))
                    continue; // ramming lines belonged to the old shelf
                o << l << "\n";
            } else if (sec == Sec::Wipe) {
                if (has(l, "WIPE_TOWER_END")) {
                    o << l << "\n";
                    sec = Sec::Tail;
                }
            } else if (sec == Sec::Tail) {
                if (is_xy_move(l))
                    continue;
                o << l << "\n";
                if (has(l, "CP TOOLCHANGE END"))
                    sec = Sec::After;
            }
            // Sec::After: old wall / grid / brim → dropped
        }
    }
    // levelling fills after the toolchange block (a non-TC visit only has fills)
    bool any_fill = false;
    for (size_t k = info.is_tc ? n_wipe : 0; k < passes.size(); ++k) {
        if (!any_fill) {
            o << "; SKYLANE fill\n;TYPE:Prime tower\n; WIPE_TOWER_START\n";
            any_fill = true;
            emit_brim(o, cur_z, cur_xy, first);
        }
        emit_pass(o, passes[k], cur_z, cur_xy, first);
    }
    if (any_fill)
        o << "; WIPE_TOWER_END\n";

    // Base wall: one ring around the tower, standing on the brim. The separate
    // material blocks can share a fence built in taller steps. Lanes dip up to max_pit below the layer,
    // and a ring touching them would be a taller neighbour the tip scrapes (1111 scrapes
    // in the first print-ready G-code). So it keeps the nozzle-cone clearance; the brim under it
    // (whose first loop IS glued to the tower) ties ring, brim and tower into one piece.
    // Where the lanes are merged into one cell (simple zone) the ring stops: every cell pass
    // prints its own perimeter, glued to its grid, and that is the wall from there to the top.
    bool wall_printed = false;
    {
        const double zl = src.print_z;
        // The ring only exists from the bed up to the first merged zone (there the cell's own
        // perimeter is the wall). A lanes zone higher up gets no ring: it would start in mid-air.
        if (rt.lanes->merged())
            rt.wall_ended = true;
        const bool   partitioned = !p.block_lane_ends.empty();
        double       target = zl, step = p.hmax;
        const double wall_end       = std::min(rt.wall_h, rt.height);
        const bool   building_fence = !rt.wall_ended && rt.brim_done && rt.brim >= wall_off + lw &&
                                    (partitioned ? wall_end - rt.wall_top >= p.hmin - 1e-6 : rt.wall_top < rt.wall_h - 1e-6);
        if (building_fence && partitioned) {
            // The detached fence supports itself. Use the available bead height
            // and keep at least half a bead of overlap on a shrinking rib or cone.
            const double slope = rt.rib ? rt.rib_len / rt.height : rt.cone_tan;
            const double limit = slope > 0. ? std::clamp(lw / (2. * slope), p.hmin, p.hmax) : p.hmax;
            const double base = rt.lanes->z0(), span = wall_end - base;
            step = std::max(p.hmin, span / std::max(1., std::ceil(span / limit - 1e-6)));
            // Equal steps reach the chosen height without a thin last pass.
            target = std::min(wall_end, base + std::floor((zl - base + 1e-6) / step) * step);
        }
        if (building_fence && target - rt.wall_top >= p.hmin - 1e-6) {
            // with Rib the ring goes round the arms (same arms as the merged cell's
            // perimeter beyond the fence line, so one stands on the other across zones).
            auto ring = [&](double top) {
                return rt.cone ? wall_loop(fx0, fy0, fx1, fy1, top, 0.) // the fence rectangle + the arcs beyond it
                                 :
                                 shape(rib_loop(fx0, fy0, fx1, fy1, rt.rib ? rt.rib_hw : 0., rib_far_ext(top)), true);
            };
            const double hi = *std::max_element(rt.H.begin(), rt.H.end());
            o << "; SKYLANE base wall\n;TYPE:Prime tower\n; WIPE_TOWER_START\n;WIDTH:" << std::setprecision(3) << lw << "\n";
            o << std::setprecision(3) << "G1 Z" << std::max(hi, zl) + 0.2 + z_off << " F600\n";
            Vec2d ring_start(fx0, fy0);
            while (target - rt.wall_top >= p.hmin - 1e-6) {
                const double             top  = std::min(target, rt.wall_top + step);
                const double             h    = top - rt.wall_top;
                const double             e_mm = lw * h / rt.area(tool);
                const std::vector<Vec2d> loop = ring(top);
                ring_start                    = loop.front();
                o << std::setprecision(3) << "G1 X" << ring_start.x() << " Y" << ring_start.y() << " F12000\n";
                o << "G1 Z" << top + z_off << " F600\n;HEIGHT:" << std::setprecision(4) << h << "\n";
                emit_path(o, loop, true, e_mm, 2400., cur_xy);
                rt.wall_top = top;
                cur_z       = top + z_off;
            }
            if (butt)
                rt.rib_top = rt.wall_top;
            // step off the ring (outwards, over the brim): if this TCR printed on a plane above
            // the object layer, the dispatch then comes back DOWN right here.
            const Vec2d off_pt = rt.rib ? Vec2d(ring_start.x(), ring_start.y() - 2 * lw) : Vec2d(fx0 - 2 * lw, fy0 - 2 * lw);
            o << std::setprecision(3) << "G1 X" << off_pt.x() << " Y" << off_pt.y() << " F12000\n";
            cur_xy = Vec2f(float(off_pt.x()), float(off_pt.y()));
            o << "; WIPE_TOWER_END\n";
            wall_printed = true;
        } else if (!(partitioned && building_fence) && butt && rt.base_done && !rt.lanes->merged() &&
                   zl - rt.rib_top >= p.merged_reserve - 1e-6 && far_ok(zl)) {
            // lanes zone without ring: only the far part of the arms goes up (the roots
            // would be a taller neighbour of the lanes). The arms may lag up to merged_reserve.
            o << "; SKYLANE rib\n;TYPE:Prime tower\n; WIPE_TOWER_START\n";
            raise_far(o, zl, cur_z, cur_xy);
            // step off, beyond the tip of the last arm (same reason as the ring's step off)
            const RibArm a = rib_arm(fx0, fy0, fx1, fy1, 3, rt.rib_hw, rib_far_ext(zl));
            // clear of the arm: the dispatch may drop here (Cone: outwards from the end of the last arc)
            const Vec2d cone_end(cur_xy.x(), cur_xy.y());
            const Vec2d off_pt = rt.cone ? Vec2d(cone_end + (cone_end - Vec2d(ccx, ccy)).normalized() * (4 * lw)) :
                                           Vec2d((a.t_in + a.t_out) / 2. + a.u * (4 * lw));
            o << std::setprecision(3) << "G1 X" << off_pt.x() << " Y" << off_pt.y() << " F12000\n";
            cur_xy = Vec2f(float(off_pt.x()), float(off_pt.y()));
            o << "; WIPE_TOWER_END\n";
            wall_printed = true;
        }
    }
    if (!info.is_tc && passes.empty() && cur_z == new_z + z_off)
        o << "; SKYLANE visit (nothing to deposit)\n";
    // leave Z where the dispatch believes it is (the TCR Z), never below a lane we could cross
    // After the wall the nozzle stays up (it sits right above the ring it just printed): the
    // dispatch then travels to the object layer, which is never below the ring top.
    // and 0.2 ABOVE the highest thing of the tower (lanes, or the ring / arms just printed):
    // a round fills the cell up to the nozzle, so leaving at that very height dragged the tip over
    // the fresh purge and its drips (18 of 692 exits under 0.15 mm in BIGTEST-ADAPT-SMALL). The
    // lift is vertical and inside the block; the dispatch reads this last Z (append_tcr2)
    // and its own ramped Z hop starts from here.
    {
        const double hi = *std::max_element(rt.H.begin(), rt.H.end());
        // (the arms / ring are not in rt.H: in a lanes zone their far part runs ahead of the lanes)
        const double top   = std::max({double(new_z), hi, butt ? rt.rib_top : 0., rt.wall_top, wall_printed ? cur_z - z_off : 0.});
        const double leave = top + 0.2 + z_off;
        if (std::abs(cur_z - leave) > 1e-4)
            o << std::setprecision(3) << "G1 Z" << leave << " F600\n";
    }

    WipeTower::ToolChangeResult t = src;
    t.gcode                       = o.str();
    t.skylane_z                   = new_z; // print_z / layer_height stay nominal: they are lookup keys
    // a ramming longer than the first pass ends on a later one, which may be higher (SLUG,
    // 5 mm3 over a short strip: 238 toolchanges). The incoming tool arrives where the ramming
    // ended, so it must arrive at THAT pass's Z: at the lead's it came down 0.12 mm into the
    // plastic just rammed. (The outgoing tool arrives a little high and the ramming sets its own Z.)
    if (outgoing.printed && !std::isnan(outgoing.z))
        t.skylane_z = std::max(new_z, float(outgoing.z - z_off));
    t.start_pos             = start;
    t.tool_change_start_pos = outgoing.printed ? outgoing.end : start;
    t.nozzle_change_result  = {};
    t.purge_volume          = info.is_tc ? float(rt.purge) : 0.f;
    double final_z          = new_z + z_off;
    GCodeProcessor::get_last_z_from_gcode(t.gcode, final_z);
    t.skylane_safe_z = float(final_z - z_off);
    t.skylane_ram    = outgoing.printed;
    // dry park (append_tcr2): the outgoing tool wipes from the start point towards the
    // middle of the first pass's band, i.e. over what the incoming tool purges right after.
    if (info.is_tc && lead) {
        const Vec2f mid(float((xmin + xmax) / 2), float((lead->y0 + lead->y1) / 2));
        const Vec2f v = mid - start;
        if (const float n = v.norm(); n > 1.f) {
            t.skylane_park     = start + v / n * std::min(5.f, 0.9f * n);
            t.skylane_has_park = true;
        }
    }
    t.end_pos = cur_xy;
    t.extrusions.clear();
    t.wipe_path.clear();
    // Rebuild per-pass preview positions and material usage from the rewritten block.
    Vec2f  preview_pos = start;
    double preview_z = new_z + z_off, preview_h = src.layer_height, preview_w = lw, feed = 12000.;
    size_t preview_tool = d.old_tool;
    t.elapsed_time      = 0.f;
    t.extrusions.emplace_back(preview_pos, 0.f, unsigned(preview_tool));
    for (const auto& line : split_lines(t.gcode)) {
        if (has(line, "[change_filament_gcode]"))
            preview_tool = tool;
        if (starts(line, ";HEIGHT:"))
            preview_h = to_double(std::string_view(line).substr(8), preview_h);
        if (starts(line, ";WIDTH:"))
            preview_w = to_double(std::string_view(line).substr(7), preview_w);
        if (!starts(line, "G1 "))
            continue;
        preview_z = num_after(line, 'Z', preview_z);
        feed      = num_after(line, 'F', feed);
        const Vec2f  next(float(num_after(line, 'X', preview_pos.x())), float(num_after(line, 'Y', preview_pos.y())));
        const double e = num_after(line, 'E', 0.);
        if (e > 0.)
            rt.used_filament.at(preview_tool) += float(e);
        if (is_xy_move(line)) {
            t.elapsed_time += float((next - preview_pos).norm() / std::max(feed / 60., 1e-6));
            t.extrusions.emplace_back(next, e > 0. ? float(preview_w) : 0.f, unsigned(preview_tool), float(preview_z - z_off),
                                      float(preview_h));
        }
        preview_pos = next;
    }

    if (rt.ramp > 0. && d.volume > 0.f && rt.ramp_logged++ < 20)
        SKY_LOG("RAMP emission " << idx << " z=" << d.z << " T" << d.old_tool << "->T" << d.new_tool << " laid_mm3=" << ramp_vol
                                 << " below_top_mm3=" << ramp_slow << " below_top_s=" << ramp_secs << " flow_max=" << max_flow);
    if (idx % 50 == 0 || rt.lanes->base_now())
        SKY_LOG("EMIT " << idx << " z=" << d.z << " lane_z=" << new_z << " passes=" << passes.size() << " deepest_so_far=" << rt.deepest
                        << " over_z=" << rt.lanes->over_z());
    return t;
}

void SkyLaneTower::preview(std::vector<std::vector<WipeTower::ToolChangeResult>>& nominal,
                           std::vector<std::vector<WipeTower::ToolChangeResult>>& local)
{
    begin_emission();
    m_bounds = BoundingBoxf();
    for (size_t layer = 0; layer < nominal.size(); ++layer) {
        for (auto* changes : {layer < local.size() ? &local[layer] : nullptr, &nominal[layer]}) {
            if (changes == nullptr)
                continue;
            for (auto& original : *changes) {
                auto placed         = place(original);
                original.extrusions = std::move(placed.extrusions);
                for (size_t i = 1; i < original.extrusions.size(); ++i) {
                    const auto& e = original.extrusions[i];
                    if (e.width <= 0.)
                        continue;
                    const Vec2d margin(e.width / 2., e.width / 2.);
                    for (const auto& point : {original.extrusions[i - 1].pos, e.pos}) {
                        m_bounds.merge(point.cast<double>() - margin);
                        m_bounds.merge(point.cast<double>() + margin);
                    }
                }
            }
        }
    }
    m_outlines      = m_rt->outlines;
    m_used_filament = m_rt->used_filament;
    begin_emission();
}

// Is SkyLane the tower of this print?
bool SkyLaneTower::enabled(const PrintConfig& config)
{
    return config.enable_prime_tower.value && config.enable_skylane.value && !config.single_extruder_multi_material.value;
}

// "Dry park": with no ramming the outgoing tool has nothing left to print at the tower, and
// un-retracting it on arrival only pushed a drop out of a nozzle standing still, right before the
// tool change retraction. It arrives retracted, goes down into the lane as it is, and the tool
// change retraction wipes along tcr.skylane_park with the filament's own "Wipe while retracting"
// / "Wipe distance". Flexible filaments keep the stock sequence.
bool SkyLaneTower::dry_arrival(
    const PrintConfig& config, GCodeWriter& writer, const WipeTower::ToolChangeResult& tcr, int new_extruder_id, bool needs_toolchange)
{
    auto is_flex = [&](size_t tool) {
        const auto& types = config.filament_type.values;
        if (tool >= types.size())
            return false;
        const std::string& t = types[tool];
        return t.find("TPU") != std::string::npos || t.find("TPE") != std::string::npos || t.find("FLEX") != std::string::npos;
    };
    return !tcr.priming && tcr.skylane_z >= 0.f && new_extruder_id >= 0 && needs_toolchange &&
           !tcr.skylane_ram && // a tool that rams must arrive primed
           writer.extruder() != nullptr && !is_flex(writer.extruder()->id());
}

} // namespace Slic3r
