#include <catch2/catch.hpp>

#include "libslic3r/ColorCalibrationSwatches.hpp"

#include <algorithm>

using namespace Slic3r::ColorCalibrationSwatches;

namespace {

static FilamentSlot filament(unsigned int slot, const char *name, const char *label, const char *color, double td)
{
    FilamentSlot f;
    f.slot        = slot;
    f.name        = name;
    f.short_label = label;
    f.color_hex   = color;
    f.td          = td;
    return f;
}

static Backing black_backing()
{
    Backing backing;
    backing.type      = BackingType::Black;
    backing.slot      = 9;
    backing.label     = "Black";
    backing.color_hex = "#000000";
    return backing;
}

} // namespace

TEST_CASE("Calibration swatch compact IDs and back text match requested defaults", "[ColorCalibrationSwatches]")
{
    const FilamentSlot f1 = filament(1, "PLA Red", "RED", "#FF0000", 6.0);
    const FilamentSlot f2 = filament(2, "PLA Blue", "BLUE", "#0000FF", 5.5);
    const FilamentSlot f3 = filament(3, "PLA White", "WHITE", "#FFFFFF", 2.0);

    SwatchSpec pair;
    pair.type      = SwatchType::PairMix;
    pair.filaments = { f1, f2 };
    pair.ratios    = { 50, 50 };
    CHECK(make_swatch_id(pair) == "1_2_50_50");
    CHECK(make_back_text(pair) == "1_2\n50_50\n50_50%");

    pair.ratios = { 3, 5 };
    CHECK(make_back_text(pair) == "1_2\n3_5\n37.5_62.5%");

    SwatchSpec order = pair;
    order.ratios = { 50, 50 };
    order.type              = SwatchType::PairOrder;
    order.top_material_slot = 1;
    CHECK(make_swatch_id(order) == "1_2_50_50_1TOP");
    CHECK(make_back_text(order) == "1_2\n50_50\n50_50%\n1TOP");

    SwatchSpec ternary;
    ternary.type      = SwatchType::TernaryMix;
    ternary.filaments = { f1, f2, f3 };
    ternary.ratios    = { 33, 33, 34 };
    CHECK(make_swatch_id(ternary) == "1_2_3_33_33_34");
    CHECK(make_back_text(ternary) == "1_2_3\n33_33_34\n33_33_34%\n16_16_16\n123x16");

    SwatchSpec td_ladder;
    td_ladder.type               = SwatchType::TDLadder;
    td_ladder.filaments          = { f1 };
    td_ladder.total_thickness_mm = 0.8;
    CHECK(make_swatch_id(td_ladder) == "1_TD_0p8");
    CHECK(make_back_text(td_ladder) == "1\nTD 0p8");

    td_ladder.total_thickness_mm = 1.2;
    td_ladder.backing            = black_backing();
    CHECK(make_swatch_id(td_ladder) == "1_TD_1p2_BLACK");
    CHECK(make_back_text(td_ladder) == "1\nTD 1p2\nBLACK");
}

TEST_CASE("Calibration swatch formatting options can change separator prefix and wrapping", "[ColorCalibrationSwatches]")
{
    SwatchSpec pair;
    pair.type      = SwatchType::PairMix;
    pair.filaments = {
        filament(1, "Long Red Filament", "RED", "#FF0000", 6.0),
        filament(2, "Long Blue Filament", "BLUE", "#0000FF", 5.0)
    };
    pair.ratios = { 75, 25 };

    IdFormatOptions id_options;
    id_options.separator = "-";
    id_options.include_swatch_type_prefix = true;
    CHECK(make_swatch_id(pair, id_options) == "P-1-2-75-25");

    BackTextFormatOptions text_options;
    text_options.separator = "_";
    text_options.max_chars_per_line = 3;
    text_options.include_percentages = false;
    CHECK(make_back_text(pair, text_options, id_options) == "1_2\n75\n25");

    text_options.wrap_lines = false;
    CHECK(make_back_text(pair, text_options, id_options) == "P-1-2-75-25");
}

TEST_CASE("Calibration swatch plan generates anchor TD ladder and pair grids with manifest details", "[ColorCalibrationSwatches]")
{
    SwatchGeneratorConfig config;
    config.filaments = {
        filament(1, "PLA Red", "RED", "#FF0000", 6.0),
        filament(2, "PLA Blue", "BLUE", "#0000FF", 5.0),
        filament(3, "PLA White", "WHITE", "#FFFFFF", 2.0)
    };
    config.families.td_ladder = true;
    config.td_ladder_thicknesses = { 0.8, 1.2 };
    config.pair_mix_ratios       = { { 1, 7 }, { 2, 3 } };
    config.plate_label.title       = "Test Calibration";
    config.anchor_thickness_mm     = 6.5;
    config.nominal_layer_height_mm = 0.16;
    config.pair_mix_layer_height_mm = 0.16;
    config.local_z_enabled = true;

    SwatchPlan plan = generate_swatch_plan(config);
    REQUIRE(plan.records.size() == 15);
    CHECK(validate_swatch_plan(plan, config).empty());

    const nlohmann::json manifest = manifest_json(plan);
    CHECK(manifest["schema"] == "fullspectrum.calibration_swatches.v1");
    CHECK(manifest["title"] == "Test Calibration");
    CHECK(manifest["count"] == 15);
    REQUIRE(manifest["primary_filaments"].is_array());
    CHECK(manifest["primary_filaments"].size() == 3);
    CHECK(manifest["primary_filaments"][0]["slot"] == 1);
    CHECK(manifest["primary_filaments"][0]["td"] == 6.0);
    CHECK(manifest["primary_colors"]["1"] == "#FF0000");
    CHECK(manifest["primary_td_values"]["1"] == 6.0);
    CHECK(manifest["primary_td_values"]["3"] == 2.0);
    CHECK(manifest["layer_height_mm"] == 0.16);
    CHECK(manifest["swatch_depth_mm"] == 6.5);
    CHECK(manifest["local_z_enabled"] == true);
    CHECK(manifest["records"].size() == 15);

    auto it = std::find_if(plan.records.begin(), plan.records.end(), [](const SwatchRecord &record) {
        return record.swatch_id == "1_2_2_3";
    });
    REQUIRE(it != plan.records.end());
    const nlohmann::json record_json = swatch_record_to_json(*it);
    CHECK(record_json["filament_names"][0] == "PLA Red");
    CHECK(record_json["filament_slots"][1] == 2);
    CHECK(record_json["colors"][0] == "#FF0000");
    CHECK(record_json["td_values"][0] == 6.0);
    CHECK(record_json["ratios"][0] == 2);
    CHECK(record_json["ratios"][1] == 3);
    CHECK(record_json["percentages"][0] == 40.0);
    CHECK(record_json["percentages"][1] == 60.0);
    CHECK(record_json["effective_layer_counts"][0] == 2);
    CHECK(record_json["effective_layer_counts"][1] == 3);
    CHECK(record_json["effective_layer_sequence"][0] == 1);
    CHECK(record_json["effective_layer_sequence"][1] == 1);
    CHECK(record_json["effective_layer_sequence"][2] == 2);
    CHECK(record_json["effective_layer_sequence"][3] == 2);
    CHECK(record_json["effective_layer_sequence"][4] == 2);
    CHECK(record_json["effective_layer_cycle"] == "11222");
    CHECK(record_json["back_text"] == "1_2\n2_3\n40_60%");
    CHECK(record_json["layer_height_mm"] == 0.16);
    CHECK(record_json["measurement_side"] == "side");
    CHECK(record_json["object_name"] == "CS_1_2_2_3");

    auto anchor_it = std::find_if(plan.records.begin(), plan.records.end(), [](const SwatchRecord &record) {
        return record.swatch_id == "1";
    });
    REQUIRE(anchor_it != plan.records.end());
    CHECK(swatch_record_to_json(*anchor_it)["layer_height_mm"] == 0.16);

    const std::string csv = manifest_csv_string(plan);
    CHECK(csv.find("swatch_id,swatch_type,plate_index") == 0);
    CHECK(csv.find("1_2_2_3,pair_mix") != std::string::npos);
    CHECK(csv.find(",side,") != std::string::npos);
}

TEST_CASE("Calibration swatch pair ratios default to canonical one-to-many proportions", "[ColorCalibrationSwatches]")
{
    SwatchGeneratorConfig config;
    config.filaments = {
        filament(1, "PLA Red", "RED", "#FF0000", 6.0),
        filament(2, "PLA Blue", "BLUE", "#0000FF", 5.0)
    };
    config.families.reflective_anchor = false;
    config.families.td_ladder         = false;
    config.families.pair_order        = false;
    config.families.ternary_mix       = false;
    config.pair_ratio_layer_limit     = 7;

    SwatchPlan plan = generate_swatch_plan(config);
    REQUIRE(plan.records.size() == 13);
    CHECK(validate_swatch_plan(plan, config).empty());

    auto has_id = [&plan](const std::string &id) {
        return std::any_of(plan.records.begin(), plan.records.end(), [&id](const SwatchRecord &record) {
            return record.swatch_id == id;
        });
    };
    CHECK(has_id("1_2_1_1"));
    CHECK(has_id("1_2_1_7"));
    CHECK(has_id("1_2_1_2"));
    CHECK(has_id("1_2_7_1"));
    CHECK_FALSE(has_id("1_2_2_3"));
    CHECK_FALSE(has_id("1_2_2_4"));
    CHECK_FALSE(has_id("1_2_3_6"));
}

TEST_CASE("Calibration swatch records group pair swatches by filament combination then ratio", "[ColorCalibrationSwatches]")
{
    SwatchGeneratorConfig config;
    config.filaments = {
        filament(1, "PLA Red", "RED", "#FF0000", 6.0),
        filament(2, "PLA Blue", "BLUE", "#0000FF", 5.0),
        filament(3, "PLA White", "WHITE", "#FFFFFF", 2.0),
        filament(4, "PLA Yellow", "YELLOW", "#FFFF00", 3.0)
    };
    config.families.reflective_anchor = false;
    config.families.td_ladder         = false;
    config.families.pair_order        = false;
    config.families.ternary_mix       = false;
    config.pair_ratio_layer_limit     = 3;

    SwatchPlan plan = generate_swatch_plan(config);
    REQUIRE(plan.records.size() == 30);

    CHECK(plan.records[0].swatch_id == "1_2_1_1");
    CHECK(plan.records[1].swatch_id == "1_2_1_2");
    CHECK(plan.records[2].swatch_id == "1_2_1_3");
    CHECK(plan.records[3].swatch_id == "1_2_2_1");
    CHECK(plan.records[4].swatch_id == "1_2_3_1");
    CHECK(plan.records[5].swatch_id == "1_3_1_1");
    CHECK(plan.records[6].swatch_id == "1_3_1_2");
    CHECK(plan.records[7].swatch_id == "1_3_1_3");
    CHECK(plan.records[10].swatch_id == "1_4_1_1");
    CHECK(plan.records[15].swatch_id == "2_3_1_1");
    CHECK(plan.records[20].swatch_id == "2_4_1_1");
}

TEST_CASE("Calibration swatch plate label reserves only the lower-right corner", "[ColorCalibrationSwatches]")
{
    SwatchGeneratorConfig config;
    config.filaments = {
        filament(1, "PLA Red", "RED", "#FF0000", 6.0),
        filament(2, "PLA Blue", "BLUE", "#0000FF", 5.0)
    };
    config.families.reflective_anchor = true;
    config.families.td_ladder         = false;
    config.families.pair_mix          = false;
    config.families.ternary_mix       = false;
    config.layout.chip_width_mm       = 20.0;
    config.layout.chip_depth_mm       = 20.0;
    config.layout.spacing_x_mm        = 0.0;
    config.layout.spacing_y_mm        = 0.0;
    config.layout.margin_x_mm         = 5.0;
    config.layout.margin_y_mm         = 5.0;
    config.layout.plate_width_mm      = 50.0;
    config.layout.plate_depth_mm      = 50.0;
    config.layout.reserve_prime_tower = false;

    SwatchPlan unlabeled = generate_swatch_plan(config);
    REQUIRE(unlabeled.records.size() == 2);
    CHECK(unlabeled.records.front().position.y_mm == Approx(8.0));
    CHECK(unlabeled.records[1].position.x_mm == Approx(35.0));
    CHECK(unlabeled.records[1].position.y_mm == Approx(8.0));

    config.plate_label.enabled = true;
    config.plate_label.reserved_height_mm = 22.0;
    config.plate_label.reserved_width_mm = 22.0;
    SwatchPlan labeled = generate_swatch_plan(config);
    REQUIRE(labeled.records.size() == 2);
    CHECK(labeled.records.front().position.x_mm == Approx(15.0));
    CHECK(labeled.records.front().position.y_mm == Approx(8.0));
    CHECK(labeled.records[1].position.x_mm == Approx(15.0));
    CHECK(labeled.records[1].position.y_mm == Approx(14.0));
}

TEST_CASE("Calibration swatch layout reserves top-left prime tower space", "[ColorCalibrationSwatches]")
{
    SwatchGeneratorConfig config;
    for (unsigned int slot = 1; slot <= 12; ++slot)
        config.filaments.emplace_back(filament(slot, "PLA", std::to_string(slot).c_str(), "#AAAAAA", 3.0));

    config.families.reflective_anchor = true;
    config.families.td_ladder         = false;
    config.families.pair_mix          = false;
    config.families.ternary_mix       = false;
    config.layout.chip_width_mm       = 20.0;
    config.layout.footprint_depth_mm  = 10.0;
    config.layout.spacing_x_mm        = 0.0;
    config.layout.spacing_y_mm        = 0.0;
    config.layout.margin_x_mm         = 5.0;
    config.layout.margin_y_mm         = 5.0;
    config.layout.plate_width_mm      = 60.0;
    config.layout.plate_depth_mm      = 60.0;
    config.layout.reserve_prime_tower = true;
    config.layout.prime_tower_width_mm = 30.0;
    config.layout.prime_tower_depth_mm = 20.0;

    const SwatchPlan plan = generate_swatch_plan(config);
    REQUIRE(plan.records.size() == 12);

    const double reserve_min_x = config.layout.margin_x_mm;
    const double reserve_max_x = reserve_min_x + config.layout.prime_tower_width_mm;
    const double reserve_max_y = config.layout.plate_depth_mm - config.layout.margin_y_mm;
    const double reserve_min_y = reserve_max_y - config.layout.prime_tower_depth_mm;

    auto overlaps_reserve = [&](const SwatchRecord &record) {
        const double min_x = record.position.x_mm - config.layout.chip_width_mm * 0.5;
        const double max_x = record.position.x_mm + config.layout.chip_width_mm * 0.5;
        const double min_y = record.position.y_mm - config.layout.footprint_depth_mm * 0.5;
        const double max_y = record.position.y_mm + config.layout.footprint_depth_mm * 0.5;
        return min_x < reserve_max_x && max_x > reserve_min_x && min_y < reserve_max_y && max_y > reserve_min_y;
    };

    CHECK(std::none_of(plan.records.begin(), plan.records.end(), overlaps_reserve));
}

TEST_CASE("Calibration swatch ternary ratios auto-generate reduced positive proportions", "[ColorCalibrationSwatches]")
{
    SwatchGeneratorConfig config;
    config.filaments = {
        filament(1, "PLA Red", "RED", "#FF0000", 6.0),
        filament(2, "PLA Blue", "BLUE", "#0000FF", 5.0),
        filament(3, "PLA White", "WHITE", "#FFFFFF", 2.0),
        filament(4, "PLA Yellow", "YELLOW", "#FFFF00", 3.0)
    };
    config.families.reflective_anchor = false;
    config.families.td_ladder         = false;
    config.families.pair_mix          = false;
    config.families.pair_order        = false;
    config.families.ternary_mix       = true;
    config.pair_ratio_layer_limit     = 7;
    config.back_text_format.max_chars_per_line = 64;

    SwatchPlan plan = generate_swatch_plan(config);
    REQUIRE(plan.records.size() == 136);
    CHECK(validate_swatch_plan(plan, config).empty());

    CHECK(plan.records[0].swatch_id == "1_2_3_33_33_34");
    CHECK(plan.records[1].swatch_id == "1_2_3_25_25_50");
    CHECK(plan.records[33].swatch_id == "1_2_3_72_14_14");
    CHECK(plan.records[34].swatch_id == "1_2_4_33_33_34");

    auto it = std::find_if(plan.records.begin(), plan.records.end(), [](const SwatchRecord &record) {
        return record.swatch_id == "1_2_3_14_14_72";
    });
    REQUIRE(it != plan.records.end());
    CHECK(it->spec.ratios == std::vector<int>({ 1, 1, 5 }));
    CHECK(it->back_text.find("1_1_5") != std::string::npos);
    CHECK(it->back_text.find("14.3_14.3_71.4%") != std::string::npos);
}

TEST_CASE("Calibration swatch validation catches duplicate compact IDs and high TD anchor warnings", "[ColorCalibrationSwatches]")
{
    SwatchGeneratorConfig config;
    config.filaments = { filament(1, "PLA Clear", "CLR", "#F8FFFF", 6.0) };
    config.families.td_ladder = false;
    config.families.pair_mix  = false;
    config.anchor_thickness_mm = 1.0;
    config.anchor_backings = { {}, black_backing() };
    config.id_format.include_backing = false;

    SwatchPlan plan = generate_swatch_plan(config);
    REQUIRE(plan.records.size() == 2);

    const std::vector<ValidationIssue> issues = validate_swatch_plan(plan, config);
    CHECK(std::any_of(issues.begin(), issues.end(), [](const ValidationIssue &issue) {
        return issue.severity == ValidationSeverity::Error && issue.message == "duplicate swatch id";
    }));
    CHECK(std::any_of(issues.begin(), issues.end(), [](const ValidationIssue &issue) {
        return issue.severity == ValidationSeverity::Warning &&
               issue.message.find("not an opaque anchor") != std::string::npos;
    }));
}

TEST_CASE("Calibration swatch disabled families generate zero records for that family", "[ColorCalibrationSwatches]")
{
    SwatchGeneratorConfig config;
    config.filaments = {
        filament(1, "PLA Red", "RED", "#FF0000", 6.0),
        filament(2, "PLA Blue", "BLUE", "#0000FF", 5.0)
    };
    config.families.reflective_anchor = false;
    config.families.td_ladder         = false;
    config.families.pair_mix          = false;

    const SwatchPlan plan = generate_swatch_plan(config);
    CHECK(plan.records.empty());
}
