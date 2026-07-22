#ifndef slic3r_MixedFilamentExport_hpp_
#define slic3r_MixedFilamentExport_hpp_

#include "MixedFilament.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

struct MixedFilamentExportPhysicalFilament
{
    unsigned int          id = 0;
    std::string           name;
    std::string           color_hex;
    std::optional<double> td_mm;
    bool                  selected = true;
};

struct MixedFilamentExportComponent
{
    unsigned int physical_filament_id = 0;
    int          ratio                = 0;
};

enum class MixedFilamentExportRecipeSource : uint8_t { Current, Automatic };

struct MixedFilamentExportRecipe
{
    std::string                               recipe_id;
    std::string                               name;
    MixedFilamentExportRecipeSource           source = MixedFilamentExportRecipeSource::Automatic;
    std::vector<MixedFilamentExportComponent> components;
    std::string                               filament_mixer_hex;
    std::optional<std::string>                km_ks_hex;
};

struct MixedFilamentExportPredictionOptions
{
    bool include_km_ks = false;
    bool use_td        = true;
};

struct MixedFilamentAutomaticExportOptions
{
    bool                                 include_two_filament   = true;
    bool                                 include_three_filament = false;
    bool                                 include_four_filament  = false;
    MixedFilamentExportPredictionOptions prediction;
};

struct MixedFilamentXlsxWriteResult
{
    bool        success = false;
    std::string error;

    explicit operator bool() const { return success; }
};

// Convert the enabled, non-deleted mixed rows to their effective display
// proportions and predict their colours through explicitly selected engines.
std::vector<MixedFilamentExportRecipe> make_current_mixed_filament_export_recipes(
    const std::vector<MixedFilamentExportPhysicalFilament>& physical_filaments,
    const std::vector<MixedFilament>&                       mixed_filaments,
    const MixedFilamentDisplayContext&                      display_context,
    const MixedFilamentExportPredictionOptions&             prediction_options = {});

// Generate the canonical calibration families for every selected physical
// combination: 9 ratios/pair, 10 ratios/triple, and 79 ratios/quadruple.
std::vector<MixedFilamentExportRecipe> make_automatic_mixed_filament_export_recipes(
    const std::vector<MixedFilamentExportPhysicalFilament>& physical_filaments, const MixedFilamentAutomaticExportOptions& options);

// Write a native OpenXML workbook with normalized recipe, component, and
// physical-filament sheets. Existing files at output_path are replaced.
MixedFilamentXlsxWriteResult write_mixed_filament_xlsx(const std::string&                                      output_path,
                                                       const std::vector<MixedFilamentExportPhysicalFilament>& physical_filaments,
                                                       const std::vector<MixedFilamentExportRecipe>&           recipes);

} // namespace Slic3r

#endif // slic3r_MixedFilamentExport_hpp_
