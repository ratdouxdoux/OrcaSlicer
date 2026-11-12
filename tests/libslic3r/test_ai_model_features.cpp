#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/AIModelFeatures.hpp"
#include "libslic3r/AIModelTools.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include <set>

using namespace Slic3r;
namespace AI = Slic3r::AIPrintAssistant;
using json   = nlohmann::json;
using Catch::Matchers::WithinAbs;

namespace {
DynamicPrintConfig settings(double line_width = 0.45)
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(line_width, false));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("top_shell_layers", new ConfigOptionInt(3));
    config.set_key_value("bottom_shell_layers", new ConfigOptionInt(3));
    return config;
}

TriangleMesh box(double x, double y, double z, const Vec3d& at = Vec3d::Zero())
{
    auto mesh = make_cube(x, y, z);
    mesh.translate(at.cast<float>());
    return mesh;
}

struct Fixture
{
    Model        model;
    ModelObject* object = model.add_object();

    ModelVolume& add(TriangleMesh mesh, ModelVolumeType type = ModelVolumeType::MODEL_PART)
    {
        return *object->add_volume(std::move(mesh), type, false);
    }
    AI::FeatureReport detect(const DynamicPrintConfig& config = settings())
    {
        if (object->instances.empty())
            object->add_instance();
        object->invalidate_bounding_box();
        return AI::detect_model_features(*object, 0, config);
    }
};

std::vector<const AI::ModelFeature*> of(const AI::FeatureReport& report, const std::string& kind)
{
    std::vector<const AI::ModelFeature*> result;
    for (const auto& feature : report.features)
        if (feature.kind == kind)
            result.push_back(&feature);
    return result;
}
// The pointers would outlive a temporary report.
std::vector<const AI::ModelFeature*> of(AI::FeatureReport&&, const std::string&) = delete;

TriangleMesh tilted(TriangleMesh mesh, double degrees)
{
    mesh.transform(Transform3d(Eigen::AngleAxisd(degrees * PI / 180., Vec3d::UnitY())));
    return mesh;
}

ExPolygons slice_at(const indexed_triangle_set& its, double z, MeshSlicingParams::SlicingMode mode = MeshSlicingParams::SlicingMode::Regular)
{
    MeshSlicingParamsEx params;
    params.mode = mode;
    return slice_mesh_ex(its, {float(z)}, params).front();
}

bool covers(const indexed_triangle_set& its, double x, double y, double z)
{
    const auto slice = slice_at(its, z);
    return std::any_of(slice.begin(), slice.end(), [&](const ExPolygon& shape) { return shape.contains(Point::new_scale(x, y)); });
}

double area_mm2(const ExPolygons& shapes)
{
    double area = 0.;
    for (const auto& shape : shapes)
        area += shape.area();
    return area * SCALING_FACTOR * SCALING_FACTOR;
}
} // namespace

TEST_CASE("Assistant features of a plain cube", "[AIPrintAssistant][fast]")
{
    Fixture f;
    f.add(box(20, 20, 20));
    const auto report = f.detect();
    for (const auto* kind : {"overhang", "bridge", "floating", "thin_wall", "fine_detail", "small_section", "thin_plate", "vertical_hole",
                             "cavity", "narrow_gap", "curved_surface", "shallow_slope", "horizontal_hole"}) {
        INFO(kind);
        CHECK(of(report, kind).empty());
    }
    const auto tops = of(report, "flat_top");
    REQUIRE(tops.size() == 1);
    CHECK_THAT(tops[0]->measures["area_mm2"].get<double>(), WithinAbs(400., 0.1));
    CHECK_THAT(tops[0]->measures["z_mm"].get<double>(), WithinAbs(20., 0.01));
    const auto edges = of(report, "sharp_edge");
    REQUIRE(edges.size() == 4);
    for (const auto* edge : edges)
        CHECK(edge->measures["convex"] == true);
    const auto bed = of(report, "bed_contact");
    REQUIRE(bed.size() == 1);
    CHECK_THAT(bed[0]->measures["area_mm2"].get<double>(), WithinAbs(400., 1.));
    CHECK_THAT(bed[0]->measures["height_to_base"].get<double>(), WithinAbs(1., 0.01));
    CHECK(bed[0]->measures["sharp_corners"] == 4);
}

TEST_CASE("Assistant grades overhangs and finds bridges and floating areas", "[AIPrintAssistant][fast]")
{
    SECTION("A T-shape overhangs on both sides")
    {
        Fixture f;
        auto    shape = box(10, 10, 10, {10, 0, 0});
        MeshBoolean::cgal::plus(shape, box(30, 10, 2, {0, 0, 10}));
        f.add(std::move(shape));
        const auto report    = f.detect();
        const auto overhangs = of(report, "overhang");
        REQUIRE(overhangs.size() == 2);
        for (const auto* overhang : overhangs) {
            CHECK_THAT(overhang->measures["area_mm2"].get<double>(), WithinAbs(100., 0.5));
            CHECK(overhang->measures["needs_support"] == true);
            CHECK(overhang->measures["overhang_speed_setting"] == "overhang_4_4_speed");
            CHECK(overhang->measures["bridged_percent"] == 0.);
        }
        CHECK(of(report, "bridge").empty());
    }
    SECTION("A slab between two pillars is a bridge")
    {
        Fixture f;
        f.add(box(5, 10, 10));
        f.add(box(5, 10, 10, {15, 0, 0}));
        f.add(box(20, 10, 2, {0, 0, 10}));
        const auto report  = f.detect();
        const auto bridges = of(report, "bridge");
        REQUIRE(bridges.size() == 1);
        CHECK_THAT(bridges[0]->box.min.z(), WithinAbs(10., 0.21));
        CHECK_THAT(bridges[0]->measures["span_mm"].get<double>(), WithinAbs(10., 1.));
        const double direction = std::fmod(bridges[0]->measures["direction_deg"].get<double>() + 360., 180.);
        CHECK((direction < 1. || direction > 179.));
        REQUIRE(of(report, "overhang").size() == 1);
        CHECK(of(report, "overhang")[0]->measures["bridged_percent"].get<double>() > 80.);
    }
    SECTION("A part that starts in mid-air floats")
    {
        Fixture f;
        f.add(box(5, 5, 10));
        f.add(box(5, 5, 5, {20, 0, 5}));
        const auto report   = f.detect();
        const auto floating = of(report, "floating");
        REQUIRE(floating.size() == 1);
        CHECK_THAT(floating[0]->box.min.z(), WithinAbs(5., 0.21));
        CHECK_THAT(floating[0]->box.min.x(), WithinAbs(20., 0.01));
    }
}

TEST_CASE("Assistant finds thin walls, fine detail, small sections and thin plates", "[AIPrintAssistant][fast]")
{
    SECTION("A 0.6 mm wall is thin for 0.45 mm lines but not for 0.25 mm lines")
    {
        Fixture f;
        f.add(box(20, 0.6, 5));
        const auto report = f.detect(settings(0.45));
        REQUIRE(of(report, "thin_wall").size() == 1);
        CHECK(of(report, "fine_detail").empty());
        const auto finer = f.detect(settings(0.25));
        CHECK(of(finer, "thin_wall").empty());
    }
    SECTION("A 0.3 mm wall is finer than a 0.45 mm line")
    {
        Fixture f;
        f.add(box(20, 0.3, 5));
        const auto report = f.detect();
        CHECK(of(report, "fine_detail").size() == 1);
    }
    SECTION("A pin on a base is a small section")
    {
        Fixture f;
        f.add(box(20, 20, 2));
        auto pin = make_cylinder(1.5, 10);
        pin.translate(10, 10, 2);
        f.add(std::move(pin));
        const auto report = f.detect();
        const auto small  = of(report, "small_section");
        REQUIRE(small.size() == 1);
        CHECK_THAT(small[0]->box.min.z(), WithinAbs(2., 0.21));
        CHECK_THAT(small[0]->box.max.z(), WithinAbs(12., 0.21));
    }
    SECTION("A three-layer plate is thinner than six shell layers")
    {
        Fixture f;
        f.add(box(20, 20, 0.6));
        const auto report = f.detect();
        const auto plates = of(report, "thin_plate");
        REQUIRE(plates.size() == 1);
        CHECK_THAT(plates[0]->measures["thinnest_mm"].get<double>(), WithinAbs(0.6, 0.01));
    }
}

TEST_CASE("Assistant finds holes, cavities and narrow gaps", "[AIPrintAssistant][fast]")
{
    SECTION("A through hole and a blind hole")
    {
        Fixture f;
        auto    shape = box(20, 20, 20);
        auto    hole  = make_cylinder(3, 30);
        hole.translate(6, 10, -5);
        MeshBoolean::cgal::minus(shape, hole);
        auto blind = make_cylinder(2, 15);
        blind.translate(15, 10, 10);
        MeshBoolean::cgal::minus(shape, blind);
        f.add(std::move(shape));
        const auto report = f.detect();
        auto       holes  = of(report, "vertical_hole");
        REQUIRE(holes.size() == 2);
        std::sort(holes.begin(), holes.end(), [](auto* a, auto* b) { return a->box.min.x() < b->box.min.x(); });
        CHECK_THAT(holes[0]->measures["diameter_mm"].get<double>(), WithinAbs(6., 0.1));
        CHECK(holes[0]->measures["open_at_bottom"] == true);
        CHECK(holes[0]->measures["open_at_top"] == true);
        CHECK_THAT(holes[1]->measures["diameter_mm"].get<double>(), WithinAbs(4., 0.1));
        CHECK(holes[1]->measures["open_at_bottom"] == false);
        CHECK(holes[1]->measures["open_at_top"] == true);
    }
    SECTION("A negative sphere inside a cube is a cavity")
    {
        Fixture f;
        f.add(box(20, 20, 20));
        auto sphere = make_sphere(4);
        sphere.translate(10, 10, 10);
        f.add(std::move(sphere), ModelVolumeType::NEGATIVE_VOLUME);
        const auto report = f.detect();
        REQUIRE(of(report, "cavity").size() == 1);
        CHECK(of(report, "vertical_hole").empty());
        // The cavity's ceiling is a printed surface inside the part.
        CHECK_FALSE(of(report, "overhang").empty());
    }
    SECTION("A horizontal through hole")
    {
        Fixture f;
        auto    shape = box(20, 20, 20);
        auto    hole  = make_cylinder(3, 30);
        hole.rotate_y(float(PI / 2.));
        hole.translate(-5, 10, 10);
        MeshBoolean::cgal::minus(shape, hole);
        f.add(std::move(shape));
        const auto report = f.detect();
        const auto holes  = of(report, "horizontal_hole");
        REQUIRE(holes.size() == 1);
        CHECK_THAT(holes[0]->measures["diameter_mm"].get<double>(), WithinAbs(6., 0.1));
        CHECK(holes[0]->measures["through"] == true);
        CHECK_THAT(std::abs(holes[0]->measures["axis"][0].get<double>()), WithinAbs(1., 0.01));
        CHECK(of(report, "vertical_hole").empty());
    }
    SECTION("Two parts 0.2 mm apart leave a narrow gap")
    {
        Fixture f;
        f.add(box(10, 10, 5));
        f.add(box(10, 10, 5, {10.2, 0, 0}));
        const auto report = f.detect();
        const auto gaps   = of(report, "narrow_gap");
        REQUIRE(gaps.size() == 1);
        CHECK_THAT(gaps[0]->box.max.z(), WithinAbs(5., 0.01));
    }
}

TEST_CASE("Assistant finds curved surfaces and shallow slopes", "[AIPrintAssistant][fast]")
{
    SECTION("A cylinder's wall is curved")
    {
        Fixture f;
        f.add(make_cylinder(10, 10));
        const auto report = f.detect();
        const auto curved = of(report, "curved_surface");
        REQUIRE(curved.size() == 1);
        CHECK(curved[0]->measures["orientation"] == "wall");
        CHECK(curved[0]->measures["normal_spread_deg"].get<double>() > 170.);
        CHECK(of(report, "sharp_edge").empty());
    }
    SECTION("A top tilted by 10 degrees is shallow, by 40 degrees it is not")
    {
        for (const double tilt : {10., 40.}) {
            Fixture f;
            auto    cube = box(20, 20, 20);
            cube.rotate_y(float(tilt * PI / 180.));
            f.add(std::move(cube));
            const auto report  = f.detect();
            const auto shallow = of(report, "shallow_slope");
            if (tilt == 10.) {
                REQUIRE(shallow.size() == 1);
                CHECK_THAT(shallow[0]->measures["slope_deg"][0].get<double>(), WithinAbs(10., 0.1));
            } else
                CHECK(shallow.empty());
        }
    }
}

TEST_CASE("Assistant finds material boundaries, painted regions and text", "[AIPrintAssistant][fast]")
{
    SECTION("Parts with different filaments meet")
    {
        Fixture f;
        f.add(box(10, 10, 5));
        f.add(box(10, 10, 5, {10, 0, 0})).config.set_key_value("extruder", new ConfigOptionInt(2));
        const auto report     = f.detect();
        const auto boundaries = of(report, "material_boundary");
        REQUIRE(boundaries.size() == 1);
        CHECK(boundaries[0]->measures["filaments"] == json::array({1, 2}));
    }
    SECTION("Painting and text from the model tools")
    {
        Model                    model;
        auto*                    cube = model.add_object();
        MixedFilamentManager     mixes;
        std::vector<std::string> colors{"#FF0000", "#0000FF"};
        cube->add_volume(box(20, 20, 20), ModelVolumeType::MODEL_PART, false);
        cube->add_instance();
        auto run = [&](const std::string& action, const json& parameters, bool part) {
            const auto context   = AI::capture_model_tool_context(model, {}, colors, mixes);
            const auto operation = AI::parse_model_operation({{"context_id", context.id},
                                                              {"operation", action},
                                                              {"object_id", std::to_string(cube->id().id)},
                                                              {"volume_id", part ? std::to_string(cube->volumes[0]->id().id) : ""},
                                                              {"parameters", parameters}});
            model = AI::prepare_model_edit(model, mixes, colors, operation, AI_ASSISTANT_RESOURCE_DIR).model;
            cube  = model.objects.front();
        };
        run("paint_surface", {{"channel", "color"}, {"state", "paint"}, {"face", "top"}, {"filament_id", 2}}, true);
        run("add_text", {{"text", "hi"}, {"face", "front"}, {"height_mm", 5}, {"depth_mm", 0.6}}, false);
        cube->invalidate_bounding_box();
        const auto report  = AI::detect_model_features(*cube, 0, settings());
        const auto painted = of(report, "painted_region");
        REQUIRE(painted.size() == 1);
        CHECK(painted[0]->measures["filament"] == 2);
        CHECK_THAT(painted[0]->box.min.z(), WithinAbs(20., 0.01));
        const auto text = of(report, "text");
        REQUIRE(text.size() == 1);
        CHECK(text[0]->measures["text"] == "hi");
        CHECK(text[0]->measures["mode"] == "raised");
    }
}

TEST_CASE("Assistant feature reports are compact, filtered and stable", "[AIPrintAssistant][fast]")
{
    Fixture f;
    f.add(box(20, 20, 20));
    const auto report = f.detect();
    CHECK(f.detect().geometry_key == report.geometry_key);
    const auto all = AI::feature_report_json(report);
    CHECK(all["counts"]["sharp_edge"] == 4);
    CHECK(all["definitions"].contains("sharp_edge"));
    CHECK_FALSE(all["definitions"].contains("bridge"));
    const auto limited = AI::feature_report_json(report, {"sharp_edge"}, 2);
    CHECK(limited["features"].size() == 2);
    CHECK(limited["omitted"] == 2);
    CHECK(limited["features"][0]["id"] == "sharp_edge-1");
    CHECK_THROWS(AI::feature_report_json(report, {"invented"}));
    // Detection depends on the settings it grades with.
    CHECK(AI::feature_geometry_key(*f.object, 0, settings(0.3)) != report.geometry_key);
    // A modifier does not change the detected geometry.
    f.add(box(1, 1, 1), ModelVolumeType::PARAMETER_MODIFIER);
    CHECK(AI::feature_geometry_key(*f.object, 0, settings()) == report.geometry_key);
}

TEST_CASE("Assistant builds closed modifier meshes around features", "[AIPrintAssistant][fast]")
{
    Fixture f;
    auto    shape = box(10, 10, 10, {10, 0, 0});
    MeshBoolean::cgal::plus(shape, box(30, 10, 2, {0, 0, 10}));
    f.add(std::move(shape));
    const auto report   = f.detect();
    const auto overhang = of(report, "overhang").front();
    for (const bool footprint : {false, true}) {
        INFO(footprint);
        const auto its    = AI::feature_modifier_mesh(*overhang, footprint, 1.);
        const auto bounds = BoundingBoxf3(TriangleMesh(its).bounding_box().min.cast<double>(),
                                          TriangleMesh(its).bounding_box().max.cast<double>());
        CHECK(its_volume(its) > 0.);
        CHECK(TriangleMesh(its).stats().open_edges == 0);
        CHECK(bounds.min.z() <= overhang->box.min.z() - 0.99);
        CHECK(bounds.max.z() >= overhang->box.max.z() + 0.99);
        CHECK_THAT(bounds.min.x(), WithinAbs(overhang->box.min.x() - 1., 0.3));
        CHECK_THAT(bounds.max.x(), WithinAbs(overhang->box.max.x() + 1., 0.3));
    }
}

TEST_CASE("Assistant mesh modifiers follow sloped features", "[AIPrintAssistant][fast]")
{
    SECTION("A tilted overhang gets a slab along its face")
    {
        // Tilted 40 degrees, the bottom face runs from (0, 0) down to the bed at
        // x = 20 cos 40, z = -20 sin 40. Its centre is at x = 7.66, z = -6.43.
        Fixture f;
        f.add(tilted(box(20, 20, 20), 40.));
        const auto report    = f.detect();
        const auto overhangs = of(report, "overhang");
        REQUIRE(overhangs.size() == 1);
        const auto& overhang = *overhangs.front();
        CHECK(overhang.slabs.size() > 10);
        const auto slab  = AI::feature_modifier_mesh(overhang, true, 1.);
        const auto block = AI::feature_modifier_mesh(overhang, false, 1.);
        CHECK(TriangleMesh(slab).stats().open_edges == 0);
        CHECK(its_volume(slab) > 0.);
        CHECK(its_volume(slab) < 0.5 * its_volume(block));
        CHECK(covers(slab, 7.66, 10., -6.43));
        // Six millimetres into the cube and six out into the air, the block
        // still covers but the slab does not.
        for (const double x : {1.66, 13.66}) {
            INFO(x);
            CHECK(covers(block, x, 10., -6.43));
            CHECK_FALSE(covers(slab, x, 10., -6.43));
        }
        // The prisms touch without overlapping, so even-odd slicing reads them
        // the same as the default.
        for (double z = -13.5; z < 0.9; z += 0.37) {
            INFO(z);
            CHECK_THAT(area_mm2(slice_at(slab, z, MeshSlicingParams::SlicingMode::EvenOdd)), WithinAbs(area_mm2(slice_at(slab, z)), 0.01));
        }
    }
    SECTION("A leaning thin wall follows its slices")
    {
        Fixture f;
        f.add(tilted(box(0.6, 20, 20), 30.));
        const auto report = f.detect();
        const auto thin   = of(report, "thin_wall");
        REQUIRE(thin.size() == 1);
        CHECK(thin[0]->slabs.size() > 10);
        const auto slab = AI::feature_modifier_mesh(*thin[0], true, 1.);
        CHECK(TriangleMesh(slab).stats().open_edges == 0);
        CHECK(its_volume(slab) < 0.5 * its_volume(AI::feature_modifier_mesh(*thin[0], false, 1.)));
        // Halfway up, the wall's centre line is at x = 10 tan 30.
        CHECK(covers(slab, 0.3 + 8.66 * std::tan(PI / 6.), 10., 8.66));
        CHECK_FALSE(covers(slab, 0.3 + 8.66 * std::tan(PI / 6.) + 5., 10., 8.66));
    }
    SECTION("A straight wall stays one prism")
    {
        Fixture f;
        f.add(make_cylinder(10, 10));
        const auto report = f.detect();
        const auto curved = of(report, "curved_surface");
        REQUIRE(curved.size() == 1);
        const auto      prism = AI::feature_modifier_mesh(*curved[0], true, 1.);
        std::set<float> heights;
        for (const auto& v : prism.vertices)
            heights.insert(v.z());
        CHECK(heights.size() == 2);
        // A ring around the wall, open in the middle.
        CHECK(covers(prism, 10., 0., 5.));
        CHECK_FALSE(covers(prism, 0., 0., 5.));
    }
}

TEST_CASE("Assistant adds and edits feature modifiers through model operations", "[AIPrintAssistant][fast]")
{
    Model                    model;
    auto*                    object = model.add_object();
    MixedFilamentManager     mixes;
    std::vector<std::string> colors{"#FF0000"};
    auto                     shape = box(10, 10, 10, {10, 0, 0});
    MeshBoolean::cgal::plus(shape, box(30, 10, 2, {0, 0, 10}));
    object->add_volume(std::move(shape), ModelVolumeType::MODEL_PART, false);
    object->add_instance()->set_offset({50, 50, 0});
    object->invalidate_bounding_box();
    const auto config   = settings();
    const auto features = AI::detect_model_features(*object, 0, config);
    auto       prepare  = [&](const Model& current, const std::string& action, const json& parameters, const std::string& volume = {}) {
        const auto context = AI::capture_model_tool_context(current, {}, colors, mixes);
        const auto op      = AI::parse_model_operation({{"context_id", context.id},
                                                        {"operation", action},
                                                        {"object_id", std::to_string(current.objects.front()->id().id)},
                                                        {"volume_id", volume},
                                                        {"parameters", parameters}});
        return AI::prepare_model_edit(current, mixes, colors, op, AI_ASSISTANT_RESOURCE_DIR, std::nullopt, &config, &features);
    };
    const auto overhang = of(features, "overhang").front();
    auto       edit     = prepare(model, "add_feature_modifier",
                                  {{"feature_id", overhang->id}, {"shape", "mesh"}, {"settings", {{"outer_wall_speed", 20}}}});
    const auto& volumes = edit.model.objects.front()->volumes;
    const auto  added   = std::find_if(volumes.begin(), volumes.end(), [](const ModelVolume* v) { return v->is_modifier(); });
    REQUIRE(added != volumes.end());
    CHECK((*added)->name == overhang->id);
    CHECK((*added)->config.get().option<ConfigOptionFloats>("outer_wall_speed")->values.front() == 20.);
    // In world coordinates the modifier wraps the feature, whatever the instance offset.
    const auto world = (*added)->mesh().transformed_bounding_box(edit.model.objects.front()->instances.front()->get_matrix() *
                                                                 (*added)->get_matrix());
    CHECK(world.contains(overhang->box.center()));
    CHECK(edit.result["settings"]["outer_wall_speed"][0] == 20.);

    const auto modifier_id = std::string(edit.result["volume_id"]);
    const auto changed     = prepare(edit.model, "set_modifier_settings", {{"settings", {{"outer_wall_speed", nullptr}, {"wall_loops", 5}}}},
                                     modifier_id);
    CHECK(changed.result["settings"] == json({{"wall_loops", 5}}));
    // A second modifier from the same report still works: modifiers change no geometry.
    CHECK_NOTHROW(prepare(edit.model, "add_feature_modifier", {{"feature_id", overhang->id}, {"shape", "box"}, {"settings", {{"wall_loops", 3}}}}));

    CHECK_THROWS(prepare(model, "add_feature_modifier", {{"feature_id", "overhang-99"}, {"shape", "box"}, {"settings", {{"wall_loops", 3}}}}));
    CHECK_THROWS(prepare(model, "add_feature_modifier", {{"feature_id", overhang->id}, {"shape", "box"}, {"settings", {{"layer_height", 0.1}}}}));
    CHECK_THROWS(prepare(model, "set_modifier_settings", {{"settings", {{"wall_loops", 3}}}}, std::to_string(object->volumes[0]->id().id)));
    // Moving the object makes the report stale.
    auto moved = prepare(model, "transform", {{"mode", "move"}, {"value", {5, 0, 0}}});
    CHECK_THROWS(prepare(moved.model, "add_feature_modifier", {{"feature_id", overhang->id}, {"shape", "box"}, {"settings", {{"wall_loops", 3}}}}));
}

TEST_CASE("Assistant feature detection agrees with the slicer", "[AIPrintAssistant][fast]")
{
    // Slice for real and check that the slicer prints bridge infill and overhang
    // walls where the detectors found bridges and overhangs, and neither on a cube.
    auto slice_roles = [](Model& model, const DynamicPrintConfig& config) {
        model.objects.front()->add_instance()->set_offset({90, 90, 0});
        model.objects.front()->invalidate_bounding_box();
        Print print;
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.process();
        // A loop reports its first path's role; overhanging stretches are paths inside it.
        std::vector<std::pair<double, ExtrusionRole>> roles;
        for (const auto* layer : print.objects().front()->layers())
            for (const auto* region : layer->regions())
                for (const auto* entities : {&region->fills, &region->perimeters})
                    for (const auto* entity : entities->flatten().entities) {
                        if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(entity))
                            for (const auto& path : loop->paths)
                                roles.emplace_back(layer->print_z, path.role());
                        else if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(entity))
                            for (const auto& path : multi->paths)
                                roles.emplace_back(layer->print_z, path.role());
                        else
                            roles.emplace_back(layer->print_z, entity->role());
                    }
        return roles;
    };
    auto has_role = [](const auto& roles, ExtrusionRole role, double lo, double hi) {
        return std::any_of(roles.begin(), roles.end(), [&](const auto& item) { return item.second == role && item.first > lo && item.first <= hi; });
    };
    const auto config = settings();
    {
        Model model;
        auto* object = model.add_object();
        object->add_volume(box(5, 10, 10), ModelVolumeType::MODEL_PART, false);
        object->add_volume(box(5, 10, 10, {15, 0, 0}), ModelVolumeType::MODEL_PART, false);
        object->add_volume(box(20, 10, 2, {0, 0, 10}), ModelVolumeType::MODEL_PART, false);
        const auto roles = slice_roles(model, config);
        const auto report  = AI::detect_model_features(*object, 0, config);
        const auto bridges = of(report, "bridge");
        REQUIRE(bridges.size() == 1);
        CHECK(has_role(roles, erBridgeInfill, bridges[0]->box.min.z(), bridges[0]->box.max.z() + 0.01));
    }
    {
        Model model;
        auto* object = model.add_object();
        auto  shape  = box(10, 10, 10, {10, 0, 0});
        MeshBoolean::cgal::plus(shape, box(30, 10, 2, {0, 0, 10}));
        object->add_volume(std::move(shape), ModelVolumeType::MODEL_PART, false);
        const auto roles     = slice_roles(model, config);
        const auto report    = AI::detect_model_features(*object, 0, config);
        const auto overhangs = of(report, "overhang");
        REQUIRE_FALSE(overhangs.empty());
        CHECK(has_role(roles, erOverhangPerimeter, overhangs[0]->box.min.z(), overhangs[0]->box.max.z() + 0.41));
    }
    {
        Model model;
        auto* object = model.add_object();
        object->add_volume(box(20, 20, 20), ModelVolumeType::MODEL_PART, false);
        const auto roles  = slice_roles(model, config);
        const auto report = AI::detect_model_features(*object, 0, config);
        CHECK(of(report, "bridge").empty());
        CHECK(of(report, "overhang").empty());
        CHECK_FALSE(has_role(roles, erBridgeInfill, 0., 100.));
        CHECK_FALSE(has_role(roles, erOverhangPerimeter, 0., 100.));
    }
}
