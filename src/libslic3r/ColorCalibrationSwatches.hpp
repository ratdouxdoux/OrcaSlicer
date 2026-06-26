#ifndef slic3r_ColorCalibrationSwatches_hpp_
#define slic3r_ColorCalibrationSwatches_hpp_

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
namespace ColorCalibrationSwatches {

enum class SwatchType {
    ReflectiveAnchor,
    TDLadder,
    PairMix,
    PairOrder,
    TernaryMix,
    QuaternaryMix,
    LayerLineStrip
};

enum class BackingType {
    None,
    Black,
    White,
    Custom
};

enum class ValidationSeverity {
    Warning,
    Error
};

struct FilamentSlot
{
    unsigned int          slot = 1;
    std::string           name;
    std::string           short_label;
    std::string           color_hex;
    std::optional<double> td;
};

struct Backing
{
    BackingType           type = BackingType::None;
    unsigned int          slot = 0;
    std::string           label;
    std::string           color_hex;
    std::optional<double> td;
};

struct PlatePosition
{
    unsigned int plate_index = 0;
    unsigned int row         = 0;
    unsigned int column      = 0;
    double       x_mm        = 0.0;
    double       y_mm        = 0.0;
};

struct IdFormatOptions
{
    std::string separator = "_";

    bool include_swatch_type_prefix = false;
    bool include_top_material       = true;
    bool include_backing            = true;
    bool include_thickness          = false;
};

struct BackTextFormatOptions
{
    std::string separator = "_";

    bool   enabled                   = true;
    bool   wrap_lines                = true;
    size_t max_chars_per_line        = 12;
    bool   include_swatch_type_prefix = false;
    bool   include_swatch_id         = false;
    bool   include_top_material      = true;
    bool   include_backing           = true;
    bool   include_thickness         = false;
    bool   include_filament_td_values = false;
    bool   include_percentages       = true;
    bool   include_layer_cycle       = true;
    bool   use_full_filament_names   = false;

    bool   embossed         = false;
    double text_size_mm     = 4.5;
    double text_depth_mm    = 0.35;
    double line_spacing_mm  = 0.45;
    double margin_mm        = 1.0;
    bool   mirror           = true;
    double rotation_degrees = 0.0;
};

struct SwatchReferenceOptions
{
    bool        enabled         = true;
    // Use a unique starting letter/reference for each printed run. Additional plates increment from this value.
    std::string plate_reference = "A";
    double      text_size_mm    = 4.5;
    double      text_depth_mm   = 0.35;
    double      stroke_width_mm = 0.30;
    double      margin_mm       = 1.0;
};

struct SwatchSpec
{
    SwatchType                type = SwatchType::ReflectiveAnchor;
    std::vector<FilamentSlot> filaments;
    std::vector<int>          ratios;

    Backing backing;

    double       total_thickness_mm = 0.0;
    double       layer_height_mm    = 0.0;
    unsigned int top_material_slot  = 0;
    unsigned int top_layer_count    = 0;

    // Bottom-to-top stack order where it is meaningful.
    std::vector<unsigned int> stack_order;

    std::string measurement_side = "side";
};

struct SwatchRecord
{
    SwatchSpec              spec;
    std::string             swatch_id;
    std::string             object_name;
    std::vector<std::string> volume_names;
    std::string             printed_reference;
    std::string             plate_reference;
    unsigned int            sample_number = 0;
    PlatePosition           position;
    std::vector<std::string> warnings;
};

struct SwatchFamilySelection
{
    bool reflective_anchor = true;
    bool td_ladder         = false;
    bool pair_mix          = true;
    bool pair_order        = false;
    bool ternary_mix       = false;
    bool quaternary_mix    = false;
    bool layer_line_strip  = false;
};

struct SwatchLayoutOptions
{
    double chip_width_mm  = 20.0;
    double chip_depth_mm  = 20.0;
    double spacing_x_mm   = 4.0;
    double spacing_y_mm   = 4.0;
    // Zero means derive from the generated swatches' actual bed footprint.
    double footprint_depth_mm = 0.0;
    double margin_x_mm    = 5.0;
    double margin_y_mm    = 5.0;
    double plate_width_mm = 220.0;
    double plate_depth_mm = 220.0;

    bool   reserve_prime_tower = true;
    double prime_tower_width_mm = 70.0;
    double prime_tower_depth_mm = 70.0;

    bool multi_plate = true;
};

struct PlateLabelOptions
{
    bool        enabled            = false;
    std::string title              = "Calibration Swatches";
    double      reserved_height_mm = 36.0;
    double      reserved_width_mm  = 100.0;
    double      margin_x_mm        = 5.0;
    double      text_size_mm       = 10.0;
    double      text_depth_mm      = 0.45;
    // Additional text stroke thickening. Zero means use the font's normal stroke.
    double      stroke_width_mm    = 0.5;
};

struct SpectroJigOptions
{
    bool         enabled            = false;
    double       diameter_mm        = 45.0;
    double       thickness_mm       = 6.0;
    double       clearance_mm       = 3.0;
    bool         wall_enabled       = true;
    double       ring_clearance_mm  = 0.5;
    double       wall_thickness_mm  = 3.0;
    double       wall_height_mm     = 10.0;
    double       wall_arc_degrees   = 360.0;
    unsigned int filament_slot      = 1;
};

struct SwatchGeneratorConfig
{
    std::vector<FilamentSlot> filaments;

    SwatchFamilySelection families;
    SwatchLayoutOptions   layout;
    PlateLabelOptions     plate_label;
    SpectroJigOptions     spectro_jig;
    IdFormatOptions       id_format;
    SwatchReferenceOptions swatch_reference;

    double nominal_layer_height_mm     = 0.2;
    bool   local_z_enabled             = false;
    bool   local_z_direct_multicolor   = false;

    double anchor_thickness_mm             = 6.0;
    std::vector<Backing> anchor_backings;

    std::vector<double>  td_ladder_thicknesses;
    std::vector<Backing> td_ladder_backings;

    double pair_mix_thickness_mm = 6.0;
    double pair_mix_layer_height_mm = 0.2;
    // Automatic pair mixes generate canonical 1:N and N:1 proportions up to this value.
    unsigned int pair_ratio_layer_limit = 7;
    std::vector<std::vector<int>> pair_mix_ratios;
    std::vector<Backing> pair_mix_backings;

    double pair_order_thickness_mm = 6.0;
    double pair_order_layer_height_mm = 0.2;
    unsigned int pair_order_top_layer_count = 1;
    std::vector<std::vector<int>> pair_order_ratios;
    std::vector<Backing> pair_order_backings;

    double ternary_thickness_mm = 6.0;
    double ternary_layer_height_mm = 0.2;
    // Empty means generated from pair_ratio_layer_limit as reduced positive ternary proportions.
    std::vector<std::vector<int>> ternary_ratios;
    std::vector<Backing> ternary_backings;

    double quaternary_thickness_mm = 6.0;
    double quaternary_layer_height_mm = 0.2;
    // Empty means generated from all reduced positive 4-part proportions where each component is <= this value.
    unsigned int quaternary_ratio_layer_limit = 3;
    std::vector<std::vector<int>> quaternary_ratios;
    std::vector<Backing> quaternary_backings;

    double layer_line_strip_thickness_mm = 6.0;
    double layer_line_strip_layer_height_mm = 0.2;
    std::vector<Backing> layer_line_strip_backings;
};

struct SwatchPlan
{
    std::vector<SwatchRecord> records;
    std::vector<std::string>  warnings;
    std::vector<FilamentSlot> primary_filaments;
    std::string               title;
    std::string               swatch_reference_start = "A";
    double                    nominal_layer_height_mm   = 0.0;
    double                    swatch_depth_mm           = 0.0;
    bool                      local_z_enabled           = false;
    bool                      local_z_direct_multicolor = false;
};

struct ValidationIssue
{
    ValidationSeverity severity = ValidationSeverity::Error;
    std::string        swatch_id;
    std::string        message;
};

std::string swatch_type_key(SwatchType type);
std::string swatch_type_prefix(SwatchType type);
std::string backing_label(const Backing &backing);
bool        has_backing(const Backing &backing);

std::string format_decimal_token(double value);
std::string make_swatch_id(const SwatchSpec &spec, const IdFormatOptions &options = {});

SwatchPlan generate_swatch_plan(const SwatchGeneratorConfig &config);

std::vector<ValidationIssue> validate_swatch_plan(const SwatchPlan &plan,
                                                  const SwatchGeneratorConfig &config = {});

nlohmann::json swatch_record_to_json(const SwatchRecord &record);
nlohmann::json manifest_json(const SwatchPlan &plan,
                             const std::string &manifest_name = "calibration-swatches.manifest.json",
                             const std::string &csv_name      = "calibration-swatches.csv");
std::string    manifest_json_string(const SwatchPlan &plan,
                                    const std::string &manifest_name = "calibration-swatches.manifest.json",
                                    const std::string &csv_name      = "calibration-swatches.csv");
std::string    manifest_csv_string(const SwatchPlan &plan);

} // namespace ColorCalibrationSwatches
} // namespace Slic3r

#endif // slic3r_ColorCalibrationSwatches_hpp_
