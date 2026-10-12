#include <catch2/catch_test_macros.hpp>

#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/Preset.hpp"

using namespace Slic3r;

TEST_CASE("Filament palette resolves the compatible family profile", "[FilamentPalette]")
{
    std::deque<Preset> presets;
    presets.emplace_back(Preset::TYPE_FILAMENT, "Snapmaker PLA Matte @U1 0.4 nozzle");
    presets.back().is_system     = true;
    presets.back().is_compatible = false;
    presets.emplace_back(Preset::TYPE_FILAMENT, "Snapmaker PLA Matte @U1 0.6 nozzle");
    presets.back().is_system = true;
    presets.emplace_back(Preset::TYPE_FILAMENT, "Snapmaker PLA Silk @U1 0.6 nozzle");
    // Sharing an ID is insufficient: selecting Matte must never apply Silk.
    for (auto& preset : presets)
        preset.filament_id = "shared-id";

    CHECK(FindFilamentFamilyPreset(presets, "Snapmaker PLA Matte @U1", "Generic PLA") == &presets[1]);
    CHECK(FindFilamentFamilyPreset(presets, "Snapmaker PLA Matte", "Generic PLA") == &presets[1]);
    CHECK(FindFilamentFamilyPreset(presets, "Snapmaker PLA Matte @U1 0.8 nozzle", "Generic PLA") == &presets[1]);
    CHECK(FindFilamentFamilyPreset(presets, "Snapmaker ABS @U1", "Generic PLA") == nullptr);
    CHECK(FindFilamentFamilyPreset(presets, "", "Generic PLA") == nullptr);

    SECTION("Hidden compatible profiles can be enabled by a palette selection")
    {
        presets[1].is_visible = false;
        CHECK(FindFilamentFamilyPreset(presets, "Snapmaker PLA Matte", "Generic PLA") == &presets[1]);
    }
    SECTION("A family without a compatible nozzle has no selectable profile")
    {
        presets[1].is_compatible = false;
        CHECK(FindFilamentFamilyPreset(presets, "Snapmaker PLA Matte", presets[0].name) == nullptr);
    }
}

TEST_CASE("Filament palette preserves the current family profile and otherwise prefers system profiles", "[FilamentPalette]")
{
    std::deque<Preset> presets;
    presets.emplace_back(Preset::TYPE_FILAMENT, "Snapmaker PLA Matte @Custom");
    presets.emplace_back(Preset::TYPE_FILAMENT, "Snapmaker PLA Matte @U1");
    presets.back().is_system  = true;
    presets.back().is_visible = false;
    presets.emplace_back(Preset::TYPE_FILAMENT, "Snapmaker PLA Matte @J1");
    presets.back().is_system = true;

    CHECK(FindFilamentFamilyPreset(presets, "Snapmaker PLA Matte", "Generic PLA") == &presets[2]);
    CHECK(FindFilamentFamilyPreset(presets, "Snapmaker PLA Matte", presets[0].name) == &presets[0]);
    CHECK(FindFilamentFamilyPreset(presets, "Snapmaker PLA Matte", Preset::suffix_modified() + presets[0].name) == &presets[0]);
    presets[2].is_compatible = false;
    CHECK(FindFilamentFamilyPreset(presets, "Snapmaker PLA Matte", "Generic PLA") == &presets[1]);
}
