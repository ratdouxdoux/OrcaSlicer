// Lane-based prime tower inspired by Neotko's SkyLane. The packer is adapted
// from the supplied October 2026 tour; this emitter is specific to this fork.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SkyLaneTower.hpp"
#include "SkyLanePacker.hpp"
#include "GCodeProcessor.hpp"
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

struct Phase
{
    size_t request;
    size_t tool;
    bool   ramming;
};

class Emitter
{
public:
    Emitter(
        const PrintConfig& config, const PrintRegionConfig& region, SkyLaneTowerResult& result, double width, double safe_z, double offset)
        : config(config), region(region), result(result), width(width), safe_z(safe_z), offset(offset)
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
        code << "G1 X" << x - offset << " Y" << y - offset << " F"
             << get_value_at(config, config.travel_speed, ConfigFlowDomain::Process, tool) * 60. << "\n";
        pos = Vec2f(float(x), float(y));
        preview.push_back({pos, 0.f, unsigned(tool)});
    }
    void leave()
    {
        lift();
        travel(-1., -1.);
    }
    void change_tool(size_t id)
    {
        leave();
        code << "[change_filament_gcode]\n";
        set_tool(id);
    }
    void bead(double x, double y, double flow, double h, double line_width, double max_speed)
    {
        const Vec2f  end{float(x), float(y)};
        const double length = (end - pos).norm();
        if (length < 1e-7)
            return;
        const double area         = PI * std::pow(config.filament_diameter.get_at(tool) / 2., 2);
        const double max_flow     = get_value_at(config, config.filament_max_volumetric_speed, ConfigFlowDomain::Filament, tool);
        const double ramp         = config.skylane_ramp_volume.value;
        const double chunk_volume = ramp > 0. ? ramp / 16. : std::numeric_limits<double>::max();
        const Vec2f  start        = pos;
        double       done         = 0.;
        while (done < length - 1e-7) {
            const double remaining_ramp = std::max(0., ramp - ramp_volume);
            const double step           = remaining_ramp > 1e-7 ? std::min(length - done, std::min(chunk_volume, remaining_ramp) / flow) :
                                                                  length - done;
            const double fraction       = ramp > 0. ? std::min(1., ramp_volume / ramp) : 1.;
            double       speed          = max_speed;
            if (max_flow > 0.)
                speed = std::min(speed, max_flow * (.25 + .75 * fraction) / flow);
            const Vec2f  point = start + (end - start) * float((done + step) / length);
            const double e     = step * flow / area;
            code << "G1 X" << point.x() - offset << " Y" << point.y() - offset << " E" << e << " F" << speed * 60. << "\n";
            preview.push_back({point, float(line_width), unsigned(tool), float(print_z), float(h)});
            result.used_filament.at(tool) += float(e);
            elapsed += step / speed;
            ramp_volume += step * flow;
            deposited_volume += step * flow;
            done += step;
        }
        pos = end;
    }
    void pass(const SkyLane::Pass& p, size_t ordinal, double offset, double first_z, bool ramming)
    {
        const double volume_before = deposited_volume;
        print_z                    = p.z();
        const double h             = p.height;
        const double lw            = config.nozzle_diameter.get_at(tool) * 1.25;
        const double flow          = h * (lw - h * (1. - PI / 4.));
        const double x0 = offset + lw / 2., x1 = offset + width - lw / 2.;
        const double y0 = offset + p.y0 + std::min(lw / 2., (p.y1 - p.y0) / 2.);
        const double y1 = offset + p.y1 - std::min(lw / 2., (p.y1 - p.y0) / 2.);
        if (flow <= 0. || x1 <= x0 || y1 < y0)
            throw std::runtime_error("SkyLane pass has no printable area");
        const double infill_speed = get_value_at(config, region.sparse_infill_speed, ConfigFlowDomain::Process, tool);
        double       speed        = std::min(config.wipe_tower_max_purge_speed.value, infill_speed > 0. ? infill_speed : 80.);
        if (ramming) {
            const double ramming_flow = get_value_at(config, config.filament_multitool_ramming_flow, ConfigFlowDomain::Filament, tool);
            if (ramming_flow > 0.)
                speed = std::min(speed, ramming_flow / flow);
        }
        if (p.z_base < first_z - 1e-6) {
            const double first_speed = get_value_at(config, config.initial_layer_speed, ConfigFlowDomain::Process, tool);
            speed                    = std::min(speed, first_speed > 0. ? first_speed : 30.);
        }
        if (speed <= 0.)
            throw std::runtime_error("SkyLane requires positive tower and first-layer speeds");
        code << "; SKYLANE " << (p.demand < 0 ? "FILL" : "PURGE") << " lane=" << p.lane << " base=" << p.z_base << "\n"
             << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) << h << "\n"
             << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) << lw << "\n";
        lift();
        travel(x0, y0);
        code << "G1 Z{" << p.z() << "+z_offset} F"
             << (config.travel_speed_z.value > 0. ? config.travel_speed_z.value :
                                                    get_value_at(config, config.travel_speed, ConfigFlowDomain::Process, tool)) *
                    60.
             << "\n";

        // Each independently growing lane has its own perimeter. A small gap
        // avoids putting the first bead of a parked tool on its perimeter.
        double perimeter_volume = 0.;
        if (y1 - y0 > 2. * lw) {
            const double gap = config.wipe_tower_wall_gap.value ? std::min(2.5 * lw, x1 - x0) : 0.;
            travel(x0 + gap, y0);
            bead(x1, y0, flow, h, lw, speed);
            bead(x1, y1, flow, h, lw, speed);
            bead(x0, y1, flow, h, lw, speed);
            bead(x0, y0, flow, h, lw, speed);
            perimeter_volume = (2. * (x1 - x0 + y1 - y0) - gap) * flow;
        }
        const bool   solid         = p.solid || p.z_base < first_z - 1e-6;
        const double target_volume = width * (p.y1 - p.y0) * h * (solid ? 1. : p.density);
        // Alternate the fill direction so sparse support doesn't form isolated
        // parallel walls. Leave one bead between the fill and its perimeter.
        const bool   frame = y1 - y0 > 2. * lw;
        const double xa = frame ? x0 + lw : x0, xb = frame ? x1 - lw : x1;
        const double ya = frame ? y0 + lw : y0, yb = frame ? y1 - lw : y1;
        const bool   vertical = ordinal % 2 != 0 && yb - ya > lw;
        const double span     = vertical ? xb - xa : yb - ya;
        const double length   = vertical ? yb - ya : xb - xa;
        if (length > 1e-6 && span >= 0.) {
            const int    capacity      = std::max(1, int(std::floor(span / lw)) + 1);
            const double remaining     = std::max(0., target_volume - perimeter_volume);
            const int    support_lines = std::max(1, int(std::ceil(span / std::max(lw, config.wipe_tower_bridging.value))) + 1);
            int          lines         = solid ? capacity : std::max(support_lines, int(std::ceil(remaining / (length * flow))));
            // The packer deliberately leaves extrusion headroom for the oval
            // bead cross-section and the lane perimeters. Never overflow a cell.
            if (!solid && p.demand >= 0 && lines > capacity && remaining > capacity * length * flow + 1e-5)
                throw std::runtime_error("SkyLane purge does not fit its planned cell; increase tower width");
            lines = std::min(capacity, lines);
            for (int i = 0; i < lines; ++i) {
                const double s = lines == 1 ? span / 2. : span * i / (lines - 1);
                const double a = i % 2 ? length : 0., b = length - a;
                travel(vertical ? xa + s : xa + a, vertical ? ya + a : ya + s);
                bead(vertical ? xa + s : xa + b, vertical ? ya + b : ya + s, flow, h, lw, speed);
            }
        }
        if (p.demand >= 0 && deposited_volume - volume_before + 1e-5 < width * (p.y1 - p.y0) * h * p.density)
            throw std::runtime_error("SkyLane purge does not fit its planned cell; increase tower width");
        result.height = std::max(result.height, p.z());
    }
    void brim(double offset, double depth, double first_z)
    {
        print_z                       = first_z;
        const double lw               = config.nozzle_diameter.get_at(tool) * 1.25;
        const double flow             = first_z * (lw - first_z * (1. - PI / 4.));
        const double configured_speed = get_value_at(config, config.initial_layer_speed, ConfigFlowDomain::Process, tool);
        const double speed            = std::min(config.wipe_tower_max_purge_speed.value, configured_speed > 0. ? configured_speed : 30.);
        code << "; SKYLANE BRIM\n;" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) << first_z << "\n;"
             << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) << lw << "\n";
        for (double ring = lw / 2.; ring <= offset + 1e-6; ring += lw) {
            lift();
            travel(offset - ring, offset - ring);
            code << "G1 Z{" << first_z << "+z_offset} F"
                 << (config.travel_speed_z.value > 0. ? config.travel_speed_z.value :
                                                        get_value_at(config, config.travel_speed, ConfigFlowDomain::Process, tool)) *
                        60.
                 << "\n";
            bead(offset + width + ring, offset - ring, flow, first_z, lw, speed);
            bead(offset + width + ring, offset + depth + ring, flow, first_z, lw, speed);
            bead(offset - ring, offset + depth + ring, flow, first_z, lw, speed);
            bead(offset - ring, offset - ring, flow, first_z, lw, speed);
        }
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
    double                   width, safe_z, offset, ramp_volume = 0., deposited_volume = 0., print_z = 0.;
    size_t                   tool = 0;
    Vec2f                    pos  = Vec2f::Zero();
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
    // Packing volume uses rectangular slabs; emitted beads have rounded sides
    // and a perimeter. Reserve enough space to emit the requested volume without
    // either over-extruding a bead or writing outside its lane.
    params.purge_density = .55 / std::max(1., config.wipe_tower_extra_spacing.value / 100.);
    std::vector<SkyLane::Demand> demands;
    std::vector<Phase>           phases;
    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& r = requests[i];
        if (r.old_tool >= result.used_filament.size() || r.new_tool >= result.used_filament.size() || r.layer >= layer_count)
            throw std::invalid_argument("Invalid SkyLane tool or layer index");
        const double ramming = r.old_tool != r.new_tool && config.filament_multitool_ramming.get_at(r.old_tool) ?
                                   config.filament_multitool_ramming_volume.get_at(r.old_tool) :
                                   0.;
        if (ramming > 0.) {
            demands.push_back({r.z, r.height, ramming, r.layer});
            phases.push_back({i, r.old_tool, true});
        }
        demands.push_back({r.z, r.height, r.volume, r.layer});
        phases.push_back({i, r.new_tool, false});
    }
    const auto packed = SkyLane::plan(demands, params);
    if (!packed.ok)
        throw std::runtime_error("SkyLane could not fit the tool changes within its nozzle-dip limit");
    result.depth        = packed.depth;
    const double lw     = params.line_width;
    const double offset = std::ceil(config.prime_tower_brim_width.value / lw) * lw;
    result.brim         = offset;
    result.bounds       = BoundingBoxf(Vec2d(-offset, -offset), Vec2d(params.width + offset, result.depth + offset));
    std::vector<std::vector<size_t>> passes_by_phase(demands.size());
    for (size_t i = 0; i < packed.passes.size(); ++i)
        passes_by_phase.at(packed.passes[i].owner).push_back(i);
    size_t phase   = 0;
    double highest = 0.;
    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& r   = requests[i];
        size_t      end = phase;
        while (end < phases.size() && phases[end].request == i) {
            for (size_t pi : passes_by_phase[end])
                highest = std::max(highest, packed.passes[pi].z());
            ++end;
        }
        const double safe_z = std::max(r.z, highest) + .2;
        Emitter      emitter(config, region, result, params.width, safe_z, offset);
        emitter.set_tool(r.old_tool);
        if (i == 0) {
            emitter.code << "; SKYLANE plan: lanes=" << packed.lanes << " depth=" << packed.depth << " width=" << params.width
                         << " max_dip=" << params.max_dip << "\n";
            emitter.brim(offset, packed.depth, requests.front().z);
        }
        for (; phase < end; ++phase) {
            if (!phases[phase].ramming && r.old_tool != r.new_tool)
                emitter.change_tool(r.new_tool);
            if (phases[phase].ramming)
                emitter.code << "; SKYLANE RAMMING\n";
            for (size_t pi : passes_by_phase[phase])
                emitter.pass(packed.passes[pi], pi, offset, requests.front().z, phases[phase].ramming);
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
        // Park 1 mm beyond the brim corner so restoring a lower model plane
        // cannot descend through the tower, including when the brim is disabled.
        tcr.start_pos = tcr.end_pos = tcr.tool_change_start_pos = Vec2f(float(-offset - 1.), float(-offset - 1.));
        tcr.gcode                                               = emitter.code.str();
        tcr.extrusions                                          = std::move(emitter.preview);
        for (auto& e : tcr.extrusions)
            e.pos -= Vec2f(float(offset), float(offset));
        tcr.elapsed_time = float(emitter.elapsed);
        tcr.purge_volume = float(r.volume);
        (r.local_z ? result.local_z : result.nominal)[r.layer].push_back(std::move(tcr));
        result.toolchanges += r.old_tool != r.new_tool;
    }
    const Points outline = {scaled(Vec2d(-offset, -offset)), scaled(Vec2d(params.width + offset, -offset)),
                            scaled(Vec2d(params.width + offset, packed.depth + offset)), scaled(Vec2d(-offset, packed.depth + offset)),
                            scaled(Vec2d(-offset, -offset))};
    for (const auto& r : requests)
        result.outer_wall[float(r.z)] = {Polyline(outline)};
    return result;
}
} // namespace Slic3r
