#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "test_data.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;
using Catch::Approx;

TEST_CASE("Gloss painting reaches G-code with independent capped surface speeds", "[GlossPainting][GCode]")
{
    DynamicPrintConfig config = Test::default_print_config();
    config.set_key_value("outer_wall_speed", new ConfigOptionFloats{80.});
    config.set_key_value("top_surface_speed", new ConfigOptionFloats{80.});
    config.set_key_value("slow_down_for_layer_cooling", new ConfigOptionBools{false});
    config.set_key_value("slow_down_layers", new ConfigOptionInts{0});
    config.set_key_value("slow_down_min_speed", new ConfigOptionFloats{10.});
    config.set_key_value("enable_overhang_speed", new ConfigOptionBools{false});
    config.set_key_value("small_perimeter_speed", new ConfigOptionFloatsOrPercents{{80., false}});
    config.set_key_value("machine_max_speed_x", new ConfigOptionFloats{60., 60.});
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{30.});
    config.set_key_value("enable_arc_fitting", new ConfigOptionBool(false));
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));
    config.set_key_value("machine_start_gcode", new ConfigOptionString(""));
    config.set_key_value("machine_end_gcode", new ConfigOptionString(""));

    Print print;
    Model model;
    Test::init_print({TriangleMesh(its_make_cube(20., 20., 2.))}, print, model, config);
    ModelObject*     object  = model.objects.front();
    ModelVolume*     volume  = object->volumes.front();
    const auto&      mesh    = volume->mesh().its;
    const Vec3d      maximum = volume->mesh().bounding_box().max;
    TriangleSelector paint(volume->mesh());
    for (size_t facet = 0; facet < mesh.indices.size(); ++facet) {
        const auto& triangle = mesh.indices[facet];
        const bool  right    = mesh.vertices[triangle[0]].x() == maximum.x() && mesh.vertices[triangle[1]].x() == maximum.x() &&
                           mesh.vertices[triangle[2]].x() == maximum.x();
        const bool top = mesh.vertices[triangle[0]].z() == maximum.z() && mesh.vertices[triangle[1]].z() == maximum.z() &&
                         mesh.vertices[triangle[2]].z() == maximum.z();
        paint.set_facet(int(facet), static_cast<EnforcerBlockerType>(top ? 3 : right ? 2 : 1));
    }
    volume->gloss_facets.set(paint);
    object->config.set_key_value("gloss_paint_times", new ConfigOptionFloats{4., 2., 8., 10.});
    print.apply(model, config);
    const std::string                                   gcode = Test::gcode(print);
    std::array<std::vector<double>, GLOSS_PAINT_COLORS> speeds;
    int                                                 color = 0;
    GCodeReader                                         reader;
    reader.apply_config(print.config());
    reader.parse_buffer(gcode, [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
        const std::string comment(line.comment());
        if (comment.rfind("GLOSS_COLOR:", 0) == 0)
            color = std::stoi(comment.substr(12));
        if (color > 0 && line.extruding(state) && line.dist_XY(state) > 0.)
            speeds[size_t(color - 1)].push_back(line.new_F(state) / 60.);
    });
    for (size_t i = 0; i < 3; ++i) {
        INFO("Gloss color " << i + 1);
        REQUIRE_FALSE(speeds[i].empty());
        const double expected = i == 0 ? 25. : i == 1 ? 50. : 12.5;
        for (double speed : speeds[i])
            CHECK(speed == Approx(expected).margin(0.02));
    }
    REQUIRE(print.objects().front()->is_step_done(posSlice));
    object->config.set_key_value("gloss_paint_times", new ConfigOptionFloats{0.01, 2., 8., 10.});
    print.apply(model, config);
    // Palette edits only invalidate G-code; paint geometry remains usable.
    CHECK(print.objects().front()->is_step_done(posSlice));
    const std::string capped = Test::gcode(print);
    color                    = 0;
    reader.reset();
    size_t checked = 0;
    reader.parse_buffer(capped, [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
        const std::string comment(line.comment());
        if (comment.rfind("GLOSS_COLOR:", 0) == 0)
            color = std::stoi(comment.substr(12));
        if (color == 1 && line.extruding(state) && line.dist_XY(state) > 0.) {
            CHECK(line.new_F(state) / 60. <= 60.01);
            ++checked;
        }
    });
    CHECK(checked > 0);

    volume->gloss_facets.reset();
    print.apply(model, config);
    CHECK_FALSE(print.objects().front()->is_step_done(posSlice));
    CHECK(Test::gcode(print).find(";GLOSS_COLOR:") == std::string::npos);
}
