#ifndef slic3r_GUI_AIModelTools_hpp_
#define slic3r_GUI_AIModelTools_hpp_

#include "libslic3r/AIModelTools.hpp"

namespace Slic3r::GUI {
class Plater;
nlohmann::json                      ai_plate_context(Plater& plater);
nlohmann::json                      ai_slice_status(Plater& plater);
nlohmann::json                      ai_read_slice_tool(Plater& plater, const std::string& name, const nlohmann::json& arguments);
AIPrintAssistant::ModelToolContext  ai_model_context(Plater& plater, bool include_selection = true);
nlohmann::json                      ai_model_tools(Plater& plater);
AIPrintAssistant::PreparedModelEdit ai_prepare_model_edit(Plater& plater, const AIPrintAssistant::ModelOperation& operation);
void           ai_commit_model_edit(Plater& plater, AIPrintAssistant::PreparedModelEdit&& edit, const std::string& snapshot_name);
nlohmann::json ai_run_native_model_tool(Plater& plater, const AIPrintAssistant::ModelOperation& operation);
} // namespace Slic3r::GUI
#endif
