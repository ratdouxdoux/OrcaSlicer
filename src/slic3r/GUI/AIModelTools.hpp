#pragma once

#include "libslic3r/AIGeneratedModel.hpp"
#include "libslic3r/AIModelFeatures.hpp"
#include <atomic>
#include <optional>
#include <thread>

namespace Slic3r::GUI {
class Plater;
nlohmann::json                      ai_plate_context(Plater& plater);
nlohmann::json                      ai_slice_status(Plater& plater);
nlohmann::json                      ai_read_slice_tool(Plater& plater, const std::string& name, const nlohmann::json& arguments);
// Runs the native Slice plate action. Slicing continues in the background; it may not start when a check blocks it.
void                                ai_start_slice(Plater& plater);
// Result of slice_plate once slicing is idle: the slice summary with native warnings, or the reason there is no slice.
nlohmann::json                      ai_slice_outcome(Plater& plater);
// Searches the bundled knowledge base, defaulting to the active printer's machine family.
nlohmann::json                      ai_search_knowledge(const nlohmann::json& arguments);
AIPrintAssistant::ModelToolContext  ai_model_context(Plater& plater, bool include_selection = true);
nlohmann::json                      ai_model_tools(Plater& plater);
AIPrintAssistant::PreparedModelEdit ai_prepare_model_edit(Plater& plater, const AIPrintAssistant::ModelOperation& operation);
void           ai_commit_model_edit(Plater& plater, AIPrintAssistant::PreparedModelEdit&& edit, const std::string& snapshot_name);
nlohmann::json ai_run_native_model_tool(Plater& plater, const AIPrintAssistant::ModelOperation& operation);
// Parts made by generate_model in this session, with their OpenSCAD code.
AIPrintAssistant::GeneratedModels& ai_generated_models();
// The generated part of object_id. Throws unless exactly one of its parts still matches its code.
const ModelVolume& ai_generated_part(Plater& plater, const std::string& object_id);
nlohmann::json     ai_model_source(Plater& plater, const nlohmann::json& arguments);
// get_model_features: detects an active-plate object's features before slicing.
// A report already cached for the object answers at once. Otherwise detection
// runs on a worker thread over a copy of the model, so the window stays
// responsive and the user can stop it. Create, poll and take on the UI thread.
class AIFeatureDetection
{
public:
    AIFeatureDetection(Plater& plater, const nlohmann::json& arguments);
    ~AIFeatureDetection();
    AIFeatureDetection(const AIFeatureDetection&)            = delete;
    AIFeatureDetection& operator=(const AIFeatureDetection&) = delete;

    bool done() const { return m_done.load(); }
    void cancel() { m_cancel.store(true); }
    // The tool result; throws what detection threw. The report stays cached for
    // add_feature_modifier until the object's geometry or settings change.
    nlohmann::json take();

private:
    std::string                                    m_object_id;
    std::vector<std::string>                       m_kinds;
    size_t                                         m_limit = 8;
    Model                                          m_model; // The worker's copy; meshes are shared and immutable.
    std::atomic<bool>                              m_cancel{false}, m_done{false};
    std::optional<AIPrintAssistant::FeatureReport> m_report;
    std::string                                    m_error;
    std::thread                                    m_thread;
};
} // namespace Slic3r::GUI
