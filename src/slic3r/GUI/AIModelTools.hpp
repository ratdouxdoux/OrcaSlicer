#ifndef slic3r_GUI_AIModelTools_hpp_
#define slic3r_GUI_AIModelTools_hpp_

#include "libslic3r/AIModelTools.hpp"

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
} // namespace Slic3r::GUI
#endif
