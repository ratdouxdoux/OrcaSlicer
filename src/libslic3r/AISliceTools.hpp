#ifndef slic3r_AISliceTools_hpp_
#define slic3r_AISliceTools_hpp_

#include <cstddef>
#include <iosfwd>
#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r {
struct GCodeProcessorResult;
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

} // namespace AIPrintAssistant
} // namespace Slic3r
#endif
