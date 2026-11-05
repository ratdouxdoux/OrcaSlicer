// Lane-based prime tower inspired by Neotko's SkyLane. The packer is adapted
// from the supplied October 2026 tour; this emitter is specific to this fork.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SkyLaneTower.hpp"
#include "SkyLanePacker.hpp"
#include "GCodeProcessor.hpp"
#include "WipeTower2.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace Slic3r {
namespace {
constexpr double PI = 3.14159265358979323846;

Polygon rectangle(double x0, double y0, double x1, double y1)
{
    return Polygon({scaled(Vec2d(x0, y0)), scaled(Vec2d(x1, y0)), scaled(Vec2d(x1, y1)), scaled(Vec2d(x0, y1))});
}

struct Geometry
{
    const PrintConfig& config;
    double             width, depth, height, lw;

    Polygon outline(double z, double y0, double y1) const
    {
        const double x0 = lw, x1 = width + lw;
        y0 += lw / 2.;
        y1 -= lw / 2.;
        Polygon core = rectangle(x0, y0, x1, y1);
        if (config.wipe_tower_wall_type == WipeTowerWallType::wtwRib) {
            const double length = std::max(0., config.wipe_tower_extra_rib_length.value) * std::max(0., 1. - z / height);
            const double half   = std::min(config.wipe_tower_rib_width.value, std::min(width, depth) / 2.) / 2.;
            if (length <= 1e-6 || half <= 0.)
                return core;
            Polygons pieces{core};
            for (int sx : {-1, 1})
                for (int sy : {-1, 1}) {
                    const Vec2d corner(sx < 0 ? x0 : x1, sy < 0 ? y0 : y1);
                    const Vec2d axis  = Vec2d(sx, sy).normalized(), normal(-axis.y(), axis.x());
                    const Vec2d start = corner - axis * half, end = corner + axis * length;
                    Polygon     arm({scaled(Vec2d(start + normal * half)), scaled(Vec2d(start - normal * half)),
                                     scaled(Vec2d(end - normal * half)), scaled(Vec2d(end + normal * half))});
                    arm.make_counter_clockwise();
                    pieces.push_back(std::move(arm));
                }
            Polygon wall = union_(pieces).front();
            if (config.wipe_tower_fillet_wall)
                wall = union_({rounding_polygon(wall), core}).front();
            return wall;
        }
        if (config.wipe_tower_wall_type == WipeTowerWallType::wtwCone) {
            const auto [radius, scale] = WipeTower2::get_wipe_tower_cone_base(width, height, depth, config.wipe_tower_cone_angle);
            const double r = radius * std::max(0., 1. - z / height), half = (y1 - y0) / 2.;
            if (r > half + .01) {
                const Vec2d  center((x0 + x1) / 2., (y0 + y1) / 2.);
                const double start = std::asin(half / r);
                Points       points{scaled(Vec2d(x1, y1))};
                for (int i = 0; i <= 40; ++i) {
                    const double angle = start + (PI - 2. * start) * i / 40.;
                    points.push_back(scaled(Vec2d(center + Vec2d(r * std::cos(angle) / scale, r * std::sin(angle)))));
                }
                points.push_back(scaled(Vec2d(x0, y1)));
                points.push_back(scaled(Vec2d(x0, y0)));
                for (int i = 0; i <= 40; ++i) {
                    const double angle = PI - start - (PI - 2. * start) * i / 40.;
                    points.push_back(scaled(Vec2d(center + Vec2d(r * std::cos(angle) / scale, -r * std::sin(angle)))));
                }
                points.push_back(scaled(Vec2d(x1, y0)));
                return Polygon(std::move(points));
            }
        }
        return core;
    }
};

class Emitter
{
public:
    Emitter(const PrintConfig&       config,
            const PrintRegionConfig& region,
            SkyLaneTowerResult&      result,
            const Geometry&          geometry,
            double                   safe_z,
            Vec2f                    park)
        : config(config), region(region), result(result), geometry(geometry), width(geometry.width), safe_z(safe_z), park(park)
    {
        code.imbue(std::locale::classic());
        code << std::fixed << std::setprecision(5);
        code << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) << ExtrusionEntity::role_to_string(erWipeTower) << "\n;"
             << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) << "\n";
    }
    void set_tool(size_t id)
    {
        tool        = id;
        ramp_volume = 0.;
    }
    void lift()
    {
        // Explicit coordinates in every move keep post_process_wipe_tower_moves
        // independent of the toolchange macro's final XY position.
        code << "G1 Z{" << safe_z << "+z_offset} F"
             << (config.travel_speed_z.value > 0. ? config.travel_speed_z.value :
                                                    get_value_at(config, config.travel_speed, ConfigFlowDomain::Process, tool)) *
                    60.
             << "\n";
    }
    void travel(double x, double y)
    {
        code << "G1 X" << x << " Y" << y << " F" << get_value_at(config, config.travel_speed, ConfigFlowDomain::Process, tool) * 60.
             << "\n";
        pos = Vec2f(float(x), float(y));
        preview.push_back({pos, 0.f, unsigned(tool)});
    }
    void leave()
    {
        lift();
        travel(park.x(), park.y());
    }
    void change_tool(size_t id)
    {
        leave();
        code << "[change_filament_gcode]\n";
        set_tool(id);
    }
    void begin(size_t old_tool, size_t new_tool, double ramming)
    {
        set_tool(old_tool);
        incoming     = new_tool;
        ramming_left = ramming;
        if (ramming > 0.)
            code << "; SKYLANE RAMMING\n";
        else if (old_tool != new_tool)
            change_tool(new_tool);
    }
    void lower(double z)
    {
        print_z = z;
        code << "G1 Z{" << z << "+z_offset} F"
             << (config.travel_speed_z.value > 0. ? config.travel_speed_z.value :
                                                    get_value_at(config, config.travel_speed, ConfigFlowDomain::Process, tool)) *
                    60.
             << "\n";
    }
    void bead(double x, double y, double flow, double h, double line_width, double max_speed)
    {
        const Vec2f  end{float(x), float(y)};
        const double length = (end - pos).norm();
        if (length < 1e-7)
            return;
        const double ramp         = config.skylane_ramp_volume.value;
        const double chunk_volume = ramp > 0. ? ramp / 16. : std::numeric_limits<double>::max();
        const Vec2f  start        = pos;
        double       done         = 0.;
        while (done < length - 1e-7) {
            const double area           = PI * std::pow(config.filament_diameter.get_at(tool) / 2., 2);
            const double max_flow       = get_value_at(config, config.filament_max_volumetric_speed, ConfigFlowDomain::Filament, tool);
            const double remaining_ramp = std::max(0., ramp - ramp_volume);
            double step = remaining_ramp > 1e-7 ? std::min(length - done, std::min(chunk_volume, remaining_ramp) / flow) : length - done;
            const bool ramming = ramming_left > 1e-7;
            if (ramming)
                step = std::min(step, ramming_left / flow);
            const double fraction = ramp > 0. ? std::min(1., ramp_volume / ramp) : 1.;
            double       speed    = max_speed;
            if (max_flow > 0.)
                speed = std::min(speed, max_flow * (.25 + .75 * fraction) / flow);
            if (ramming) {
                const double limit = get_value_at(config, config.filament_multitool_ramming_flow, ConfigFlowDomain::Filament, tool);
                if (limit > 0.)
                    speed = std::min(speed, limit / flow);
            }
            const Vec2f  point = start + (end - start) * float((done + step) / length);
            const double e     = step * flow / area;
            code << "G1 X" << point.x() << " Y" << point.y() << " E" << e << " F" << speed * 60. << "\n";
            preview.push_back({point, float(line_width), unsigned(tool), float(print_z), float(h)});
            result.used_filament.at(tool) += float(e);
            elapsed += step / speed;
            ramp_volume += step * flow;
            done += step;
            pos = point;
            if (ramming) {
                ramming_left -= step * flow;
                if (ramming_left <= 1e-7) {
                    change_tool(incoming);
                    travel(point.x(), point.y());
                    lower(print_z);
                }
            }
        }
        pos = end;
    }
    double speed(double z_base, double first_z) const
    {
        double value = config.wipe_tower_max_purge_speed.value;
        if (z_base < first_z - 1e-6) {
            const double first = get_value_at(config, config.initial_layer_speed, ConfigFlowDomain::Process, tool);
            value              = std::min(value, first > 0. ? first : 30.);
        }
        if (value <= 0.)
            throw std::runtime_error("SkyLane requires positive tower and first-layer speeds");
        return value;
    }
    void path(const Polygon& polygon, double h, double feed, double gap = 0.)
    {
        // Rotate the seam between rounds and omit exactly 2.5 bead widths of
        // perimeter. The gap is not a wipe and is never repeated per purge strip.
        const Points& points = polygon.points;
        if (points.size() < 3)
            return;
        // Locate the entry on the contour, including rounded rib tips. Cutting
        // by arc length keeps the gap exact even across many short segments.
        std::vector<double> distance(points.size() + 1, 0.);
        double              seam = 0., nearest = std::numeric_limits<double>::max();
        for (size_t j = 0; j < points.size(); ++j) {
            const Vec2d  a = unscale(points[j]), b = unscale(points[(j + 1) % points.size()]);
            const Vec2d  edge   = b - a;
            const double length = edge.norm();
            distance[j + 1]     = distance[j] + length;
            if (length <= 1e-8)
                continue;
            const double t = std::clamp((pos.cast<double>() - a).dot(edge) / edge.squaredNorm(), 0., 1.);
            const double d = (pos.cast<double>() - (a + edge * t)).squaredNorm();
            if (d < nearest) {
                nearest = d;
                seam    = distance[j] + length * t;
            }
        }
        const double perimeter = distance.back();
        if (perimeter < 1e-7)
            return;
        gap                = std::min(gap, perimeter / 2.);
        const double begin = std::fmod(seam + gap / 2., perimeter), end = begin + perimeter - gap;
        auto         point_at = [&](double at) -> Vec2d {
            at = std::fmod(at, perimeter);
            size_t j = std::min(points.size() - 1, size_t(std::upper_bound(distance.begin(), distance.end(), at) - distance.begin() - 1));
            const Vec2d a = unscale(points[j]), b = unscale(points[(j + 1) % points.size()]);
            return a + (b - a) * ((at - distance[j]) / (distance[j + 1] - distance[j]));
        };
        const Vec2d start = point_at(begin);
        lift();
        travel(start.x(), start.y());
        lower(print_z);
        const double flow = h * geometry.lw;
        for (int lap = 0; lap < 2; ++lap)
            for (size_t j = 1; j <= points.size(); ++j) {
                const double at = distance[j] + lap * perimeter;
                if (at > begin + 1e-7 && at < end - 1e-7) {
                    const Vec2d next = unscale(points[j % points.size()]);
                    bead(next.x(), next.y(), flow, h, geometry.lw, feed);
                }
            }
        const Vec2d last = point_at(end);
        bead(last.x(), last.y(), flow, h, geometry.lw, feed);
    }
    void base_support(const Polygon& wall, double first_z, double feed)
    {
        // Fill the rib/cone feet as part of the solid foundation, outside the
        // rectangular purge cell. The brim follows this same contour.
        const double   lw   = geometry.lw;
        const Polygons area = diff(offset(wall, scale_(-lw / 2.)), {rectangle(lw, lw / 2., width + lw, geometry.depth - lw / 2.)});
        if (area.empty())
            return;
        const BoundingBox box = get_extents(area);
        Polylines         lines;
        for (coord_t y = box.min.y(); y <= box.max.y(); y += scale_(lw))
            lines.emplace_back(Points{Point(box.min.x(), y), Point(box.max.x(), y)});
        for (const auto& line : intersection_pl(lines, area)) {
            lift();
            const Vec2d start = unscale(line.points.front());
            travel(start.x(), start.y());
            lower(first_z);
            for (size_t j = 1; j < line.points.size(); ++j) {
                const Vec2d end = unscale(line.points[j]);
                bead(end.x(), end.y(), first_z * lw, first_z, lw, feed);
            }
        }
    }
    void brim(double first_z)
    {
        const double lw = geometry.lw, feed = speed(0., first_z);
        print_z = first_z;
        code << "; SKYLANE BRIM\n;" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) << first_z << "\n;"
             << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) << lw << "\n";
        const Polygon wall = geometry.outline(first_z, 0., geometry.depth);
        base_support(wall, first_z, feed);
        // A 10% overlap on the first ring attaches the brim to the wall.
        for (double ring = .9 * lw; ring < result.brim + 1e-6; ring += lw)
            for (const auto& polygon : offset(wall, scale_(ring), ClipperLib::jtMiter))
                path(polygon, first_z, feed);
    }
    void pass(const SkyLane::Pass& p, size_t round, double first_z, bool vertical)
    {
        print_z        = p.z();
        const double h = p.height, lw = geometry.lw, feed = speed(p.z_base, first_z);
        const double x0 = 2. * lw, x1 = width;
        const double y0 = std::max(p.y0 + lw / 2., p.cell_y0 + 1.5 * lw);
        const double y1 = std::min(p.y1 - lw / 2., p.cell_y1 - 1.5 * lw);
        // Tiny residual strips are narrower than a bead; place their centreline
        // within the strip instead of adding a frame or overflowing the cell.
        const double ya = y1 >= y0 ? y0 : (p.y0 + p.y1) / 2.;
        const double yb = y1 >= y0 ? y1 : ya;
        code << "; SKYLANE " << (p.demand < 0 ? "FILL" : "PURGE") << " lane=" << p.lane << " base=" << p.z_base << "\n"
             << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) << h << "\n"
             << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) << lw << "\n";
        auto wall = [&] {
            code << "; SKYLANE WALL\n";
            path(geometry.outline(p.z(), p.cell_y0, p.cell_y1), h, std::min(feed, 60.), config.wipe_tower_wall_gap ? 2.5 * lw : 0.);
            code << "; SKYLANE WALL_END\n";
        };
        const bool solid  = p.solid || p.z_base < first_z - 1e-6;
        auto       raster = [&](bool along_y, int count, double flow) {
            const double span = along_y ? x1 - x0 : yb - ya;
            for (int line = 0; line < count; ++line) {
                const double t       = count == 1 ? .5 : double(line) / (count - 1);
                const double at      = (round % 2 ? 1. - t : t) * span;
                const bool   reverse = (line + round / 2) % 2;
                const Vec2d  a(along_y ? x0 + at : (reverse ? x1 : x0), along_y ? (reverse ? yb : ya) : ya + at);
                const Vec2d  b(along_y ? x0 + at : (reverse ? x0 : x1), along_y ? (reverse ? ya : yb) : ya + at);
                if (line == 0)
                    lift();
                // The reference travels between rows without extruding. Their
                // connecting moves are not part of the allocated purge volume.
                travel(a.x(), a.y());
                lower(p.z());
                bead(b.x(), b.y(), flow, h, lw, feed);
                // The first couple of lines prime the nozzle before a new wall.
                if (p.wall && line == std::min(1, count - 1) && p.demand >= 0)
                    wall();
            }
        };
        if (p.demand >= 0) {
            const double length = vertical && yb > ya ? yb - ya : x1 - x0;
            vertical            = vertical && yb > ya;
            const double span   = vertical ? x1 - x0 : yb - ya;
            const double pitch  = lw / std::max(p.density, 1e-6);
            const int    lines  = std::max(1, int(std::ceil(span / pitch)) + 1);
            const double volume = width * (p.y1 - p.y0) * h * p.density;
            raster(vertical, lines, volume / (lines * length));
        } else if (solid) {
            const int lines = std::max(1, int(std::ceil((yb - ya) / lw)) + 1);
            raster(false, lines, h * lw);
            if (p.wall)
                wall();
        } else {
            // Open grid: long cross-ties plus sparse longitudinal ties. No
            // perimeter is added to the unfilled tail of an existing round.
            const double pitch = std::min(lw / p.density, std::max(lw, config.wipe_tower_bridging.value));
            if (yb > ya)
                raster(true, std::max(2, int(std::floor((x1 - x0) / pitch)) + 1), h * lw);
            raster(false, std::max(2, int(std::ceil((yb - ya) / std::max(lw, config.wipe_tower_bridging.value)))), h * lw);
            if (p.wall)
                wall();
        }
        result.height = std::max(result.height, p.z());
    }
    void finish()
    {
        leave();
        code << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) << "\nG92 E0\n";
    }
    std::ostringstream                code;
    std::vector<WipeTower::Extrusion> preview;
    double                            elapsed = 0.;

private:
    const PrintConfig&       config;
    const PrintRegionConfig& region;
    SkyLaneTowerResult&      result;
    const Geometry&          geometry;
    double                   width, safe_z, ramp_volume = 0., print_z = 0., ramming_left = 0.;
    Vec2f                    park;
    size_t                   incoming = 0;
    size_t                   tool     = 0;
    Vec2f                    pos      = Vec2f::Zero();
};
} // namespace

SkyLaneTowerResult generate_skylane_tower(const PrintConfig&                 config,
                                          const PrintRegionConfig&           region,
                                          const std::vector<SkyLaneRequest>& requests,
                                          size_t                             layer_count)
{
    if (config.single_extruder_multi_material || config.wipe_tower_no_sparse_layers || config.enable_tower_interface_features)
        throw std::invalid_argument("SkyLane requires a tool changer with No sparse layers and tower interface features disabled");
    SkyLaneTowerResult result;
    result.nominal.resize(layer_count);
    result.local_z.resize(layer_count);
    result.used_filament.resize(config.filament_diameter.size(), 0.f);
    if (requests.empty())
        return result;
    SkyLane::Parameters params;
    const double        min_nozzle = *std::min_element(config.nozzle_diameter.values.begin(), config.nozzle_diameter.values.end());
    const double        max_nozzle = *std::max_element(config.nozzle_diameter.values.begin(), config.nozzle_diameter.values.end());
    params.width                   = config.prime_tower_width.value;
    params.line_width              = max_nozzle * 1.25;
    params.max_height              = min_nozzle * .75;
    // Use the smallest bead when computing clearance, even if another tool
    // determines the packing pitch. This keeps mixed-nozzle lanes conservative.
    params.tip_radius = std::max(.5, max_nozzle) + (max_nozzle - min_nozzle) * 1.25 / 2.;
    params.max_dip    = config.skylane_max_dip.value;
    if (params.max_dip == 0.)
        params.max_dip = std::max(*std::max_element(config.z_hop.values.begin(), config.z_hop.values.end()), params.max_height);
    else
        params.max_height = std::min(params.max_height, params.max_dip);
    if (params.max_dip > 2. || config.skylane_max_dip.value < 0. || !std::isfinite(config.skylane_ramp_volume.value) ||
        config.skylane_ramp_volume.value < 0.)
        throw std::invalid_argument("Invalid SkyLane dip or ramp volume");
    params.purge_density = 1. / std::max(1., config.wipe_tower_extra_spacing.value / 100.);
    std::vector<SkyLane::Demand> demands;
    std::vector<double>          ramming_volumes;
    for (const auto& r : requests) {
        if (r.old_tool >= result.used_filament.size() || r.new_tool >= result.used_filament.size() || r.layer >= layer_count)
            throw std::invalid_argument("Invalid SkyLane tool or layer index");
        const double ramming = r.old_tool != r.new_tool && config.filament_multitool_ramming.get_at(r.old_tool) ?
                                   config.filament_multitool_ramming_volume.get_at(r.old_tool) :
                                   0.;
        // One continuous path: the outgoing tool consumes its ramming volume,
        // then the incoming tool carries on in the same strip.
        demands.push_back({r.z, r.height, r.volume + ramming, r.layer});
        ramming_volumes.push_back(ramming);
    }
    auto packed = SkyLane::plan(demands, params);
    if (!packed.ok)
        throw std::runtime_error("SkyLane could not fit the tool changes within its nozzle-dip limit");
    if (config.wipe_tower_wall_type != WipeTowerWallType::wtwRib)
        packed = SkyLane::taper(demands, params, packed);
    result.depth    = packed.depth;
    const double lw = params.line_width, first_z = requests.front().z;
    double       top = first_z;
    for (const auto& pass : packed.passes)
        top = std::max(top, pass.z());
    const Geometry geometry{config, params.width, packed.depth, top, lw};
    result.brim                           = std::ceil(config.prime_tower_brim_width.value / lw) * lw;
    const Polygons    footprint           = offset(geometry.outline(first_z, 0., packed.depth), scale_(result.brim + lw / 2.));
    const BoundingBox bounds              = get_extents(footprint);
    result.bounds                         = BoundingBoxf(unscale(bounds.min), unscale(bounds.max));
    const Vec2f                      park = result.bounds.min.cast<float>() - Vec2f(1.f, 1.f);
    std::vector<std::vector<size_t>> passes_by_request(demands.size());
    for (size_t j = 0; j < packed.passes.size(); ++j)
        passes_by_request.at(packed.passes[j].owner).push_back(j);
    size_t            round     = 0;
    double            highest   = 0.;
    bool              brim_done = false;
    std::vector<bool> lane_vertical(packed.lanes, false);
    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& r = requests[i];
        for (size_t pi : passes_by_request[i])
            highest = std::max(highest, packed.passes[pi].z());
        const double safe_z = std::max(r.z, highest) + .2;
        Emitter      emitter(config, region, result, geometry, safe_z, park);
        const bool   base = !passes_by_request[i].empty() && packed.passes[passes_by_request[i].front()].z_base < first_z - 1e-6;
        emitter.begin(r.old_tool, r.new_tool, base ? 0. : ramming_volumes[i]);
        if (i == 0)
            emitter.code << "; SKYLANE plan: lanes=" << packed.lanes << " depth=" << packed.depth << " width=" << params.width
                         << " max_dip=" << params.max_dip << "\n";
        // Like the reference, finish the foundation when leaving the first
        // plane. This keeps the brim attached without framing every strip.
        if (!brim_done && (r.z > first_z + 1e-6 || i + 1 == requests.size())) {
            emitter.brim(first_z);
            brim_done = true;
        }
        for (size_t pi : passes_by_request[i]) {
            const auto& pass = packed.passes[pi];
            if (pass.wall)
                ++round;
            emitter.pass(pass, round - (round > 0), first_z, !pass.merged && lane_vertical[pass.lane]);
            if (!pass.merged)
                lane_vertical[pass.lane] = !lane_vertical[pass.lane];
            const Polygon wall                 = geometry.outline(pass.z(), pass.cell_y0, pass.cell_y1);
            result.outer_wall[float(pass.z())] = {wall.split_at_first_point()};
        }
        emitter.finish();
        WipeTower::ToolChangeResult tcr{};
        tcr.print_z         = float(r.z);
        tcr.layer_height    = float(r.height);
        tcr.initial_tool    = int(r.old_tool);
        tcr.new_tool        = int(r.new_tool);
        tcr.is_tool_change  = r.old_tool != r.new_tool;
        tcr.is_finish_first = !tcr.is_tool_change;
        tcr.skylane_safe_z  = float(safe_z);
        tcr.start_pos = tcr.end_pos = tcr.tool_change_start_pos = park;
        tcr.gcode                                               = emitter.code.str();
        tcr.extrusions                                          = std::move(emitter.preview);
        tcr.elapsed_time                                        = float(emitter.elapsed);
        tcr.purge_volume                                        = float(r.volume);
        (r.local_z ? result.local_z : result.nominal)[r.layer].push_back(std::move(tcr));
        result.toolchanges += r.old_tool != r.new_tool;
    }
    // Include the brim in collision/arrangement bounds at the bed only.
    for (const auto& polygon : footprint)
        result.outer_wall[float(first_z)].push_back(polygon.split_at_first_point());
    return result;
}
} // namespace Slic3r
