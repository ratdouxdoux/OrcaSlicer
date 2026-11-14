#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r {
struct GCodeProcessorResult;
struct TriangleMeshStats;
class Print;
namespace AIPrintAssistant {

nlohmann::json slice_query_schema();
nlohmann::json gcode_query_schema();
void           validate_slice_query(const nlohmann::json& arguments);
void           validate_gcode_query(const nlohmann::json& arguments);

// Call only on a completed, current result. The GUI owns freshness/lifecycle checks.
std::string    slice_identity(const GCodeProcessorResult& result, int plate_index);
nlohmann::json summarize_slice(const GCodeProcessorResult& result, int plate_index, const nlohmann::json& arguments);
// Reads only the indexed window of the application's slice, never a model-supplied path.
nlohmann::json read_slice_gcode(std::istream& file, const GCodeProcessorResult& result, int plate_index, const nlohmann::json& arguments);

// Native checks as compact issue lists. Each issue has a code, a severity
// (error, serious, warning or info) and a message.
// Results of the admesh repair on import; empty for a clean mesh.
nlohmann::json mesh_issues(const TriangleMeshStats& stats);
// Current warnings of the print and object steps, attributed to objects where possible.
nlohmann::json print_warnings(const Print& print, size_t limit = 50);
// Checks of the processed G-code: paths outside the plate, conflicts, bed mismatch and processor warnings.
nlohmann::json gcode_issues(const GCodeProcessorResult& result, size_t limit = 50);

} // namespace AIPrintAssistant
} // namespace Slic3r
