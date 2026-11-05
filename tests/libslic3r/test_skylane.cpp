#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "libslic3r/GCode/SkyLanePacker.hpp"
#include "libslic3r/GCode/SkyLaneTower.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PlaceholderParser.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include <cmath>
#include <limits>
#include <fstream>

using namespace Slic3r;
using Catch::Approx;

// Coordinates and change counts from the supplied October 2026 Dragon
// D2 (Rectangle), D4 (Cone) and D6 (Rib), relative to the tower origin.
TEST_CASE("SkyLane matches the archive's shared cells and wall types", "[SkyLane][Geometry]")
{
    std::ifstream counts(std::string(TEST_DATA_DIR) + "/skylane/dragon_changes.txt");
    REQUIRE(counts.good());
    std::vector<SkyLaneRequest>  requests;
    std::vector<SkyLane::Demand> demands;
    size_t                       layer = 0, tool = 0;
    int                          n;
    while (counts >> n) {
        for (int i = 0; i < n; ++i) {
            const double z = .25 + layer * .22, h = layer == 0 ? .25 : .22;
            requests.push_back({layer, false, tool, 1 - tool, z, h, 38.});
            demands.push_back({z, h, 43., layer});
            tool = 1 - tool;
        }
        ++layer;
    }
    REQUIRE(requests.size() == 395);
    SkyLane::Parameters p;
    p.width           = 28.;
    p.purge_density   = 1. / 1.2;
    const auto packed = SkyLane::plan(demands, p);
    REQUIRE(packed.ok);
    CHECK(packed.lanes == 2);
    CHECK(packed.depth == Approx(28.8997).margin(.001));
    REQUIRE(packed.passes.size() > 3);
    CHECK(packed.passes[0].wall);
    CHECK_FALSE(packed.passes[1].wall);
    CHECK(packed.passes[0].y1 == Approx(43. / (28. * .25 / 1.2)));

    auto dynamic = DynamicPrintConfig::full_print_config();
    dynamic.set_num_extruders(2);
    dynamic.set_num_filaments(2);
    dynamic.set("single_extruder_multi_material", false);
    dynamic.set("enable_skylane", true);
    dynamic.set("enable_tower_interface_features", false);
    dynamic.set("prime_tower_width", 28.);
    dynamic.set("prime_tower_brim_width", 5.);
    dynamic.set("skylane_max_dip", .4);
    dynamic.option<ConfigOptionPercent>("wipe_tower_extra_spacing")->value = 120.;
    dynamic.set("wipe_tower_extra_rib_length", 8.);
    dynamic.set("wipe_tower_rib_width", 8.);
    dynamic.set("wipe_tower_fillet_wall", true);
    dynamic.set("wipe_tower_wall_gap", true);
    dynamic.set("wipe_tower_cone_angle", 15.);
    dynamic.option<ConfigOptionBools>("filament_multitool_ramming")->values         = {true, true};
    dynamic.option<ConfigOptionFloats>("filament_multitool_ramming_volume")->values = {5., 5.};
    dynamic.option<ConfigOptionFloats>("filament_diameter")->values                 = {1.75, 1.75};
    PrintConfig       config;
    PrintRegionConfig region;
    config.apply(dynamic, true);
    region.apply(dynamic, true);
    std::vector<SkyLaneTowerResult> towers;
    for (auto type : {WipeTowerWallType::wtwRectangle, WipeTowerWallType::wtwCone, WipeTowerWallType::wtwRib}) {
        config.wipe_tower_wall_type.value = type;
        towers.push_back(generate_skylane_tower(config, region, requests, layer));
        const auto& tower = towers.back();
        CHECK(tower.depth == Approx(28.8997).margin(.001));
        CHECK(tower.toolchanges == 395);
        const auto& first  = tower.nominal[0][0];
        const auto& second = tower.nominal[0][1];
        CHECK(first.gcode.find("SKYLANE WALL") != std::string::npos);
        CHECK(second.gcode.find("SKYLANE WALL") == std::string::npos);
        // This strip has horizontal purge rows only. The source's row turns
        // travel without extrusion, so no deposited segment may step in Y.
        double min_y = 1e6, max_y = -1e6;
        for (size_t j = 1; j < second.extrusions.size(); ++j) {
            const auto& e = second.extrusions[j];
            if (e.width <= 0.)
                continue;
            const auto& a = second.extrusions[j - 1];
            CHECK(e.pos.y() == Approx(a.pos.y()).margin(1e-5));
            min_y = std::min(min_y, double(e.pos.y()));
            max_y = std::max(max_y, double(e.pos.y()));
        }
        CHECK(min_y == Approx(205.170 - 197.549).margin(.002));
        CHECK(max_y == Approx(212.042 - 197.549).margin(.002));
        CHECK(tower.outer_wall.begin()->second.front().is_closed());

        auto path_measure = [&](const std::string& raw, bool volume) {
            PlaceholderParser parser;
            parser.set("z_offset", 0.);
            GCodeReader reader;
            reader.apply_config(config);
            double total = 0.;
            reader.parse_buffer(parser.process(raw), [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
                if (line.extruding(state))
                    total += volume ? line.dist_E(state) * M_PI * 1.75 * 1.75 / 4. : line.dist_XY(state);
            });
            return total;
        };
        const size_t wall_start = first.gcode.find("; SKYLANE WALL\n");
        const size_t wall_end   = first.gcode.find("; SKYLANE WALL_END", wall_start);
        REQUIRE(wall_end != std::string::npos);
        const double perimeter = unscaled(tower.outer_wall.begin()->second.front().length());
        CHECK(path_measure(first.gcode.substr(wall_start, wall_end - wall_start), false) == Approx(perimeter - 1.25).margin(.003));
        const auto&  ram    = tower.nominal[1][1].gcode;
        const size_t change = ram.find("[change_filament_gcode]");
        REQUIRE(change != std::string::npos);
        CHECK(path_measure(ram.substr(0, change), true) == Approx(5.).margin(.002));
    }
    CHECK(towers[0].nominal[0][0].gcode == towers[1].nominal[0][0].gcode);
    const auto rect = get_extents(towers[0].outer_wall.begin()->second.front());
    CHECK(unscaled(rect.min.x()) == Approx(.5));
    CHECK(unscaled(rect.max.x()) == Approx(28.5));
    CHECK(unscaled(rect.min.y()) == Approx(.25));
    CHECK(unscaled(rect.max.y()) == Approx(28.6497).margin(.001));
    const auto rib = get_extents(towers[2].outer_wall.begin()->second.front());
    CHECK(unscaled(rib.min.x()) == Approx(11.248 - 18.380).margin(.01));
    CHECK(unscaled(rib.max.x()) == Approx(54.512 - 18.380).margin(.01));
    const auto rect_top = get_extents(towers[0].outer_wall.rbegin()->second.front());
    const auto rib_top  = get_extents(towers[2].outer_wall.rbegin()->second.front());
    CHECK(unscaled(rect_top.size().y()) == Approx(11.5599 - .5).margin(.002));
    CHECK(unscaled(rib_top.size().y()) == Approx(28.8997 - .5).margin(.002));

    config.wipe_tower_wall_type.value   = WipeTowerWallType::wtwRib;
    config.wipe_tower_fillet_wall.value = false;
    config.wipe_tower_wall_gap.value    = false;
    const auto  sharp                   = generate_skylane_tower(config, region, requests, layer);
    const auto& sharp_wall              = sharp.outer_wall.begin()->second.front();
    CHECK(sharp_wall.points.size() < towers[2].outer_wall.begin()->second.front().points.size());
    CHECK(get_extents(sharp_wall).min.x() < rib.min.x());

    // The archive's shallow Dragon has no cone. Also exercise a protruding one.
    config.wipe_tower_wall_type.value  = WipeTowerWallType::wtwCone;
    config.wipe_tower_cone_angle.value = 60.;
    const auto cone                    = generate_skylane_tower(config, region, requests, layer);
    CHECK(cone.bounds.size().y() > towers[0].bounds.size().y() + 10.);
    CHECK(get_extents(cone.outer_wall.begin()->second.front()).size().y() >
          get_extents(cone.outer_wall.rbegin()->second.front()).size().y());
}

TEST_CASE("SkyLane is opt-in and saved in process presets", "[SkyLane][Config]")
{
    const auto defaults = DynamicPrintConfig::full_print_config();
    CHECK_FALSE(defaults.opt_bool("enable_skylane"));
    const auto& keys = Preset::print_options();
    for (const auto* key : {"enable_skylane", "skylane_max_dip", "skylane_ramp_volume"}) {
        CHECK(std::find(keys.begin(), keys.end(), key) != keys.end());
        DynamicPrintConfig restored;
        restored.set_deserialize_strict(key, defaults.option(key)->serialize());
        CHECK(*restored.option(key) == *defaults.option(key));
    }
}

TEST_CASE("SkyLane preserves purge volumes and deposition bounds", "[SkyLane]")
{
    SkyLane::Parameters          p;
    std::vector<SkyLane::Demand> demands;
    for (size_t layer = 0; layer < 40; ++layer) {
        const size_t changes = layer % 7 == 0 ? 8 : 2;
        for (size_t change = 0; change < changes; ++change)
            demands.push_back({.2 + .08 * layer, .08, double(4 + (layer * 17 + change * 13) % 21), layer});
    }
    auto plan = SkyLane::plan(demands, p);
    REQUIRE(plan.ok);
    CHECK(plan.lanes >= 2);
    CHECK(plan.lanes <= 6);
    CHECK(plan.depth > 0.);
    std::vector<double> volumes(demands.size(), 0.);
    size_t              owner  = 0;
    bool                dipped = false;
    for (const auto& pass : plan.passes) {
        CAPTURE(pass.owner, pass.demand, pass.z_base, pass.height, pass.y0, pass.y1);
        REQUIRE(pass.owner < demands.size());
        CHECK(pass.owner >= owner);
        owner = pass.owner;
        CHECK(pass.height > 0.);
        CHECK(pass.height <= p.max_height + 1e-7);
        CHECK(pass.z() <= demands[owner].z + 1e-7);
        CHECK(pass.y0 >= 0.);
        CHECK(pass.y1 <= plan.depth + 1e-7);
        CHECK(pass.y1 > pass.y0);
        if (pass.demand >= 0) {
            CHECK(demands[owner].z - pass.z_base <= p.max_dip + 1e-7);
            volumes.at(size_t(pass.demand)) += p.width * (pass.y1 - pass.y0) * pass.height * pass.density;
            dipped |= pass.z() < demands[owner].z - .01;
        }
    }
    CHECK(dipped);
    for (size_t i = 0; i < demands.size(); ++i) {
        CAPTURE(i);
        CHECK(volumes[i] == Approx(demands[i].volume).margin(1e-6));
    }
    const auto repeat = SkyLane::plan(demands, p);
    CHECK(repeat.depth == plan.depth);
    CHECK(repeat.lanes == plan.lanes);
    CHECK(repeat.passes.size() == plan.passes.size());
    // A dipped nozzle must clear already deposited material in adjacent lanes,
    // including support/catch-up passes, not just the purge passes.
    for (size_t i = 0; i < plan.passes.size(); ++i) {
        const auto& pass = plan.passes[i];
        for (size_t j = 0; j < i; ++j) {
            const auto& previous = plan.passes[j];
            if (previous.z() <= pass.z() + 1e-7)
                continue;
            const double gap      = std::max(pass.y0 - previous.y1, previous.y0 - pass.y1);
            const double required = p.tip_radius - p.line_width / 2. +
                                    (previous.z() - pass.z()) * std::tan(p.nozzle_cone_angle * M_PI / 180.);
            CAPTURE(i, j, gap, required);
            CHECK(gap >= required - 1e-7);
        }
    }
}

TEST_CASE("SkyLane fills quiet stretches and rejects invalid inputs", "[SkyLane]")
{
    SkyLane::Parameters p;
    const auto          quiet = SkyLane::plan({{.2, .2, 20., 0}, {5., .2, 0., 1}, {5.2, .2, 30., 2}}, p);
    REQUIRE(quiet.ok);
    CHECK(std::any_of(quiet.passes.begin(), quiet.passes.end(),
                      [](const auto& pass) { return pass.owner == 1 && pass.demand == -1 && pass.height > 0.; }));
    CHECK(SkyLane::plan({}, p).passes.empty());
    CHECK_FALSE(SkyLane::pack_scheduled({{.2, .2, 1e6, 0}}, 10., p).ok);
    CHECK_THROWS_AS(SkyLane::plan({{.2, .2, -1., 0}}, p), std::invalid_argument);
    CHECK_THROWS_AS(SkyLane::plan({{.4, .2, 1., 0}, {.2, .2, 1., 1}}, p), std::invalid_argument);
    p.max_dip = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS(SkyLane::plan({}, p), std::invalid_argument);
}

TEST_CASE("SkyLane emits enough material with bounded flow and clearance", "[SkyLane][GCode]")
{
    auto dynamic = DynamicPrintConfig::full_print_config();
    dynamic.set_num_extruders(2);
    dynamic.set_num_filaments(2);
    dynamic.set("single_extruder_multi_material", false);
    dynamic.set("enable_skylane", true);
    dynamic.set("prime_tower_width", 20.);
    dynamic.option<ConfigOptionEnum<WipeTowerWallType>>("wipe_tower_wall_type")->value = WipeTowerWallType::wtwRectangle;
    dynamic.set("prime_tower_brim_width", 0.);
    dynamic.set("enable_tower_interface_features", false);
    dynamic.option<ConfigOptionFloats>("filament_diameter")->values             = {1.75, 1.75};
    dynamic.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {10., 10.};
    dynamic.option<ConfigOptionBools>("filament_multitool_ramming")->values     = {false, false};
    PrintConfig       config;
    PrintRegionConfig region;
    config.apply(dynamic, true);
    region.apply(dynamic, true);
    std::vector<SkyLaneRequest> requests;
    size_t                      tool = 0;
    for (size_t layer = 0; layer < 8; ++layer)
        for (int change = 0; change < 3; ++change) {
            requests.push_back({layer, change < 2, tool, 1 - tool, .2 + layer * .08, .08, 15.});
            tool = 1 - tool;
        }
    auto tower = generate_skylane_tower(config, region, requests, 8);
    CHECK(tower.toolchanges == requests.size());
    CHECK(tower.brim == 0.);
    const double area    = M_PI * 1.75 * 1.75 / 4.;
    double       total_e = 0.;
    for (const auto* layers : {&tower.local_z, &tower.nominal}) {
        for (const auto& layer : *layers) {
            for (const auto& change : layer) {
                PlaceholderParser parser;
                parser.set("z_offset", 0.);
                parser.set("change_filament_gcode", std::string("; change\n"));
                const std::string gcode = parser.process(change.gcode);
                GCodeReader       reader;
                reader.apply_config(config);
                reader.x() = change.start_pos.x();
                reader.y() = change.start_pos.y();
                double e   = 0.;
                reader.parse_buffer(gcode, [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
                    if (line.extruding(state) && line.dist_XY(state) > 1e-5) {
                        CHECK(state.z() <= change.print_z + 1e-5);
                        CHECK(state.z() > 0.);
                        const double volume = line.dist_E(state) * area;
                        const double flow   = volume / line.dist_XY(state) * (line.has_f() ? line.f() : state.f()) / 60.;
                        CHECK(flow <= 10.02); // Output rounding.
                        const double ramp_limit = 10. * (.25 + .75 * std::min(1., e * area / 4.));
                        CHECK(flow <= ramp_limit + .02);
                        e += line.dist_E(state);
                        CHECK(line.x() >= -.001);
                        CHECK(line.x() <= 20.501);
                        CHECK(line.y() >= -.001);
                        CHECK(line.y() <= tower.depth + .001);
                    }
                });
                CHECK(e * area >= change.purge_volume - .01);
                CHECK(reader.z() == Approx(change.skylane_safe_z).margin(1e-5));
                CHECK(reader.x() == Approx(change.end_pos.x()));
                CHECK(reader.y() == Approx(change.end_pos.y()));
                total_e += e;
            }
        }
    }
    CHECK(total_e == Approx(tower.used_filament[0] + tower.used_filament[1]).margin(.02));
    config.skylane_max_dip.value = .1;
    const auto shallow           = generate_skylane_tower(config, region, requests, 8);
    for (const auto* layers : {&shallow.local_z, &shallow.nominal})
        for (const auto& layer : *layers)
            for (const auto& change : layer)
                for (const auto& extrusion : change.extrusions)
                    if (extrusion.width > 0.)
                        CHECK(extrusion.height <= .100001f);
    config.wipe_tower_no_sparse_layers.value = true;
    CHECK_THROWS_AS(generate_skylane_tower(config, region, requests, 8), std::invalid_argument);
}
