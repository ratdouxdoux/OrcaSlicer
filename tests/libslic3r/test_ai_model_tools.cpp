#include <catch2/catch_test_macros.hpp>
#include "libslic3r/AIModelTools.hpp"
#include "libslic3r/AIPrintAssistant.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include <set>

using namespace Slic3r;
namespace AI = Slic3r::AIPrintAssistant;
using json   = nlohmann::json;
namespace {
struct ModelFixture
{
    Model                    model;
    ModelObject*             cube = model.add_object();
    MixedFilamentManager     mixes;
    std::vector<std::string> colors{"#FF0000", "#0000FF", "#FFFF00"};
    ModelFixture()
    {
        cube->name = "Cube";
        cube->add_volume(make_cube(27, 27, 27), ModelVolumeType::MODEL_PART, false);
        cube->add_instance();
    }
    std::string        object_id() const { return std::to_string(cube->id().id); }
    std::string        volume_id() const { return std::to_string(cube->volumes[0]->id().id); }
    AI::ModelOperation operation(const std::string& action, const json& params, bool part = false) const
    {
        const auto context = AI::capture_model_tool_context(model, {object_id()}, colors, mixes);
        return AI::parse_model_operation({{"context_id", context.id},
                                          {"operation", action},
                                          {"object_id", object_id()},
                                          {"volume_id", part ? volume_id() : ""},
                                          {"parameters", params}});
    }
    AI::PreparedModelEdit prepare(const std::string& action, const json& params, bool part = false) const
    {
        return AI::prepare_model_edit(model, mixes, colors, operation(action, params, part), AI_ASSISTANT_RESOURCE_DIR);
    }
};
json text_parameters(const std::string& face = "top") { return {{"text", "hello"}, {"face", face}, {"height_mm", 5}, {"depth_mm", 0.6}}; }
} // namespace

TEST_CASE("Assistant live preview accumulates resize lettering settings and mixing", "[AIPrintAssistant][fast]")
{
    ModelFixture                      f;
    const auto                        defaults = DynamicPrintConfig::full_print_config();
    std::array<DynamicPrintConfig, 3> presets;
    presets[0].set_key_value("layer_height", new ConfigOptionFloat(0.2));
    auto state_id = [&] {
        std::string config;
        for (const auto& preset : presets)
            for (const auto& key : preset.keys())
                config += key + "=" + preset.opt_serialize(key) + "\n";
        return AI::capture_model_tool_context(f.model, {}, f.colors, f.mixes, config).id;
    };
    const auto            original_id        = state_id();
    const auto            original_object_id = f.cube->id();
    AI::PreviewCheckpoint checkpoint;
    checkpoint.state.model = f.model;
    checkpoint.state.mixes = f.mixes;
    checkpoint.presets     = presets;
    AI::LivePreview preview(std::move(checkpoint), original_id);
    auto            step = [&](const std::string& action, const json& parameters) {
        preview.verify(state_id());
        auto edit = f.prepare(action, parameters);
        if (edit.model_changed) {
            f.model.replace_objects(std::move(edit.model));
            f.cube = f.model.objects.front();
        }
        if (edit.mixes_changed)
            f.mixes = std::move(edit.mixes);
        preview.record(state_id());
    };
    step("transform", {{"mode", "size"}, {"value", {120, 27, 27}}});
    CHECK(std::abs(f.cube->bounding_box_exact().size().x() - 120.) < 1e-6);
    auto text    = text_parameters("front");
    text["text"] = "this was done with ai";
    step("add_text", text);
    REQUIRE(f.cube->volumes.size() == 2);
    CHECK(f.cube->volumes.back()->text_configuration->text == "this was done with ai");

    const auto   context = AI::capture_context(defaults, *f.cube);
    AI::Proposal proposal;
    proposal.context_id = context.id;
    proposal.object_id  = f.object_id();
    proposal.outcome    = "propose_changes";
    proposal.changes.set_key_value("wall_loops", new ConfigOptionInt(4));
    AI::Transaction transaction;
    f.cube->config.assign_config(AI::prepare_apply(context, proposal, transaction));
    preview.record(state_id());
    CHECK(f.cube->config.opt_int("wall_loops") == 4);
    CHECK_THROWS(AI::prepare_apply(AI::capture_context(defaults, *f.cube), proposal, transaction));

    step("create_mix", {{"components", {1, 2}}, {"percentages", {30, 70}}});
    presets[0].set_key_value("layer_height", new ConfigOptionFloat(0.16));
    preview.record(state_id());
    REQUIRE(preview.steps() == 5);
    CHECK(preview.pending());
    CHECK(f.mixes.mixed_filaments().size() == 1);
    SECTION("Apply retains the entire live state and allows explicit Revert")
    {
        const auto live = state_id();
        preview.accept(live);
        CHECK_FALSE(preview.pending());
        CHECK(state_id() == live);
        CHECK_THROWS(preview.record(live));
        CHECK_THROWS(preview.accept(live));
    }
    SECTION("Dismiss or Stop followed by Dismiss restores the original checkpoint") {}
    auto before = preview.before(state_id());
    // Restore from a copy: a failed GUI refresh must not consume the checkpoint.
    CHECK(preview.before(state_id()).state.model.objects.front()->volumes.size() == 1);
    f.model.replace_objects(std::move(before.state.model));
    f.cube  = f.model.objects.front();
    f.mixes = std::move(before.state.mixes);
    presets = std::move(before.presets);
    CHECK(state_id() == original_id);
    CHECK(f.cube->id() == original_object_id);
    CHECK(f.cube->volumes.size() == 1);
    CHECK(f.cube->config.empty());
    CHECK(f.mixes.mixed_filaments().empty());
}

TEST_CASE("Assistant live preview refuses to overwrite later manual edits", "[AIPrintAssistant][fast]")
{
    ModelFixture          f;
    auto                  id = [&] { return AI::capture_model_tool_context(f.model, {}, f.colors, f.mixes).id; };
    AI::PreviewCheckpoint before;
    before.state.model = f.model;
    AI::LivePreview preview(std::move(before), id());
    CHECK_THROWS(preview.accept(id()));
    f.cube->name = "Assistant name";
    preview.record(id());
    const auto preview_id = id();
    f.cube->name          = "Later user name";
    CHECK_THROWS(preview.verify(id()));
    CHECK_THROWS(preview.accept(id()));
    CHECK_THROWS(preview.before(id()));
    CHECK(f.cube->name == "Later user name");
    f.cube->name = "Assistant name";
    CHECK(id() == preview_id);
    CHECK_NOTHROW(preview.before(id()));
}

TEST_CASE("Assistant model tool schemas reject unknown or malformed operations", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    CHECK(AI::model_tool_catalog().size() >= 18);
    CHECK_THROWS(f.operation("run_shell", json::object()));
    CHECK_THROWS(f.operation("transform", {{"mode", "scale"}, {"value", {1, 2}}}));
    CHECK_THROWS(f.operation("add_text", {{"text", "hello"}, {"face", "top"}, {"height_mm", -1}, {"depth_mm", 1}}));
    auto text         = text_parameters();
    text["font_path"] = "private-file";
    CHECK_THROWS(f.operation("add_text", text));
    CHECK_THROWS(f.operation("create_mix", {{"components", {0, 2}}, {"percentages", {50, 50}}}));
    CHECK_THROWS(f.operation("create_mix", {{"components", {1, 2}}, {"percentages", {50, 50}}, {"distribution", "same_layer"}}));
}

TEST_CASE("Assistant model context fingerprints geometry painting config and selection", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    f.cube->input_file = "private-file-sentinel";
    f.cube->config.set_key_value("printhost_apikey", new ConfigOptionString("private-key-sentinel"));
    const auto capture = [&] { return AI::capture_model_tool_context(f.model, {f.object_id()}, f.colors, f.mixes); };
    const auto before  = capture();
    CHECK(capture().id == before.id);
    CHECK(before.data.dump().find("sentinel") == std::string::npos);
    CHECK(before.data["objects"][0]["size_mm"] == json({27., 27., 27.}));
    CHECK(before.data["objects"][0]["volumes"][0]["id"] == f.volume_id());
    SECTION("Geometry with unchanged IDs")
    {
        auto mesh = f.cube->volumes[0]->mesh();
        mesh.its.vertices[0].x() += 0.1f;
        f.cube->volumes[0]->set_mesh(std::move(mesh));
    }
    SECTION("Painting")
    {
        TriangleSelector selector(f.cube->volumes[0]->mesh());
        selector.set_facet(0, EnforcerBlockerType::Extruder2);
        f.cube->volumes[0]->mmu_segmentation_facets.set(selector);
    }
    SECTION("Object setting") { f.cube->config.set_key_value("wall_loops", new ConfigOptionInt(4)); }
    SECTION("Filament palette") { f.colors[0] = "#00FF00"; }
    SECTION("Brim ears") { f.cube->brim_points.emplace_back(1.f, 2.f, 0.f, 3.f); }
    SECTION("Height range") { f.cube->layer_config_ranges[{1., 3.}].set_key_value("wall_loops", new ConfigOptionInt(5)); }
    CHECK(capture().id != before.id);
    CHECK(AI::capture_model_tool_context(f.model, {}, f.colors, f.mixes).id != before.id);
}

TEST_CASE("Assistant transforms preserve the source and use world-space centres", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    const auto   original = f.cube->bounding_box_exact();
    auto         moved    = f.prepare("transform", {{"mode", "move"}, {"value", {10, 20, 30}}});
    CHECK((moved.model.objects[0]->bounding_box_exact().center() - original.center() - Vec3d(10, 20, 30)).norm() < 1e-6);
    auto sized = f.prepare("transform", {{"mode", "size"}, {"value", {10, 20, 30}}});
    CHECK((sized.model.objects[0]->bounding_box_exact().size() - Vec3d(10, 20, 30)).norm() < 1e-6);
    CHECK((sized.model.objects[0]->bounding_box_exact().center() - original.center()).norm() < 1e-6);
    CHECK((f.cube->bounding_box_exact().size() - Vec3d(27, 27, 27)).norm() < 1e-6);
    CHECK_THROWS(f.prepare("transform", {{"mode", "scale"}, {"value", {0, 1, 1}}}));
    f.cube->add_instance();
    CHECK_THROWS(f.prepare("transform", {{"mode", "move"}, {"value", {1, 0, 0}}}));
}

TEST_CASE("Assistant creates editable hello text centred on each cube face", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    for (const auto* face : {"top", "bottom", "front", "back", "left", "right"}) {
        INFO(face);
        auto edit = f.prepare("add_text", text_parameters(face));
        REQUIRE(edit.model.objects[0]->volumes.size() == 2);
        const auto* text = edit.model.objects[0]->volumes.back();
        REQUIRE(text->text_configuration.has_value());
        CHECK(text->text_configuration->text == "hello");
        REQUIRE(text->emboss_shape.has_value());
        CHECK_FALSE(text->mesh().empty());
        const auto bounds   = text->mesh().transformed_bounding_box(text->get_matrix());
        const int  axis     = std::string(face) == "top" || std::string(face) == "bottom" ? 2 :
                              std::string(face) == "left" || std::string(face) == "right" ? 0 :
                                                                                            1;
        const bool positive = std::string(face) == "top" || std::string(face) == "right" || std::string(face) == "back";
        for (int i = 0; i < 3; ++i)
            if (i != axis)
                CHECK(std::abs(bounds.center()[i] - 13.5) < 0.01);
        CHECK(std::abs((positive ? bounds.min[axis] : bounds.max[axis]) - (positive ? 26.95 : 0.05)) < 0.01);
        CHECK(f.cube->volumes.size() == 1);
    }
}

TEST_CASE("Assistant text validates footprint and engraving keeps negative geometry", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    auto         parameters = text_parameters();
    parameters["mode"]      = "engraved";
    auto        edit        = f.prepare("add_text", parameters);
    const auto* text        = edit.model.objects[0]->volumes.back();
    CHECK(text->type() == ModelVolumeType::NEGATIVE_VOLUME);
    const auto bounds = text->mesh().transformed_bounding_box(text->get_matrix());
    CHECK(bounds.min.z() < 27.);
    CHECK(bounds.max.z() > 27.);
    parameters["height_mm"] = 200;
    CHECK_THROWS(f.prepare("add_text", parameters));
    parameters              = text_parameters();
    parameters["offset_mm"] = {200, 0};
    CHECK_THROWS(f.prepare("add_text", parameters));
    parameters         = text_parameters();
    parameters["text"] = "";
    CHECK_THROWS(f.prepare("add_text", parameters));
}

TEST_CASE("Assistant new shapes default to the active plate centre and bed surface", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    Vec3d        plate_center(128., 128., 0.);
    SECTION("First plate") {}
    SECTION("Offset plate") { plate_center = Vec3d(448., -192., 5.); }
    for (const auto* shape : {"cube", "cylinder", "sphere", "cone", "torus", "wedge"}) {
        INFO(shape);
        auto operation = f.operation("add_primitive", {{"shape", shape}, {"size_mm", {40., 30., 20.}}});
        operation.object_id.clear();
        const auto edit = AI::prepare_model_edit(f.model, f.mixes, f.colors, operation, AI_ASSISTANT_RESOURCE_DIR, plate_center);
        REQUIRE(edit.model.objects.size() == 2);
        const auto bounds = edit.model.objects.back()->bounding_box_exact();
        CHECK(std::abs(bounds.center().x() - plate_center.x()) < 0.001);
        CHECK(std::abs(bounds.center().y() - plate_center.y()) < 0.001);
        CHECK(std::abs(bounds.min.z() - plate_center.z()) < 0.001);
        CHECK((bounds.size() - Vec3d(40., 30., 20.)).norm() < 0.001);
        CHECK(f.model.objects.size() == 1);
    }
}

TEST_CASE("Assistant shape placement preserves explicit positions and parent-part defaults", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    auto         operation = f.operation("add_primitive", {{"shape", "cube"}, {"size_mm", {10., 10., 10.}}});
    SECTION("Existing part stays centred on its parent")
    {
        const auto  edit = AI::prepare_model_edit(f.model, f.mixes, f.colors, operation, AI_ASSISTANT_RESOURCE_DIR, Vec3d(448., -192., 0.));
        const auto* volume = edit.model.objects.front()->volumes.back();
        const auto  bounds = volume->mesh().transformed_bounding_box(volume->get_matrix());
        CHECK((bounds.center() - f.cube->bounding_box_exact().center()).norm() < 0.001);
    }
    SECTION("New object requires a plate instead of silently using the origin")
    {
        operation.object_id.clear();
        CHECK_THROWS(AI::prepare_model_edit(f.model, f.mixes, f.colors, operation, AI_ASSISTANT_RESOURCE_DIR));
    }
    SECTION("Explicit world position overrides plate centring, including zero XY")
    {
        operation.object_id.clear();
        operation.parameters["position_mm"] = {0., 0., 15.};
        for (const auto& plate : {std::optional<Vec3d>{}, std::optional<Vec3d>{Vec3d(448., -192., 0.)}}) {
            const auto edit = AI::prepare_model_edit(f.model, f.mixes, f.colors, operation, AI_ASSISTANT_RESOURCE_DIR, plate);
            CHECK((edit.model.objects.back()->bounding_box_exact().center() - Vec3d(0., 0., 15.)).norm() < 0.001);
        }
    }
}

TEST_CASE("Assistant primitives duplicates and deletes remain isolated until commit", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    auto         primitive = f.prepare("add_primitive",
                                       {{"shape", "cylinder"}, {"size_mm", {5, 5, 10}}, {"position_mm", {13.5, 13.5, 25}}, {"type", "negative"}});
    REQUIRE(primitive.model.objects[0]->volumes.size() == 2);
    CHECK(primitive.model.objects[0]->volumes.back()->type() == ModelVolumeType::NEGATIVE_VOLUME);
    CHECK(f.cube->volumes.size() == 1);
    auto copies = f.prepare("duplicate", {{"count", 2}, {"offset_mm", {30, 0, 0}}});
    REQUIRE(copies.model.objects.size() == 3);
    std::set<size_t> identities;
    for (const auto* object : copies.model.objects)
        identities.insert(object->id().id);
    CHECK(identities.size() == 3);
    CHECK_THROWS(f.prepare("delete", json::object(), true));
    auto removed = f.prepare("delete", json::object());
    CHECK(removed.model.objects.empty());
    CHECK(f.model.objects.size() == 1);
}

TEST_CASE("Assistant surface painting changes only matching facets", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    auto         edit = f.prepare("paint_surface", {{"channel", "color"}, {"state", "paint"}, {"face", "top"}, {"filament_id", 2}}, true);
    const auto*  part = edit.model.objects[0]->volumes[0];
    TriangleSelector selector(part->mesh());
    selector.deserialize(part->mmu_segmentation_facets.get_data());
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder2) == 2);
    CHECK(selector.num_facets(EnforcerBlockerType::NONE) == 10);
    CHECK(f.cube->volumes[0]->mmu_segmentation_facets.empty());
    CHECK_THROWS(f.prepare("paint_surface", {{"channel", "color"}, {"state", "paint"}, {"face", "top"}, {"filament_id", 99}}, true));
    CHECK_THROWS(f.prepare("paint_surface",
                           {{"channel", "support"},
                            {"state", "enforce"},
                            {"face", "top"},
                            {"box_min_mm", {100, 100, 100}},
                            {"box_max_mm", {200, 200, 200}}},
                           true));
}

TEST_CASE("Assistant color mixes create real recipes with stable IDs and assignment", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    auto         edit = f.prepare("create_mix", {{"components", {1, 2}}, {"percentages", {70, 30}}});
    REQUIRE(edit.mixes.mixed_filaments().size() == 1);
    const auto& recipe = edit.mixes.mixed_filaments()[0];
    CHECK(recipe.stable_id != 0);
    CHECK(recipe.mix_b_percent == 30);
    CHECK(recipe.distribution_mode == MixedFilament::Simple);
    CHECK(recipe.gradient_component_ids.empty());
    CHECK(edit.result["filament_id"] == 4);
    CHECK(edit.model.objects[0]->config.get().opt_int("extruder") == 4);
    CHECK(f.mixes.mixed_filaments().empty());
    CHECK(f.cube->config.empty());
    auto reloaded = MixedFilamentManager();
    reloaded.load_custom_entries(edit.mixes.serialize_custom_entries(), f.colors);
    REQUIRE_FALSE(reloaded.mixed_filaments().empty());
    CHECK_THROWS(f.prepare("create_mix", {{"components", {1, 2}}, {"percentages", {40, 40}}}));
    CHECK_THROWS(f.prepare("create_mix", {{"components", {1, 1}}, {"percentages", {50, 50}}}));
    CHECK_THROWS(f.prepare("create_mix", {{"components", {1, 9}}, {"percentages", {50, 50}}}));
    CHECK_THROWS(f.prepare("create_mix", {{"components", {1, 2}},
                                          {"percentages", {50, 50}},
                                          {"gradient", true},
                                          {"gradient_start_percent", 50},
                                          {"gradient_end_percent", 50}}));
}

TEST_CASE("Assistant cuts and booleans produce geometry without touching the source", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    auto         cut = f.prepare("cut", {{"position_mm", {13.5, 13.5, 13.5}}, {"normal", {0, 0, 1}}, {"keep", "both"}});
    REQUIRE(cut.model.objects.size() == 2);
    CHECK(f.model.objects.size() == 1);
    double sum = 0.;
    for (const auto* object : cut.model.objects)
        for (const auto* volume : object->volumes)
            sum += TriangleMesh(volume->mesh()).volume();
    CHECK(std::abs(sum - 27. * 27. * 27.) < 0.1);
    auto* other = f.cube->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
    other->set_offset(Vec3d(8, 8, 20));
    auto difference = f.prepare("boolean", {{"mode", "difference"}, {"other_volume_id", std::to_string(other->id().id)}}, true);
    CHECK(difference.model.objects[0]->volumes.size() == 1);
    const double remaining = TriangleMesh(difference.model.objects[0]->volumes[0]->mesh()).volume();
    CHECK(remaining < 27. * 27. * 27.);
    CHECK(f.cube->volumes.size() == 2);
}

TEST_CASE("Assistant settings remain available after embossed or engraved text", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    auto         parameters = text_parameters();
    SECTION("Raised") {}
    SECTION("Engraved") { parameters["mode"] = "engraved"; }
    auto       edit     = f.prepare("add_text", parameters);
    const auto defaults = DynamicPrintConfig::full_print_config();
    const auto context  = AI::capture_context(defaults, *edit.model.objects[0]);
    CHECK(context.public_context["parts"].size() == 2);
    auto* text = edit.model.objects[0]->volumes.back();
    text->set_offset(text->get_offset() + Vec3d(1, 0, 0));
    CHECK(AI::capture_context(defaults, *edit.model.objects[0]).id != context.id);
}

TEST_CASE("Assistant mixes preserve weighted recipes and active virtual IDs", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    f.mixes.add_custom_filament(1, 2, 50, f.colors);
    f.mixes.mixed_filaments().front().deleted = true;
    auto edit                                 = f.prepare("create_mix", {{"components", {1, 2, 3}}, {"percentages", {20, 30, 50}}});
    REQUIRE(edit.result["filament_id"] == 4);
    const auto definition = edit.mixes.mixed_filament_definition_from_id(4, f.colors.size());
    REQUIRE(definition.has_value());
    REQUIRE(definition->recipe.blend.components.size() == 3);
    CHECK(definition->recipe.blend.components[0].percent == 20);
    CHECK(definition->recipe.blend.components[1].percent == 30);
    CHECK(definition->recipe.blend.components[2].percent == 50);
    const auto context = AI::capture_model_tool_context(edit.model, {}, f.colors, edit.mixes);
    CHECK(context.data["filaments"].back()["id"] == 4);
    auto gradient = f.prepare("create_mix", {{"components", {1, 2}},
                                             {"percentages", {50, 50}},
                                             {"gradient", true},
                                             {"gradient_start_percent", 90},
                                             {"gradient_end_percent", 10}});
    auto reloaded = MixedFilamentManager();
    reloaded.load_custom_entries(gradient.mixes.serialize_custom_entries(), f.colors);
    const auto* row = reloaded.mixed_filament_from_id(4, f.colors.size());
    REQUIRE(row != nullptr);
    CHECK(row->gradient_enabled);
    CHECK(std::abs(row->gradient_start - 0.9) < 0.001);
    CHECK(std::abs(row->gradient_end - 0.1) < 0.001);
    CHECK_THROWS(f.prepare("create_mix", {{"components", {1, 2}}, {"percentages", {50, 50}}, {"pattern", "123"}}));
    CHECK_THROWS(f.prepare("create_mix", {{"components", {1, 2}}, {"percentages", {50, 50}}, {"pattern", "129"}}));
    CHECK_THROWS(f.prepare("create_mix", {{"components", {1, 2}}, {"percentages", {70, 30}}, {"pattern", "12"}}));
    CHECK_THROWS(f.prepare("create_mix", {{"components", {1, 2, 3}}, {"percentages", {20, 30, 50}}, {"distribution", "simple"}}));
    auto       patterned          = f.prepare("create_mix", {{"components", {1, 2}}, {"percentages", {70, 30}}, {"pattern", "1111111222"}});
    const auto pattern_definition = patterned.mixes.mixed_filament_definition_from_id(4, f.colors.size());
    REQUIRE(pattern_definition.has_value());
    CHECK(mixed_filament_manual_pattern_sequence(*pattern_definition, f.colors.size()) ==
          std::vector<unsigned int>{1, 1, 1, 1, 1, 1, 1, 2, 2, 2});
}

TEST_CASE("Assistant edits existing text and protects the final solid part", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    auto         first         = f.prepare("add_text", text_parameters());
    auto         request       = f.operation("edit_text", text_parameters());
    request.volume_id          = std::to_string(first.model.objects[0]->volumes.back()->id().id);
    request.parameters["text"] = "world";
    auto second                = AI::prepare_model_edit(first.model, f.mixes, f.colors, request, AI_ASSISTANT_RESOURCE_DIR);
    REQUIRE(second.model.objects[0]->volumes.size() == 2);
    CHECK(second.model.objects[0]->volumes.back()->text_configuration->text == "world");
    CHECK(first.model.objects[0]->volumes.back()->text_configuration->text == "hello");
    f.cube->add_volume(make_cube(5, 5, 5), ModelVolumeType::NEGATIVE_VOLUME, false);
    CHECK_THROWS(f.prepare("delete", json::object(), true));
    CHECK_THROWS(f.prepare("set_part_type", {{"type", "negative"}}, true));
}

TEST_CASE("Assistant split and simplify use native geometry operations", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    auto         mesh     = f.cube->volumes[0]->mesh();
    auto         detached = make_cube(2, 2, 2);
    detached.translate(40.f, 0.f, 0.f);
    mesh.merge(detached);
    f.cube->volumes[0]->set_mesh(std::move(mesh));
    auto split = f.prepare("split_parts", json::object(), true);
    REQUIRE(split.model.objects[0]->volumes.size() == 2);
    CHECK(f.cube->volumes.size() == 1);
    f.cube->volumes[0]->set_mesh(make_sphere(10.));
    const auto before     = f.cube->volumes[0]->mesh().facets_count();
    auto       simplified = f.prepare("simplify", {{"reduction_percent", 50}}, true);
    CHECK(simplified.model.objects[0]->volumes[0]->mesh().facets_count() < before);
    CHECK(f.cube->volumes[0]->mesh().facets_count() == before);
}

TEST_CASE("Assistant geometry commit preserves project metadata and recovery identities", "[AIPrintAssistant][fast]")
{
    ModelFixture f;
    f.model.set_object_backup_id(*f.cube, 42);
    const auto original_id = f.model.id();
    auto       edit        = f.prepare("rename", {{"name", "Renamed cube"}});
    // Project-level changes made after staging must never be overwritten by geometry.
    f.model.curr_plate_index = 3;
    f.model.plates_custom_gcodes[3].gcodes.push_back({5., CustomGCode::PausePrint, 1, "", "Pause retained"});
    f.model.mk_name = "Project metadata retained";
    f.model.replace_objects(std::move(edit.model));
    CHECK(edit.model.objects.empty());
    REQUIRE(f.model.objects.size() == 1);
    const auto* object = f.model.objects.front();
    CHECK(object->name == "Renamed cube");
    CHECK(object->get_model() == &f.model);
    CHECK(object->volumes.front()->get_object() == object);
    CHECK(f.model.id() == original_id);
    CHECK(f.model.get_object_backup_id(*object) == 42);
    CHECK(f.model.curr_plate_index == 3);
    REQUIRE(f.model.plates_custom_gcodes[3].gcodes.size() == 1);
    CHECK(f.model.plates_custom_gcodes[3].gcodes.front().extra == "Pause retained");
    CHECK(f.model.mk_name == "Project metadata retained");
}
