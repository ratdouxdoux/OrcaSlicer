#pragma once

#include "BoundingBox.hpp"
#include "ExPolygon.hpp"
#include "PrintConfig.hpp"
#include "TriangleMesh.hpp"
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>

namespace Slic3r {
class ModelObject;

namespace AIPrintAssistant {

// Values from the object's effective configuration that the detectors share
// with the slicer, so a feature means what the slicer will do with it.
struct FeatureParams
{
    double layer_height       = 0.2;
    double first_layer_height = 0.2;
    double line_width         = 0.42; // outer wall
    double nozzle_diameter    = 0.4;
    double support_angle      = 30.;  // degrees from horizontal
    double small_perimeter_r  = 0.;   // small_perimeter_threshold; 0 = off
    double ear_max_angle      = 125.; // brim_ears_max_angle
    double ear_detection      = 1.;   // brim_ears_detection_length
    int    shell_layers       = 6;    // top + bottom
};
FeatureParams feature_params(const DynamicPrintConfig& config);

// A feature's outline between two heights.
struct FeatureSlab
{
    double     z_lo = 0., z_hi = 0.; // world mm
    ExPolygons shape;                // world XY, scaled
};

struct ModelFeature
{
    std::string              kind, id;
    BoundingBoxf3            box; // world mm
    nlohmann::json           measures = nlohmann::json::object();
    ExPolygons               footprint; // world XY, scaled: the whole feature seen from above
    std::vector<FeatureSlab> slabs;     // the outline band by band, bottom up; empty when the footprint is enough
    double                   significance = 0.; // orders features of one kind, largest first
};

struct FeatureReport
{
    std::string               object_id, instance_id;
    std::uint64_t             geometry_key = 0;
    FeatureParams             params;
    std::vector<ModelFeature> features;
};

// Detects geometric features of one instance of an object in world coordinates,
// before slicing. Model parts are united and negative volumes subtracted, as the
// slicer does; modifiers and support volumes are ignored.
FeatureReport detect_model_features(const ModelObject&           object,
                                    size_t                       instance,
                                    const DynamicPrintConfig&    config,
                                    const std::function<void()>& throw_on_cancel = {});
// Changes whenever detection could give a different result: geometry, the
// instance transform or a configuration value the detectors use.
std::uint64_t  feature_geometry_key(const ModelObject& object, size_t instance, const DynamicPrintConfig& config);
// Compact report for the assistant: at most `limit` features of each kind.
nlohmann::json feature_report_json(const FeatureReport& report, const std::vector<std::string>& kinds = {}, size_t limit = 8);
const std::vector<std::string>& feature_kinds();
// A mesh around a feature in world mm, grown by margin in every direction: its
// bounding box, or a stack of short prisms that follows the feature's outline
// band by band, so a slope gets a slab along it rather than the block around it.
// Each prism is closed; they touch but never overlap, so every slicing mode
// reads them alike.
indexed_triangle_set feature_modifier_mesh(const ModelFeature& feature, bool follow, double margin);

} // namespace AIPrintAssistant
} // namespace Slic3r
