#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/AIGeneratedModel.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/Utils/AIOpenSCAD.hpp"
#include "slic3r/Utils/AIPrintAssistantChat.hpp"
#include <chrono>
#include <thread>
#include <boost/filesystem/operations.hpp>

using namespace Slic3r;
namespace AI = Slic3r::AIPrintAssistant;
using json   = nlohmann::json;
using Catch::Matchers::WithinAbs;

namespace {
json arguments(const std::string& code, const std::string& object_id = {})
{
    return {{"name", "Bracket"}, {"code", code}, {"object_id", object_id}};
}

#ifdef OPENSCAD_TEST_EXECUTABLE
AI::OpenSCADRun::Result run_openscad(const std::string& code)
{
    AI::OpenSCADRun run(code, OPENSCAD_TEST_EXECUTABLE);
    const auto      deadline = std::chrono::steady_clock::now() + std::chrono::seconds(150);
    while (!run.done() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return run.take();
}

size_t openscad_folders()
{
    const auto                folder = boost::filesystem::temp_directory_path() / "snapmaker-orca-openscad";
    boost::system::error_code error;
    size_t                    count = 0;
    for (boost::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
        ++count;
    return count;
}
#endif
} // namespace

TEST_CASE("Assistant runs OpenSCAD and returns the mesh, a view and clean errors", "[AIPrintAssistant]")
{
#ifndef OPENSCAD_TEST_EXECUTABLE
    SKIP("The bundled OpenSCAD is not in the build folder.");
#else
    const auto folders = openscad_folders();
    SECTION("Builds a mesh")
    {
        const auto result = run_openscad("difference() {\n  cube([20, 10, 5]);\n  translate([10, 5, -1]) cylinder(d = 4, h = 10, $fn = 32);\n}\n"
                                         "echo(\"done\");\n");
        INFO(result.error << "\n" << result.messages);
        REQUIRE(result.ok);
        REQUIRE_FALSE(result.mesh.empty());
        const auto size = result.mesh.bounding_box().size();
        CHECK_THAT(size.x(), WithinAbs(20., 1e-3));
        CHECK_THAT(size.y(), WithinAbs(10., 1e-3));
        CHECK_THAT(size.z(), WithinAbs(5., 1e-3));
        CHECK(result.messages.find("ECHO: \"done\"") != std::string::npos);
        CHECK(result.messages.find("Fontconfig") == std::string::npos);
        CHECK(result.summary["geometry"]["dimensions"] == 3);
        // The rendered view needs OpenGL; when it exists it is a PNG.
        if (!result.view_png_base64.empty())
            CHECK(result.view_png_base64.rfind("iVBORw0KGgo", 0) == 0);
        CHECK(result.seconds > 0.);
    }
    SECTION("Reports errors without the temporary folder")
    {
        const auto result = run_openscad("cube(10");
        CHECK_FALSE(result.ok);
        CHECK(result.mesh.empty());
        CHECK(result.error.find("openscad_output") != std::string::npos);
        CHECK(result.messages.find("line ") != std::string::npos);
        CHECK(result.messages.find("snapmaker-orca-openscad") == std::string::npos);
    }
    SECTION("Rejects 2D results")
    {
        const auto result = run_openscad("square(10);");
        CHECK_FALSE(result.ok);
        CHECK(result.messages.find("not a 3D object") != std::string::npos);
    }
    SECTION("Stops when cancelled")
    {
        // 100 million empty iterations keep OpenSCAD busy for about 20 seconds without using memory.
        AI::OpenSCADRun run("x = [for (i = [0:999]) for (j = [0:999]) for (k = [0:99]) if (i < 0) 1];\necho(len(x));\ncube(1);",
                            OPENSCAD_TEST_EXECUTABLE);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        CHECK_FALSE(run.done());
        const auto start = std::chrono::steady_clock::now();
        run.cancel();
        const auto result = run.take();
        CHECK(result.error == "Stopped by the user.");
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
    }
    CHECK(openscad_folders() == folders);
#endif
}

TEST_CASE("Assistant OpenSCAD code cannot read files", "[AIPrintAssistant][fast]")
{
    const std::vector<std::string> reading = {"import(\"secret.stl\");",
                                              "include <lib.scad>\ncube(1);",
                                              "use<lib.scad>\ncube(1);",
                                              "surface(file = \"heights.dat\");",
                                              "linear_extrude(file = \"a.dxf\", height = 2);",
                                              "data = import(\"data.json\");",
                                              "import_stl(\"a.stl\");",
                                              "dxf_linear_extrude(\"a.dxf\");",
                                              "x = dxf_dim(\"a.dxf\", \"d\");"};
    for (size_t i = 0; i < reading.size(); ++i) {
        DYNAMIC_SECTION("Rejects " << reading[i]) { CHECK_THROWS(AI::check_openscad_code(reading[i])); }
    }
    // Comments and strings may mention them, and other identifiers may contain them.
    CHECK_NOTHROW(AI::check_openscad_code("// import(\"x.stl\")\n/* include <y> */\ntext(\"use import here\");\ncube(10);"));
    CHECK_NOTHROW(AI::check_openscad_code("s = \"quote \\\" import(\";\ncube(s == \"\" ? 1 : 2);"));
    CHECK_NOTHROW(AI::check_openscad_code("important = 3; profile_file = 2; user = 1; cube(important + profile_file + user);"));
    CHECK_THROWS(AI::check_openscad_code(""));
    CHECK_THROWS(AI::check_openscad_code(std::string(AI::MAX_OPENSCAD_CODE_BYTES + 1, ' ')));
    CHECK_THROWS(AI::check_openscad_code(std::string("cube(1);\0", 9)));
    // A string or comment that never ends cannot hide code from the check.
    CHECK_NOTHROW(AI::check_openscad_code("cube(1); /* unterminated"));
    CHECK_THROWS(AI::check_openscad_code("cube(1); import(\"a.stl\"); \"unterminated"));
}

TEST_CASE("Assistant generate_model arguments are validated", "[AIPrintAssistant][fast]")
{
    const auto request = AI::parse_generate_model({{"name", "  Phone\tstand "}, {"code", "cube(10);"}, {"object_id", ""}});
    CHECK(request.name == "Phone stand");
    CHECK(request.code == "cube(10);");
    CHECK(request.object_id.empty());
    CHECK(AI::parse_generate_model(arguments("cube(1);", "42")).object_id == "42");
    std::string accented;
    for (int i = 0; i < 64; ++i)
        accented += "\xC3\xA9";
    CHECK(AI::parse_generate_model({{"name", accented}, {"code", "cube(1);"}, {"object_id", ""}}).name == accented);
    CHECK_THROWS(AI::parse_generate_model({{"name", accented + "e"}, {"code", "cube(1);"}, {"object_id", ""}}));
    CHECK_THROWS(AI::parse_generate_model({{"name", " \t "}, {"code", "cube(1);"}, {"object_id", ""}}));
    CHECK_THROWS(AI::parse_generate_model(arguments("cube(1);", "4a")));
    CHECK_THROWS(AI::parse_generate_model({{"name", "Bracket"}, {"code", "cube(1);"}}));
    CHECK_THROWS(AI::parse_generate_model({{"name", "Bracket"}, {"code", 5}, {"object_id", ""}}));
    CHECK_THROWS(AI::parse_generate_model({{"name", "Bracket"}, {"code", "cube(1);"}, {"object_id", ""}, {"path", "x"}}));
    CHECK_THROWS(AI::parse_generate_model(arguments("import(\"a.stl\");")));
    CHECK_THROWS(AI::parse_generate_model(json::array()));
}

TEST_CASE("Assistant OpenSCAD messages hide the temporary folder", "[AIPrintAssistant][fast]")
{
    const std::string folder = "C:\\Users\\me\\AppData\\Local\\Temp\\snapmaker-orca-openscad\\ab12";
    const std::string log    = "Fontconfig error: Cannot load default config file: File not found\r\n"
                               "ERROR: Parser error: syntax error in file model.scad, line 3\r\n"
                               "Can't parse file 'C:/Users/me/AppData/Local/Temp/snapmaker-orca-openscad/ab12/model.scad'!\r\n"
                               "\r\n"
                               "WARNING: Ignoring unknown module 'foo' in file C:\\Users\\me\\AppData\\Local\\Temp\\snapmaker-orca-openscad\\ab12\\"
                               "model.scad, line 1\r\n"
                               "ECHO: \"w=\", 12\r\n";
    CHECK(AI::openscad_messages(log, folder) == "ERROR: Parser error: syntax error in file model.scad, line 3\n"
                                                "Can't parse file 'model.scad'!\n"
                                                "WARNING: Ignoring unknown module 'foo' in file model.scad, line 1\n"
                                                "ECHO: \"w=\", 12\n");
    std::string trace;
    for (int i = 0; i < 200; ++i)
        trace += "TRACE: called by 'r' in file model.scad, line 1\n";
    const auto capped = AI::openscad_messages(trace, folder);
    CHECK(capped.find("... 140 more lines") != std::string::npos);
    CHECK(capped.size() < 6100);
    CHECK(AI::openscad_messages("Fontconfig error: x\n\n", folder).empty());
}

TEST_CASE("Assistant generated models land on the plate and rebuild in place", "[AIPrintAssistant][fast]")
{
    Model                    model;
    const Vec3d              plate(120., 110., 0.);
    AI::GenerateModelRequest request{"Block", "cube([20, 10, 5]);", ""};
    // OpenSCAD's own origin does not matter.
    TriangleMesh mesh = make_cube(20, 10, 5);
    mesh.translate(100.f, -40.f, 7.f);
    auto edit = AI::prepare_generated_model(model, mesh, request, "", plate);
    CHECK(model.objects.empty());
    CHECK(edit.model_changed);
    REQUIRE(edit.model.objects.size() == 1);
    auto* object = edit.model.objects.front();
    REQUIRE(object->volumes.size() == 1);
    CHECK(object->name == "Block");
    CHECK(object->volumes.front()->name == "Block");
    CHECK(edit.result["object_id"] == std::to_string(object->id().id));
    CHECK(edit.result["volume_id"] == std::to_string(object->volumes.front()->id().id));
    auto box = object->instance_bounding_box(0);
    CHECK_THAT(box.center().x(), WithinAbs(120., 1e-4));
    CHECK_THAT(box.center().y(), WithinAbs(110., 1e-4));
    CHECK_THAT(box.min.z(), WithinAbs(0., 1e-4));
    CHECK_THAT(box.size().x(), WithinAbs(20., 1e-4));
    CHECK_THAT(box.size().z(), WithinAbs(5., 1e-4));
    CHECK_THROWS(AI::prepare_generated_model(model, mesh, request, "", std::nullopt));
    CHECK_THROWS(AI::prepare_generated_model(model, TriangleMesh(), request, "", plate));
    CHECK_THROWS(AI::prepare_generated_model(model, make_cube(2500, 10, 10), request, "", plate));

    // Rebuilding keeps placement, filament and the object's other parts.
    model           = edit.model;
    auto* generated = model.objects.front();
    auto* part      = generated->volumes.front();
    generated->instances.front()->set_offset(Vec3d(50., 60., 0.));
    generated->instances.front()->set_rotation(Vec3d(0., 0., PI / 2.));
    part->config.set("extruder", 2);
    TriangleSelector selector(part->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder3);
    part->mmu_segmentation_facets.set(selector);
    REQUIRE(part->is_mm_painted());
    generated->add_volume(make_cube(2, 2, 2), ModelVolumeType::MODEL_PART, false)->name = "Label";
    const auto old_id = part->id();
    request.object_id = std::to_string(generated->id().id);
    auto rebuilt      = AI::prepare_generated_model(model, make_cube(30, 30, 30), request, std::to_string(old_id.id), std::nullopt);
    REQUIRE(rebuilt.model.objects.size() == 1);
    const auto* object2 = rebuilt.model.objects.front();
    REQUIRE(object2->volumes.size() == 2);
    const ModelVolume* replaced = nullptr;
    for (const auto* volume : object2->volumes)
        if (volume->name == "Block")
            replaced = volume;
    REQUIRE(replaced);
    CHECK(replaced->id() != old_id);
    CHECK(rebuilt.result["volume_id"] == std::to_string(replaced->id().id));
    CHECK(replaced->extruder_id() == 2);
    CHECK_FALSE(replaced->is_mm_painted());
    CHECK_THAT(replaced->mesh().bounding_box().size().x(), WithinAbs(30., 1e-4));
    CHECK_THAT(object2->instances.front()->get_offset().x(), WithinAbs(50., 1e-4));
    CHECK_THAT(object2->instances.front()->get_rotation().z(), WithinAbs(PI / 2., 1e-6));
    CHECK_THAT(object2->instance_bounding_box(0).min.z(), WithinAbs(0., 1e-4));
    CHECK(part->is_mm_painted());
    CHECK_THROWS(AI::prepare_generated_model(model, make_cube(3, 3, 3), request, "123456789", std::nullopt));
    request.object_id = "123456789";
    CHECK_THROWS(AI::prepare_generated_model(model, make_cube(3, 3, 3), request, std::to_string(old_id.id), std::nullopt));
}

TEST_CASE("Assistant remembers generated code only for the matching mesh", "[AIPrintAssistant][fast]")
{
    Model model;
    auto* object = model.add_object();
    auto* volume = object->add_volume(make_cube(10, 10, 10));
    object->add_instance();
    AI::GeneratedModels sources;
    CHECK(sources.find(*volume) == nullptr);
    sources.record(*volume, "Cube", "cube(10);");
    REQUIRE(sources.find(*volume) != nullptr);
    CHECK(sources.find(*volume)->name == "Cube");
    CHECK(sources.find(*volume)->code == "cube(10);");
    // Copies of the model, as in undo and preview checkpoints, keep IDs and meshes.
    const Model copy = model;
    REQUIRE(sources.find(*copy.objects.front()->volumes.front()) != nullptr);
    // After a mesh edit the code no longer describes the part.
    auto scaled = volume->mesh();
    scaled.scale(2.f);
    volume->set_mesh(std::move(scaled));
    CHECK(sources.find(*volume) == nullptr);
    // Each version is found for its own mesh.
    sources.record(*volume, "Cube", "scale(2) cube(10);");
    CHECK(sources.find(*volume)->code == "scale(2) cube(10);");
    CHECK(sources.find(*copy.objects.front()->volumes.front())->code == "cube(10);");
    // Another part with the same mesh has no code.
    auto* other = object->add_volume(make_cube(10, 10, 10));
    CHECK(sources.find(*other) == nullptr);
}

TEST_CASE("Assistant generate_model tools and rendered views reach the provider", "[AIPrintAssistant][fast]")
{
    for (const auto provider : AI::PROVIDERS) {
        DYNAMIC_SECTION("Provider " << AI::provider_id(provider))
        {
            const auto request = json::parse(AI::make_chat_request(provider, AI::provider_info(provider).default_model,
                                                                   json::array({AI::chat_user_message(provider, "Hello")})));
            json generate, source;
            for (const auto& tool : request["tools"]) {
                const auto& definition = AI::uses_chat_completions(provider) ? tool["function"] : tool;
                const auto  schema     = AI::uses_chat_completions(provider) ? definition["parameters"] : definition["input_schema"];
                if (definition["name"] == "generate_model")
                    generate = schema;
                if (definition["name"] == "get_model_source")
                    source = schema;
            }
            CHECK(generate == AI::generate_model_schema());
            CHECK(generate["required"] == json({"name", "code", "object_id"}));
            CHECK(source["required"] == json({"object_id"}));
            const auto system = AI::uses_chat_completions(provider) ? request["messages"][0]["content"].get<std::string>() :
                                                                      request["system"].get<std::string>();
            CHECK(system.find("call generate_model") != std::string::npos);
            CHECK(system.find("get_model_source") != std::string::npos);

            AI::ChatToolCall call;
            call.id                                 = "call_1";
            call.name                               = "generate_model";
            const std::vector<AI::ImageInput> views = {{"image/png", "YWJj", 512, 512}};
            const auto message = AI::chat_tool_result(provider, call, {{"status", "imported"}}, false, views);
            const auto plain   = AI::chat_tool_result(provider, call, {{"status", "imported"}}, false);
            if (AI::uses_chat_completions(provider)) {
                // Chat-completions tool messages carry text only.
                CHECK(message == plain);
                CHECK(message.dump().find("YWJj") == std::string::npos);
            } else {
                const auto& content = message["content"][0]["content"];
                REQUIRE(content.is_array());
                REQUIRE(content.size() == 2);
                CHECK(json::parse(content[0]["text"].get<std::string>())["status"] == "imported");
                CHECK(content[1]["type"] == "image");
                CHECK(content[1]["source"]["data"] == "YWJj");
                CHECK(plain["content"][0]["content"].is_string());
                CHECK_THROWS(AI::chat_tool_result(provider, call, {}, false, {{"image/gif", "YWJj", 1, 1}}));
            }
        }
    }
}
