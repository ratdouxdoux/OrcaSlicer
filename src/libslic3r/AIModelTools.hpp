#ifndef slic3r_AIModelTools_hpp_
#define slic3r_AIModelTools_hpp_

#include "Model.hpp"
#include "MixedFilament.hpp"
#include <array>
#include <optional>
#include <nlohmann/json.hpp>

namespace Slic3r::AIPrintAssistant {

struct ModelToolContext
{
    std::string    id;
    nlohmann::json data;
};

struct ModelOperation
{
    std::string    context_id, operation, object_id, volume_id;
    nlohmann::json parameters;
};

// Stable, discoverable schemas for native operations. Opening an editor is
// explicitly distinct from applying a parameterized edit.
nlohmann::json   model_tool_catalog();
nlohmann::json   model_operation_schema();
ModelOperation   parse_model_operation(const nlohmann::json& arguments);
ModelToolContext capture_model_tool_context(const Model&                    model,
                                            const std::vector<std::string>& selected_object_ids,
                                            const std::vector<std::string>& colors,
                                            const MixedFilamentManager&     mixes,
                                            const std::string&              project_state = {});

struct PreparedModelEdit
{
    Model                model;
    MixedFilamentManager mixes;
    bool                 model_changed = false;
    bool                 mixes_changed = false;
    nlohmann::json       result;
};

// One checkpoint spans all automatic edits in a live preview, including preset
// changes. Keep it until restore succeeds; a failed restore must remain retryable.
struct PreviewCheckpoint
{
    PreparedModelEdit                 state;
    std::array<DynamicPrintConfig, 3> presets; // process, filament, printer
};

class LivePreview
{
public:
    LivePreview(PreviewCheckpoint before, std::string state_id);
    void                     verify(const std::string& state_id) const;
    void                     record(const std::string& state_id);
    void                     accept(const std::string& state_id);
    const PreviewCheckpoint& before(const std::string& state_id) const;
    bool                     pending() const { return m_pending; }
    size_t                   steps() const { return m_steps; }

private:
    PreviewCheckpoint m_before;
    std::string       m_state_id;
    size_t            m_steps   = 0;
    bool              m_pending = true;
};

// Work on copies. The caller rechecks context, snapshots, then commits on the UI
// thread. Font paths and the active plate's world-space surface centre are
// supplied by the application, never guessed by the model. The plate centre is
// required when creating a new object without an explicit position.
PreparedModelEdit prepare_model_edit(const Model&                    model,
                                     const MixedFilamentManager&     mixes,
                                     const std::vector<std::string>& colors,
                                     const ModelOperation&           operation,
                                     const std::string&              resource_directory,
                                     const std::optional<Vec3d>&     new_object_plate_center = std::nullopt);

} // namespace Slic3r::AIPrintAssistant
#endif
