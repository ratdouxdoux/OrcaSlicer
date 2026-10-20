#include <catch2/catch_test_macros.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ProjectSchemaVersion.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/LocalesUtils.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

#include <cereal/types/polymorphic.hpp>
#include <cereal/types/string.hpp> 
#include <cereal/types/vector.hpp> 
#include <cereal/archives/binary.hpp>

using namespace Slic3r;

TEST_CASE("Single flow declarations preserve index fallbacks and filament offsets", "[Config][SingleFlowIndex]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_volume_type", new ConfigOptionEnumsGeneric(std::vector<int>{fvtStandard, fvtHighFlow}));
    for (auto domain : {ConfigFlowDomain::Process, ConfigFlowDomain::Printer}) {
        const char* key = flow_support_key(domain);
        for (const std::vector<std::string>& support : std::vector<std::vector<std::string>>{{}, {"standard"}, {"high_flow"}, {"unknown"}}) {
            config.set_key_value(key, new ConfigOptionStrings(support));
            for (unsigned filament : {0u, 1u, 10u})
                CHECK(get_config_idx(config, domain, filament) == 0);
        }
        config.set_key_value(key, new ConfigOptionStrings(std::vector<std::string>{"standard", "high_flow"}));
        CHECK(get_config_idx(config, domain, 0) == 0);
        CHECK(get_config_idx(config, domain, 1) == 1);
        CHECK(get_config_idx(config, domain, 10) == 0);
        config.erase(key);
        CHECK(get_config_idx(config, domain, 1) == 0);
    }
    config.set_key_value("filament_flow_step_size", new ConfigOptionInts(std::vector<int>{2, 2}));
    config.set_key_value("filament_flow_support",
                         new ConfigOptionStrings(std::vector<std::string>{"standard", "high_flow", "standard", "high_flow"}));
    CHECK(get_config_idx(config, ConfigFlowDomain::Filament, 0) == 0);
    CHECK(get_config_idx(config, ConfigFlowDomain::Filament, 1) == 3);
}

TEST_CASE("Measure repeated single-variant process lookups", "[.][SingleFlowLookupBenchmark]")
{
    FullPrintConfig config;
    config.process_flow_support.values = {"standard"};
    config.filament_volume_type.values = {fvtStandard, fvtHighFlow};
    constexpr size_t count             = 2000000;
    size_t           checksum          = 0;
    const auto       start             = std::chrono::steady_clock::now();
    for (size_t i = 0; i < count; ++i)
        checksum += get_config_idx(config, ConfigFlowDomain::Process, unsigned(i % 3));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(checksum == 0);
    std::cout << "Single flow lookup calls=" << count << " elapsed_ms=" << ms << '\n';
}

TEST_CASE("Project schema registry handles versions", "[Config][FlowVariant]")
{
    const auto& schema = ProjectSchemaRegistry::definition();
    REQUIRE(schema.config_key == std::string("project_schema_version"));
    REQUIRE(schema.current_version == 1);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.erase(schema.config_key);
    REQUIRE(ProjectSchemaRegistry::version_from(config) == schema.legacy_version);
    REQUIRE_FALSE(ProjectSchemaRegistry::is_newer(config));

    config.set_key_value(schema.config_key, new ConfigOptionInt(schema.current_version));
    REQUIRE_FALSE(ProjectSchemaRegistry::is_newer(config));

    config.set_key_value(schema.config_key, new ConfigOptionInt(schema.current_version + 1));
    REQUIRE(ProjectSchemaRegistry::is_newer(config));
}

SCENARIO("Generic config validation performs as expected.", "[Config]") {
    GIVEN("A config generated from default options") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        WHEN( "line_width is set to 250%, a valid value") {
            config.set_deserialize_strict("line_width", "250%");
            THEN( "The config is read as valid.") {
                REQUIRE(config.validate().empty());
            }
        }
        WHEN( "line_width is set to -10, an invalid value") {
            config.set("line_width", -10);
            THEN( "Validate returns error") {
                REQUIRE(! config.validate().empty());
            }
        }

        WHEN( "wall_loops is set to -10, an invalid value") {
            config.set("wall_loops", -10);
            THEN( "Validate returns error") {
                REQUIRE(! config.validate().empty());
            }
        }
    }
}

SCENARIO("Config accessor functions perform as expected.", "[Config]") {
    GIVEN("A config generated from default options") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        WHEN("A boolean option is set to a boolean value") {
            REQUIRE_NOTHROW(config.set("gcode_comments", true));
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionBool>("gcode_comments")->getBool() == true);
            }
        }
        WHEN("A boolean option is set to a string value representing a 0 or 1") {
            CHECK_NOTHROW(config.set_deserialize_strict("gcode_comments", "1"));
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionBool>("gcode_comments")->getBool() == true);
            }
        }
        WHEN("A boolean option is set to a string value representing something other than 0 or 1") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("gcode_comments", "Z"), BadOptionTypeException);
            }
            AND_THEN("Value is unchanged.") {
                REQUIRE(config.opt<ConfigOptionBool>("gcode_comments")->getBool() == false);
            }
        }
        WHEN("A boolean option is set to an int value") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("gcode_comments", 1), BadOptionTypeException);
            }
        }
        WHEN("A numeric option is set from serialized string") {
            config.set_deserialize_strict("nozzle_diameter", "0.6");
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionFloats>("nozzle_diameter")->get_at(0) == 0.6);
            }
        }
#if 0
		//FIXME better design accessors for vector elements.
		WHEN("An integer-based option is set through the integer interface") {
            config.set("bed_temperature", 100);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionInts>("bed_temperature")->get_at(0) == 100);
            }
        }
#endif
        WHEN("An floating-point option is set through the integer interface") {
            config.set("bridge_flow", 10);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionFloat>("bridge_flow")->getFloat() == 10.0);
            }
        }
        WHEN("A floating-point option is set through the double interface") {
            config.set("bridge_flow", 5.5);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionFloat>("bridge_flow")->getFloat() == 5.5);
            }
        }
        WHEN("An integer-based option is set through the double interface") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("wall_loops", 5.5), BadOptionTypeException);
            }
        }
        WHEN("A numeric option is set to a non-numeric value.") {
            THEN("A BadOptionTypeException exception is thown.") {
                REQUIRE_THROWS_AS(config.set_deserialize_strict("bridge_flow", "zzzz"), BadOptionValueException);
            }
            THEN("The value does not change.") {
                REQUIRE(config.opt<ConfigOptionFloat>("bridge_flow")->getFloat() == 1.0);
            }
        }
        WHEN("A string option is set through the string interface") {
            config.set("machine_end_gcode", "100");
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("machine_end_gcode")->value == "100");
            }
        }
        WHEN("A string option is set through the integer interface") {
            config.set("machine_end_gcode", 100);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("machine_end_gcode")->value == "100");
            }
        }
        WHEN("A string option is set through the double interface") {
            config.set("machine_end_gcode", 100.5);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("machine_end_gcode")->value == float_to_string_decimal_point(100.5));
            }
        }
        WHEN("A float or percent is set as a percent through the string interface.") {
            config.set_deserialize_strict("initial_layer_line_width", "100%");
            THEN("Value and percent flag are 100/true") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == true);
                REQUIRE(tmp->value == 100);
            }
        }
        WHEN("A float or percent is set as a float through the string interface.") {
            config.set_deserialize_strict("initial_layer_line_width", "100");
            THEN("Value and percent flag are 100/false") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == false);
                REQUIRE(tmp->value == 100);
            }
        }
        WHEN("A float or percent is set as a float through the int interface.") {
            config.set("initial_layer_line_width", 100);
            THEN("Value and percent flag are 100/false") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == false);
                REQUIRE(tmp->value == 100);
            }
        }
        WHEN("A float or percent is set as a float through the double interface.") {
            config.set("initial_layer_line_width", 100.5);
            THEN("Value and percent flag are 100.5/false") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == false);
                REQUIRE(tmp->value == 100.5);
            }
        }
        WHEN("An invalid option is requested during set.") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", 1), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", 1.0), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", "1"), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", true), UnknownOptionException);
            }
        }

        WHEN("An invalid option is requested during get.") {
            THEN("A UnknownOptionException exception is thrown.") {
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionString>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionFloat>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionInt>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionBool>("deadbeef_invalid_option", false), UnknownOptionException);
            }
        }
        WHEN("An invalid option is requested during opt.") {
            THEN("A UnknownOptionException exception is thrown.") {
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionString>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionFloat>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionInt>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionBool>("deadbeef_invalid_option", false), UnknownOptionException);
            }
        }

        WHEN("getX called on an unset option.") {
            THEN("The default is returned.") {
                REQUIRE(config.opt_float("layer_height") == 0.2);
                REQUIRE(config.opt_int("raft_layers") == 0);
                REQUIRE(config.opt_bool("enable_support") == false);
            }
        }

        WHEN("getFloat called on an option that has been set.") {
            config.set("layer_height", 0.5);
            THEN("The set value is returned.") {
                REQUIRE(config.opt_float("layer_height") == 0.5);
            }
        }
    }
}

SCENARIO("Config ini load/save interface", "[Config]") {
    WHEN("new_from_ini is called") {
		Slic3r::DynamicPrintConfig config;
		std::string path = std::string(TEST_DATA_DIR) + "/test_config/new_from_ini.ini";
		config.load_from_ini(path, ForwardCompatibilitySubstitutionRule::Disable);
        THEN("Config object contains ini file options.") {
			REQUIRE(config.option_throw<ConfigOptionStrings>("filament_colour", false)->values.size() == 1);
			REQUIRE(config.option_throw<ConfigOptionStrings>("filament_colour", false)->values.front() == "#ABCD");
        }
    }
}

SCENARIO("DynamicPrintConfig serialization", "[Config]") {
    WHEN("DynamicPrintConfig is serialized and deserialized") {
        FullPrintConfig full_print_config;
        DynamicPrintConfig cfg;
        cfg.apply(full_print_config, false);

        std::string serialized;
        try {
            std::ostringstream ss;
            cereal::BinaryOutputArchive oarchive(ss);
            oarchive(cfg);
            serialized = ss.str();
        } catch (const std::runtime_error & /* e */) {
            // e.what();
        }

        THEN("Config object contains ini file options.") {
            DynamicPrintConfig cfg2;
            try {
                std::stringstream ss(serialized);
                cereal::BinaryInputArchive iarchive(ss);
                iarchive(cfg2);
            } catch (const std::runtime_error & /* e */) {
                // e.what();
            }
            REQUIRE(cfg == cfg2);
        }
    }
}

TEST_CASE("DynamicPrintConfig normalizes support filament types from filament_ids", "[Config]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionStrings>("filament_type", true)->values      = { "PLA", "PA" };
    config.option<ConfigOptionStrings>("filament_ids", true)->values       = { "GFS00", "GFS01" };
    config.option<ConfigOptionBools>("filament_is_support", true)->values  = { true, true };

    std::string display_type;
    CHECK(config.get_filament_type(display_type, 0) == "PLA-S");
    CHECK(display_type == "Sup.PLA");

    CHECK(config.get_filament_type(display_type, 1) == "PA-S");
    CHECK(display_type == "Sup.PA");
}

TEST_CASE("DynamicPrintConfig keeps ordinary filament types unchanged", "[Config]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionStrings>("filament_type", true)->values      = { "PLA" };
    config.option<ConfigOptionStrings>("filament_ids", true)->values       = { "GFSL99" };
    config.option<ConfigOptionBools>("filament_is_support", true)->values  = { false };

    std::string display_type;
    CHECK(config.get_filament_type(display_type, 0) == "PLA");
    CHECK(display_type == "PLA");
}
