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

using namespace Slic3r;
using Catch::Approx;

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
                        CHECK(line.x() <= 20.001);
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
