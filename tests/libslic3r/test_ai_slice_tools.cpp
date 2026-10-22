#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "libslic3r/AISliceTools.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/Utils/AIPrintAssistantChat.hpp"

#include <sstream>

using namespace Slic3r;
namespace AI = Slic3r::AIPrintAssistant;
using json   = nlohmann::json;

namespace {
struct SliceFixture
{
    GCodeProcessorResult result;
    SliceFixture()
    {
        result.reset();
        result.id       = 42;
        result.filename = "private-local-path.gcode";
        for (auto& mode : result.print_statistics.modes)
            mode.reset();
    }
    void index(const std::string& text)
    {
        result.lines_ends.clear();
        for (size_t i = 0; i < text.size(); ++i)
            if (text[i] == '\n')
                result.lines_ends.push_back(i + 1);
    }
    json args(size_t start = 1, size_t count = 40) const
    {
        return {{"slice_id", AI::slice_identity(result, 2)}, {"start_line", start}, {"line_count", count}};
    }
};
} // namespace

TEST_CASE("Assistant slice summary reports estimates warnings and material units without local paths", "[AIPrintAssistant][fast]")
{
    SliceFixture fixture;
    auto&        result                                   = fixture.result;
    auto&        mode                                     = result.print_statistics.modes[0];
    mode.time                                             = 1234;
    mode.prepare_time                                     = 34;
    mode.layers_times                                     = {100, 200, 300};
    mode.roles_times                                      = {{erExternalPerimeter, 55}};
    result.print_statistics.total_volumes_per_extruder[0] = 1000;
    result.filament_densities                             = {1.24f};
    result.filament_diameters                             = {1.75f};
    result.toolpath_outside                               = true;
    result.warnings.push_back({2, "Toolpath warning", "code", {"object"}});
    result.conflict_result = ConflictResult("Cube", "Tower", 42.5, nullptr, nullptr);
    const auto summary     = AI::summarize_slice(result, 2, json::object());
    CHECK(summary["estimated_time_seconds"] == 1234);
    CHECK(summary["estimated_layer_count"] == 3);
    CHECK(summary["filaments"][0]["estimated_mass_g"].get<double>() == Catch::Approx(1.24));
    CHECK(summary["filaments"][0]["length_mm"].get<double>() == Catch::Approx(415.7517));
    CHECK(summary["toolpath_outside"] == true);
    CHECK(summary["warnings"][0]["code"] == "code");
    CHECK(summary["toolpath_conflict"]["height_mm"] == 42.5);
    CHECK(summary.dump().find("private-local-path") == std::string::npos);
    CHECK_FALSE(summary.contains("height_window"));
}

TEST_CASE("Assistant height windows include repeated heights and separate travel from extrusion ranges", "[AIPrintAssistant][fast]")
{
    SliceFixture fixture;
    auto&        moves = fixture.result.moves;
    for (int i = 0; i < 7; ++i) {
        GCodeProcessorResult::MoveVertex move;
        move.gcode_id       = 10 + i;
        move.type           = EMoveType::Extrude;
        move.position       = Vec3f(float(i), 20.f, i == 4 ? 100.f : 20.f);
        move.extrusion_role = erExternalPerimeter;
        move.feedrate       = 30 + i;
        move.fan_speed      = 80;
        move.temperature    = 215;
        move.width          = 0.45f;
        move.height         = 0.2f;
        move.mm3_per_mm     = 0.09f;
        moves.push_back(move);
    }
    moves[1].type       = EMoveType::Travel;
    moves[1].feedrate   = 500;
    moves[2].type       = EMoveType::Retract;
    moves[3].type       = EMoveType::Seam;
    const auto  summary = AI::summarize_slice(fixture.result, 2, {{"height_mm", 20}, {"max_moves", 2}});
    const auto& window  = summary["height_window"];
    CHECK(window["matched_moves"] == 5);
    CHECK(window["counts_by_type"]["extrude"] == 3);
    CHECK(window["counts_by_type"]["travel"] == 1);
    CHECK(window["gcode_line_range"] == json({10, 16}));
    CHECK(window["extrusion_ranges"]["feedrate_mm_s"]["max"] == 36);
    CHECK(window["extrusion_ranges"]["volumetric_rate_mm3_s"]["max"].get<double>() == Catch::Approx(3.24));
    CHECK(window["sample_moves"].size() == 2);
    CHECK(window["sample_truncated"] == true);
    const auto empty = AI::summarize_slice(fixture.result, 2, {{"height_mm", 50}})["height_window"];
    CHECK(empty["matched_moves"] == 0);
    CHECK(empty["gcode_line_range"].is_null());
    CHECK(empty["extrusion_ranges"]["feedrate_mm_s"].is_null());
}

TEST_CASE("Assistant G-code reader uses one-based indexed lines with correct paging and slice identity", "[AIPrintAssistant][fast]")
{
    SliceFixture      fixture;
    const std::string text = "; layer 1\r\nG1 X12 Y20 Z0.2 E1\nM106 S255\n; end\n";
    fixture.index(text);
    std::istringstream file(text);
    const auto         window = AI::read_slice_gcode(file, fixture.result, 2, fixture.args(2, 2));
    CHECK(window["lines"][0]["line"] == 2);
    CHECK(window["lines"][0]["text"] == "G1 X12 Y20 Z0.2 E1");
    CHECK(window["lines"][1]["text"] == "M106 S255");
    CHECK(window["next_line"] == 4);
    CHECK(window["end_of_file"] == false);
    const auto last = AI::read_slice_gcode(file, fixture.result, 2, fixture.args(4));
    CHECK(last["lines"].size() == 1);
    CHECK(last["next_line"].is_null());
    CHECK(last["end_of_file"] == true);
    CHECK_THROWS(AI::read_slice_gcode(file, fixture.result, 2, fixture.args(5)));
    CHECK_THROWS(AI::read_slice_gcode(file, fixture.result, 3, fixture.args()));
    const auto old_args = fixture.args();
    ++fixture.result.id;
    CHECK_THROWS(AI::read_slice_gcode(file, fixture.result, 2, old_args));
    std::istringstream changed(text + "G1 X1\n");
    CHECK_THROWS(AI::read_slice_gcode(changed, fixture.result, 2, fixture.args()));
}

TEST_CASE("Assistant G-code excerpts are bounded and handle long Unicode comments", "[AIPrintAssistant][fast]")
{
    SliceFixture fixture;
    std::string  text;
    for (int i = 0; i < 200; ++i)
        text += ";" + std::string(2046, 'x') + "\xc3\xa9" + std::string(1000, 'y') + "\n";
    fixture.index(text);
    std::istringstream file(text);
    const auto         window = AI::read_slice_gcode(file, fixture.result, 2, fixture.args(1, 200));
    CHECK(window["lines"].size() == 32);
    CHECK(window["next_line"] == 33);
    CHECK(window["lines"][0]["truncated"] == true);
    CHECK_NOTHROW(window.dump());
    CHECK(window.dump().size() < 70000);
    CHECK(window["lines"][0]["text"].get<std::string>().find('y') == std::string::npos);
    std::string binary("G1 X1\0hidden\n", 13);
    fixture.index(binary);
    std::istringstream binary_file(binary);
    CHECK_THROWS(AI::read_slice_gcode(binary_file, fixture.result, 2, fixture.args()));
}

TEST_CASE("Assistant slice tools reject invalid and arbitrary path arguments", "[AIPrintAssistant][fast]")
{
    for (const auto& args :
         {json::array(), json({{"height_mm", -1}}), json({{"height_mm", "20"}}), json({{"height_mm", 20}, {"radius_mm", 0}}),
          json({{"height_mm", 20}, {"max_moves", 81}}), json({{"max_moves", 20}}), json({{"path", "private.gcode"}})})
        CHECK_THROWS(AI::validate_slice_query(args));
    SliceFixture fixture;
    for (const auto& args : {json::object(), json({{"slice_id", 42}}), json({{"slice_id", "42"}, {"start_line", 0}}),
                             json({{"slice_id", "42"}, {"line_count", 201}}), json({{"slice_id", "42"}, {"start_line", 1.5}}),
                             json({{"slice_id", "42"}, {"filename", "private.gcode"}})})
        CHECK_THROWS(AI::validate_gcode_query(args));
    CHECK_NOTHROW(AI::validate_slice_query(json::object()));
    CHECK_NOTHROW(AI::validate_slice_query({{"height_mm", 20}, {"max_moves", 0}}));
    CHECK_NOTHROW(AI::validate_gcode_query(fixture.args()));
}

TEST_CASE("Assistant slice tools are exposed on both providers with evidence-aware instructions", "[AIPrintAssistant][fast]")
{
    for (auto provider : {AI::Provider::OpenRouter, AI::Provider::Anthropic}) {
        const auto request = json::parse(
            AI::make_chat_request(provider, AI::provider_info(provider).default_model,
                                  json::array({AI::chat_user_message(provider, "Why does it fail at 20 mm?")})));
        size_t slice_tools = 0;
        for (const auto& tool : request["tools"]) {
            const auto definition = provider == AI::Provider::OpenRouter ? tool["function"] : tool;
            const auto schema     = definition[provider == AI::Provider::OpenRouter ? "parameters" : "input_schema"];
            if (definition["name"] == "get_slice_context") {
                CHECK(schema == AI::slice_query_schema());
                ++slice_tools;
            } else if (definition["name"] == "read_gcode") {
                CHECK(schema == AI::gcode_query_schema());
                ++slice_tools;
            }
        }
        CHECK(slice_tools == 2);
        const std::string system = provider == AI::Provider::OpenRouter ? request["messages"][0]["content"].get<std::string>() :
                                                                          request["system"].get<std::string>();
        CHECK(system.find("Use get_slice_context before diagnosing") != std::string::npos);
        CHECK(system.find("No mesh, live slice analysis") == std::string::npos);
        CHECK(system.find("old slice is obsolete") != std::string::npos);
    }
}

TEST_CASE("Assistant slice summary consumes real processed extrusion data at the requested height", "[AIPrintAssistant][fast]")
{
    GCodeProcessor processor;
    processor.initialize("unused-test.gcode");
    processor.process_buffer("G90\nM83\nM104 S215\nM106 S255\nG1 Z0.2 F600\n"
                             ";TYPE:External perimeter\n;WIDTH:0.45\n;HEIGHT:0.2\n"
                             "G1 X10 Y0 E1 F1800\nG1 X10 Y10 E1\nG1 Z1.0\nG1 X20 E1\n");
    processor.finalize(false);
    const auto  summary = AI::summarize_slice(processor.get_result(), 0, {{"height_mm", 0.2}, {"radius_mm", 0.01}});
    const auto& window  = summary["height_window"];
    CHECK(window["counts_by_type"]["extrude"] == 2);
    CHECK(window["extrusion_ranges"]["feedrate_mm_s"]["min"].get<double>() == Catch::Approx(30));
    CHECK(window["extrusion_ranges"]["fan_percent"]["max"].get<double>() == Catch::Approx(100));
    CHECK(window["extrusion_ranges"]["nozzle_temperature_c"]["min"].get<double>() == Catch::Approx(215));
}
