#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "libslic3r/GCode/SkyLanePacker.hpp"
#include "libslic3r/GCode/SkyLaneTower.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PlaceholderParser.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>

using namespace Slic3r;
using Catch::Approx;

namespace {
PrintConfig configuration(size_t tools = 2)
{
    auto dynamic = DynamicPrintConfig::full_print_config();
    dynamic.set_num_extruders(tools);
    dynamic.set_num_filaments(tools);
    dynamic.set("enable_prime_tower", true);
    dynamic.set("enable_skylane", true);
    dynamic.set("single_extruder_multi_material", false);
    dynamic.set("enable_tower_interface_features", false);
    dynamic.set("prime_tower_width", 28.);
    dynamic.set("prime_volume", 38.);
    dynamic.set("prime_tower_brim_width", 5.);
    dynamic.set("skylane_max_dip", .4);
    dynamic.option<ConfigOptionPercent>("wipe_tower_extra_spacing")->value = 120.;
    dynamic.option<ConfigOptionFloats>("filament_diameter")->values.assign(tools, 1.75);
    dynamic.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values.assign(tools, 10.);
    dynamic.option<ConfigOptionBools>("filament_multitool_ramming")->values.assign(tools, false);
    PrintConfig result;
    result.apply(dynamic, true);
    return result;
}

// Stock TCR markers, including the outgoing-tool section that SkyLane replaces.
WipeTower::ToolChangeResult visit(float z, float h, int old_tool, int new_tool, double width = 28.)
{
    WipeTower::ToolChangeResult t{};
    t.print_z        = z;
    t.layer_height   = h;
    t.initial_tool   = old_tool;
    t.new_tool       = new_tool;
    t.is_tool_change = old_tool != new_tool;
    t.start_pos = t.end_pos = t.tool_change_start_pos = Vec2f(.5f, .25f);
    t.gcode                                           = "; CP TOOLCHANGE START\n;TYPE:Prime tower\n; WIPE_TOWER_START\n"
                                                        ";HEIGHT:0.2\n;WIDTH:0.5\n; CP TOOLCHANGE UNLOAD\n"
                                                        "G1 X0.5 Y0.25\n[change_filament_gcode]\n; CP TOOLCHANGE WIPE\nG1 X" +
              std::to_string(width + .5) + " Y0.25 E1 F1200\n; WIPE_TOWER_END\n; CP TOOLCHANGE END\n";
    return t;
}

using Layers = std::vector<std::vector<WipeTower::ToolChangeResult>>;
Layers sequence(size_t layers, size_t changes, double width = 28.)
{
    Layers result(layers);
    int    tool = 0;
    for (size_t layer = 0; layer < layers; ++layer)
        for (size_t i = 0; i < changes; ++i) {
            result[layer].push_back(visit(float(.2 + .08 * layer), .08f, tool, 1 - tool, width));
            tool = 1 - tool;
        }
    return result;
}
} // namespace

TEST_CASE("SkyLane settings preserve the existing toggle and add source-pack controls", "[SkyLane][Config]")
{
    const auto defaults = DynamicPrintConfig::full_print_config();
    CHECK_FALSE(defaults.opt_bool("enable_skylane"));
    CHECK(defaults.opt_bool("skylane_profile_speeds"));
    CHECK(defaults.opt_float("skylane_ramp_volume") == 4.);
    const auto& keys = Preset::print_options();
    for (const auto* key : {"enable_skylane", "skylane_max_dip", "skylane_ramp_volume", "skylane_wall_height", "skylane_profile_speeds"}) {
        CHECK(std::find(keys.begin(), keys.end(), key) != keys.end());
        DynamicPrintConfig restored;
        restored.set_deserialize_strict(key, defaults.option(key)->serialize());
        CHECK(*restored.option(key) == *defaults.option(key));
    }
}

TEST_CASE("SkyLane preserves archive wall shapes with material bands", "[SkyLane][Geometry]")
{
    std::ifstream counts(std::string(TEST_DATA_DIR) + "/skylane/dragon_changes.txt");
    REQUIRE(counts.good());
    Layers nominal;
    size_t layer = 0, changes = 0;
    int    n, tool            = 0;
    while (counts >> n) {
        nominal.emplace_back();
        for (int i = 0; i < n; ++i) {
            nominal.back().push_back(visit(float(.25 + layer * .22), layer == 0 ? .25f : .22f, tool, 1 - tool));
            tool = 1 - tool;
            ++changes;
        }
        ++layer;
    }
    REQUIRE(changes == 395);
    auto config                                     = configuration();
    config.filament_multitool_ramming.values        = {true, true};
    config.filament_multitool_ramming_volume.values = {5., 5.};
    config.wipe_tower_extra_rib_length.value        = 8.;
    config.wipe_tower_rib_width.value               = 8.;
    config.wipe_tower_fillet_wall.value             = true;
    config.wipe_tower_wall_gap.value                = true;
    config.wipe_tower_cone_angle.value              = 60.;
    BoundingBoxf rectangle;
    for (auto wall : {WipeTowerWallType::wtwRectangle, WipeTowerWallType::wtwCone, WipeTowerWallType::wtwRib}) {
        config.wipe_tower_wall_type.value = wall;
        SkyLaneTower tower;
        Layers       local;
        auto         preview = nominal;
        REQUIRE(tower.plan(config, nominal, local, 5.f));
        CHECK(tower.depth() > 0.);
        CHECK(tower.depth() < 80.);
        tower.preview(preview, local);
        REQUIRE_FALSE(tower.outlines().empty());
        REQUIRE(tower.bounds().defined);
        const auto foundation = tower.outlines().find(.25f);
        REQUIRE(foundation != tower.outlines().end());
        const auto& base = foundation->second;
        REQUIRE(base.size() > 1);
        CHECK(base.front().is_closed());
        if (wall == WipeTowerWallType::wtwRectangle)
            rectangle = tower.bounds();
        else
            CHECK(tower.bounds().size().y() > rectangle.size().y());
        if (wall == WipeTowerWallType::wtwRib)
            CHECK(tower.bounds().size().x() > rectangle.size().x());
    }
}

TEST_CASE("SkyLane incremental lanes preserve volume and nozzle clearance", "[SkyLane][Packing]")
{
    SkyLane::Params p;
    p.width        = 20.;
    p.fill_density = .2;
    p.solid_base   = true;
    std::vector<SkyLane::Demand> demands;
    for (size_t layer = 0; layer < 40; ++layer)
        for (size_t change = 0; change < (layer % 7 == 0 ? 8 : 2); ++change)
            demands.push_back({float(.2 + .08 * layer), float(4 + (layer * 17 + change * 13) % 21), .08f, int(layer)});
    const auto plan = SkyLane::pack_min_depth(demands, p);
    REQUIRE(plan.ok);
    CHECK(plan.over_z == 0);
    CHECK(std::any_of(plan.passes.begin(), plan.passes.end(), [](const auto& pass) { return !pass.merged; }));
    CHECK(std::any_of(plan.passes.begin(), plan.passes.end(), [](const auto& pass) { return pass.merged; }));
    std::vector<double> volumes(demands.size(), 0.);
    bool                dipped = false;
    for (size_t i = 0; i < plan.passes.size(); ++i) {
        const auto& pass = plan.passes[i];
        REQUIRE(pass.owner >= 0);
        REQUIRE(size_t(pass.owner) < demands.size());
        CHECK(pass.h > 0.);
        CHECK(pass.h <= p.hmax + 1e-6);
        CHECK(pass.z_base + pass.h <= demands[pass.owner].z + 1e-6);
        if (pass.demand >= 0) {
            volumes.at(pass.demand) += p.width * (pass.y1 - pass.y0) * pass.h * pass.density;
            dipped |= pass.z_base + pass.h < demands[pass.owner].z - .01;
        }
        for (size_t j = 0; j < i; ++j) {
            const auto& previous = plan.passes[j];
            if (previous.z_base + previous.h <= pass.z_base + pass.h + 1e-6)
                continue;
            const double gap       = std::max(pass.y0 - previous.y1, previous.y0 - pass.y1);
            const double clearance = p.r_tip - p.line_w / 2. +
                                     (previous.z_base + previous.h - pass.z_base - pass.h) * std::tan(p.cone_deg * M_PI / 180.);
            CHECK(gap + 1e-6 >= clearance);
        }
    }
    CHECK(dipped);
    for (size_t i = 0; i < demands.size(); ++i)
        CHECK(volumes[i] == Approx(demands[i].volume).margin(.001));
    p.max_pit = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS(SkyLane::pack(demands, 20., p), std::invalid_argument);
}

TEST_CASE("SkyLane ramps purge before the wall and resets cleanly between exports", "[SkyLane][GCode]")
{
    auto config                         = configuration();
    config.prime_volume.value           = 15.;
    config.skylane_ramp_volume.value    = 7.;
    config.wipe_tower_wall_type.value   = WipeTowerWallType::wtwRectangle;
    config.prime_tower_brim_width.value = 0.;
    double flow_limit                   = 10.;
    SECTION("Normal flow") {}
    SECTION("Low-flow filament with brim")
    {
        flow_limit                                  = 2.5;
        config.filament_max_volumetric_speed.values = {flow_limit, flow_limit};
        config.prime_tower_brim_width.value         = 5.;
    }
    auto nominal = sequence(8, 3);
    // This fork's stock main block has no UNLOAD marker; unloading is a sidecar.
    for (auto& layer : nominal)
        for (auto& t : layer) {
            const std::string marker = "; CP TOOLCHANGE UNLOAD\n";
            t.gcode.erase(t.gcode.find(marker), marker.size());
            t.nozzle_change_result.gcode = "G1 X999 Y999 E99\n";
        }
    SkyLaneTower tower;
    REQUIRE(tower.plan(config, nominal, {}, float(config.prime_tower_brim_width.value)));
    const double area = M_PI * 1.75 * 1.75 / 4.;
    std::string  first_export;
    for (int repetition = 0; repetition < 2; ++repetition) {
        tower.begin_emission();
        std::string output;
        for (const auto& layer : nominal)
            for (const auto& original : layer) {
                const auto t = tower.place(original);
                output += t.gcode;
                PlaceholderParser parser;
                parser.set("change_filament_gcode", std::string("; change\n"));
                GCodeReader reader;
                reader.apply_config(config);
                reader.x()    = t.start_pos.x();
                reader.y()    = t.start_pos.y();
                double volume = 0.;
                reader.parse_buffer(parser.process(t.gcode), [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
                    if (line.extruding(state) && line.dist_XY(state) > 1e-6) {
                        CHECK(state.z() <= original.print_z + 1e-5);
                        const double v    = line.dist_E(state) * area;
                        const double flow = v / line.dist_XY(state) * (line.has_f() ? line.f() : state.f()) / 60.;
                        CHECK(flow <= flow_limit + .03);
                        volume += v;
                    }
                    if (original.print_z == nominal[0][0].print_z && original.initial_tool == 0 && line.raw() == "; SKYLANE WALL")
                        CHECK(volume >= 7. - .01);
                });
                CHECK(volume >= 15. - .05);
                CHECK(reader.z() == Approx(t.skylane_safe_z).margin(.001));
                CHECK(t.nozzle_change_result.gcode.empty());
                CHECK(t.gcode.find("X999") == std::string::npos);
            }
        if (repetition == 0)
            first_export = output;
        else
            CHECK(output == first_export);
    }
    // An unexpected repeated visit must fail explicitly once the available height is consumed.
    tower.begin_emission();
    bool rejected = false;
    for (int i = 0; i < 100 && !rejected; ++i) {
        try {
            tower.place(nominal[0][0]);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
    }
    CHECK(rejected);
}

TEST_CASE("SkyLane emits rib supports when merged cells split into independent lanes", "[SkyLane][Lanes]")
{
    auto config                                     = configuration(3);
    config.prime_tower_width.value                  = 20.;
    config.prime_volume.value                       = 4.;
    config.wipe_tower_extra_spacing.value           = 105.;
    config.filament_multitool_ramming.values        = {true, true, true};
    config.filament_multitool_ramming_volume.values = {0., 7., 14.};
    config.wipe_tower_wall_type.value               = WipeTowerWallType::wtwRib;
    config.wipe_tower_extra_rib_length.value        = 8.;
    config.wipe_tower_rib_width.value               = 8.;
    Layers nominal(40);
    int    tool = 0;
    for (size_t layer = 0; layer < nominal.size(); ++layer)
        for (size_t change = 0; change < (layer % 7 == 0 ? 8 : 2); ++change) {
            nominal[layer].push_back(visit(float(.2 + .08 * layer), .08f, tool, (tool + 1) % 3, 20.));
            tool = (tool + 1) % 3;
        }
    SkyLaneTower tower;
    REQUIRE(tower.plan(config, nominal, {}, 5.f));
    tower.begin_emission();
    bool raised_ribs = false, rammed = false;
    for (const auto& layer : nominal)
        for (const auto& original : layer) {
            const auto t = tower.place(original);
            raised_ribs |= t.gcode.find("; SKYLANE rib") != std::string::npos;
            rammed |= t.skylane_ram;
            REQUIRE_FALSE(t.extrusions.empty());
            for (const auto& e : t.extrusions)
                if (e.width > 0.)
                    CHECK(e.z <= original.print_z + .001f);
            CHECK(t.skylane_safe_z >= t.skylane_z + .199f);
        }
    CHECK(raised_ribs);
    CHECK(rammed);
}

TEST_CASE("SkyLane keeps the reported four-colour sequence in contiguous planar bands", "[SkyLane][Colours]")
{
    std::ifstream input(std::string(TEST_DATA_DIR) + "/skylane/colour_visits.txt");
    REQUIRE(input.good());
    Layers nominal(1);
    auto   base = visit(.2f, .2f, 0, 0, 32.);
    base.gcode  = "; CP EMPTY GRID\n";
    nominal.front().push_back(base);
    float  z;
    int    old_tool, new_tool;
    size_t changes = 0;
    while (input >> z >> old_tool >> new_tool) {
        if (nominal.back().front().print_z != z)
            nominal.emplace_back();
        nominal.back().push_back(visit(z, .08f, old_tool, new_tool, 32.));
        ++changes;
    }
    REQUIRE(changes == 510);
    auto config                    = configuration(4);
    config.prime_tower_width.value = 30.;
    config.prime_volume.value      = 18.;
    config.filament_multitool_ramming.values.assign(4, true);
    config.filament_multitool_ramming_volume.values.assign(4, 5.);
    config.filament_multitool_ramming_flow.values.assign(4, 25.);
    config.filament_max_volumetric_speed.values.assign(4, 15.);
    SECTION("Equal ramming volumes") {}
    SECTION("Reported asymmetric black filament")
    {
        config.filament_multitool_ramming_volume.values[2] = .5;
        config.filament_multitool_ramming_flow.values[2]   = 20.;
        config.filament_max_volumetric_speed.values[2]     = 12.;
    }
    config.initial_layer_speed.values        = {50.};
    config.wipe_tower_max_purge_speed.value  = 90.;
    config.wipe_tower_extra_rib_length.value = 8.;
    config.wipe_tower_rib_width.value        = 8.;
    config.wipe_tower_fillet_wall.value      = true;
    config.wipe_tower_wall_gap.value         = true;
    config.wipe_tower_cone_angle.value       = 60.;
    const double area                        = M_PI * 1.75 * 1.75 / 4.;
    for (auto wall_type : {WipeTowerWallType::wtwRectangle, WipeTowerWallType::wtwRib, WipeTowerWallType::wtwCone}) {
        CAPTURE(int(wall_type));
        config.wipe_tower_wall_type.value = wall_type;
        SkyLaneTower tower;
        REQUIRE(tower.plan(config, nominal, {}, 5.f));
        tower.begin_emission();
        struct Band
        {
            double low, high;
            int    tool;
        };
        std::map<long, std::vector<Band>> planes;
        size_t                            walls = 0, purges = 0, adjacent_moves = 0;
        double                            travel_length      = 0.;
        double                            highest_deposition = 0.;
        for (const auto& layer : nominal)
            for (const auto& original : layer) {
                const auto        t       = tower.place(original);
                bool              is_wall = false, in_band = false, merged = false, incoming = false, recorded = false;
                bool              adjacent_entry = false;
                std::string       kind;
                int               band_tool = -1;
                double            low = 0., high = 0., cell0 = 0., cell1 = 0., ram_volume = 0., purge_volume = 0.;
                PlaceholderParser parser;
                const std::string change = "T" + std::to_string(original.new_tool);
                parser.set("change_filament_gcode", change);
                GCodeReader reader;
                reader.apply_config(config);
                reader.x() = t.start_pos.x();
                reader.y() = t.start_pos.y();
                reader.parse_buffer(parser.process(t.gcode), [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
                    const std::string raw = line.raw();
                    if (raw == change)
                        incoming = true;
                    if (raw == "; SKYLANE adjacent band travel")
                        adjacent_entry = true;
                    if (raw == "; SKYLANE BAND_END")
                        in_band = false;
                    if (raw == "; SKYLANE WALL") {
                        is_wall = true;
                        ++walls;
                    }
                    if (raw == "; SKYLANE WALL_END")
                        is_wall = false;
                    if (raw.rfind("; SKYLANE BAND ", 0) == 0) {
                        std::istringstream fields(raw.substr(15));
                        std::string        field;
                        while (fields >> field) {
                            const auto eq  = field.find('=');
                            const auto key = field.substr(0, eq), value = field.substr(eq + 1);
                            if (key == "tool")
                                band_tool = std::stoi(value);
                            if (key == "kind")
                                kind = value;
                            if (key == "y0")
                                low = std::stod(value);
                            if (key == "y1")
                                high = std::stod(value);
                            if (key == "merged")
                                merged = std::stoi(value) != 0;
                            if (key == "cell0")
                                cell0 = std::stod(value);
                            if (key == "cell1")
                                cell1 = std::stod(value);
                        }
                        in_band  = true;
                        recorded = false;
                    }
                    if (line.travel() && line.dist_XY(state) > 1e-6) {
                        travel_length += line.dist_XY(state);
                        if (adjacent_entry) {
                            CHECK(merged);
                            CHECK(line.dist_Z(state) == Approx(0.).margin(.001));
                            CHECK(state.y() >= low - .001);
                            CHECK(state.y() <= high + .001);
                            CHECK(state.y() + line.dist_Y(state) >= low - .001);
                            CHECK(state.y() + line.dist_Y(state) <= high + .001);
                            ++adjacent_moves;
                            adjacent_entry = false;
                        }
                    }
                    if (!line.extruding(state) || line.dist_XY(state) <= 1e-6)
                        return;
                    highest_deposition = std::max(highest_deposition, double(state.z()));
                    CHECK(state.z() <= original.print_z + .001);
                    if (!in_band)
                        return; // brim / base supports
                    CHECK(band_tool == (incoming ? original.new_tool : original.initial_tool));
                    if (is_wall) {
                        if (low > cell0 + .001) {
                            CHECK(state.y() >= low - .001);
                            CHECK(state.y() + line.dist_Y(state) >= low - .001);
                        }
                        if (high < cell1 - .001) {
                            CHECK(state.y() <= high + .001);
                            CHECK(state.y() + line.dist_Y(state) <= high + .001);
                        }
                        return;
                    }
                    const double volume = line.dist_E(state) * area;
                    if (kind == "ram") {
                        CHECK_FALSE(incoming);
                        ram_volume += volume;
                    } else if (kind == "purge") {
                        CHECK(incoming);
                        purge_volume += volume;
                        ++purges;
                    }
                    if (merged && !recorded) {
                        planes[std::lround(state.z() * 10000.)].push_back({low, high, band_tool});
                        recorded = true;
                    }
                    if (merged && kind != "fill") {
                        // The tool switch cannot bisect a row, or send the ram
                        // to the far side of the next colour's band.
                        CHECK(line.dist_Y(state) == Approx(0.).margin(.001));
                        CHECK(state.y() >= low - .001);
                        CHECK(state.y() <= high + .001);
                    }
                });
                CHECK(t.skylane_safe_z >= highest_deposition + .199);
                if (original.is_tool_change) {
                    CHECK(ram_volume == Approx(config.filament_multitool_ramming_volume.values[original.initial_tool]).margin(.01));
                    CHECK(purge_volume == Approx(18.).margin(.01));
                }
            }
        CHECK(walls > 510);
        CHECK(purges > 510);
        CHECK(planes.size() > 20);
        CAPTURE(travel_length);
        // Budget the complete internal traversal, not just purge extrusion:
        // emitting both side walls after every band formerly exceeded 60 m.
        CHECK(travel_length < 45000.);
        CHECK(adjacent_moves > 100);
        for (auto& [height, bands] : planes) {
            CAPTURE(height);
            std::sort(bands.begin(), bands.end(), [](const Band& a, const Band& b) { return a.low < b.low; });
            std::set<int> colours;
            int           previous = -1;
            double        end      = -1.;
            for (const auto& band : bands) {
                CAPTURE(band.low, band.high, band.tool);
                CHECK(band.low >= end - .001);
                end = band.high;
                if (band.tool != previous) {
                    CHECK(colours.insert(band.tool).second);
                    previous = band.tool;
                }
            }
        }
    }
}

TEST_CASE("SkyLane allocates small ramming volumes and zero-prime visits without recolouring support", "[SkyLane][Packing][Colours]")
{
    SkyLane::Params p;
    p.width         = 30.;
    p.lanes         = 4;
    p.fill_density  = .2;
    p.purge_density = 1. / 1.2;
    p.solid_base    = true;
    double prime    = 18.;
    SECTION("Small outgoing ram") {}
    SECTION("No incoming prime") { prime = 0.; }
    const double                 ram[] = {0., .05, 7.};
    std::vector<SkyLane::Demand> demands;
    size_t                       tool = 0;
    for (int layer = 0; layer < 30; ++layer)
        for (int i = 0; i < 3; ++i) {
            SkyLane::Demand d;
            d.z              = float(.2 + .12 * layer);
            d.layer_h        = .12f;
            d.layer          = layer;
            d.old_tool       = tool;
            d.new_tool       = (tool + 1) % 3;
            d.ramming_volume = float(ram[tool]);
            d.volume         = float(prime + ram[tool]);
            demands.push_back(d);
            tool = d.new_tool;
        }
    const auto plan = SkyLane::pack_min_depth(demands, p);
    REQUIRE(plan.ok);
    REQUIRE(plan.over_z == 0);
    std::vector<double> ramming(demands.size(), 0.), purging(demands.size(), 0.);
    std::set<int>       switched;
    for (const auto& pass : plan.passes) {
        const auto& d = demands.at(pass.owner);
        CHECK(pass.z_base + pass.h <= d.z + 1e-6);
        if (pass.tool == d.new_tool)
            switched.insert(pass.owner);
        else {
            CHECK(pass.tool == d.old_tool);
            CHECK(switched.count(pass.owner) == 0);
        }
        if (pass.demand >= 0) {
            CHECK(pass.tool == (pass.ramming ? d.old_tool : d.new_tool));
            (pass.ramming ? ramming : purging).at(pass.owner) += p.width * (pass.y1 - pass.y0) * pass.h * pass.density;
        }
    }
    for (size_t i = 0; i < demands.size(); ++i) {
        CAPTURE(i);
        CHECK(ramming[i] == Approx(demands[i].ramming_volume).margin(.001));
        CHECK(purging[i] == Approx(prime).margin(.001));
    }
}

TEST_CASE("SkyLane lets the first colour return at the same height without creating an island", "[SkyLane][Packing][Colours]")
{
    SkyLane::Params p;
    p.width         = 30.;
    p.purge_density = 1. / 1.2;
    SkyLane::Lanes             lanes(p, 30.);
    std::vector<SkyLane::Pass> passes;
    for (size_t tool = 0; tool < 4; ++tool) {
        SkyLane::Demand d;
        d.z              = .2f;
        d.layer_h        = .08f;
        d.active         = 1;
        d.old_tool       = tool;
        d.new_tool       = (tool + 1) % 4;
        d.ramming_volume = 2.f;
        d.volume         = 10.f;
        REQUIRE(lanes.place(d, int(tool), passes));
    }
    // Four switches fit on one plane; the returning T0 joins its first ram
    // at the far end instead of forcing another thin tower layer.
    REQUIRE(passes.size() == 8);
    std::vector<SkyLane::Pass> first_tool;
    for (const auto& pass : passes) {
        CHECK(pass.z_base + pass.h == Approx(.2).margin(1e-6));
        if (pass.tool == 0)
            first_tool.push_back(pass);
    }
    REQUIRE(first_tool.size() == 2);
    CHECK(first_tool.front().y0 == Approx(first_tool.back().y1).margin(1e-6));
    CHECK(first_tool.front().y1 == Approx(30.));
}
