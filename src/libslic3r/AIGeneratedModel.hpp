#pragma once

#include "AIModelTools.hpp"
#include <cstdint>

namespace Slic3r::AIPrintAssistant {

// generate_model: the assistant writes OpenSCAD code, the application runs it in
// a separate OpenSCAD process and imports the mesh. The OpenSCAD language cannot
// run programs, use the network or write files. It reads files only through
// import, include, use, surface and the legacy file= and dxf_* forms, which
// check_openscad_code rejects.
struct GenerateModelRequest
{
    std::string name, code, object_id;
};
inline constexpr size_t MAX_OPENSCAD_CODE_BYTES = 64 * 1024;
inline constexpr size_t MAX_GENERATED_TRIANGLES = 1000000;

nlohmann::json       generate_model_schema();
GenerateModelRequest parse_generate_model(const nlohmann::json& arguments);
void                 check_openscad_code(const std::string& code);
// OpenSCAD's errors, warnings and echo output for the assistant, with the
// temporary folder removed from file names.
std::string openscad_messages(const std::string& log, const std::string& directory);

// A new object on the active plate, or the generated part volume_id of
// request.object_id rebuilt in place. A rebuilt part keeps its transforms,
// filament and the object's other parts; its painting no longer matches the
// mesh and is cleared. Both are centred in XY with their bottom on the bed.
PreparedModelEdit prepare_generated_model(const Model&                model,
                                          TriangleMesh                mesh,
                                          const GenerateModelRequest& request,
                                          const std::string&          volume_id,
                                          const std::optional<Vec3d>& plate_center);

std::uint64_t mesh_fingerprint(const indexed_triangle_set& mesh);

// OpenSCAD code of parts made by generate_model. A lookup matches the part's
// current mesh, so undo, redo and manual mesh edits never return code that
// describes different geometry.
class GeneratedModels
{
public:
    struct Source
    {
        std::string name, code;
    };
    void          record(const ModelVolume& volume, std::string name, std::string code);
    const Source* find(const ModelVolume& volume) const;

private:
    struct Entry
    {
        size_t        volume_id;
        std::uint64_t mesh;
        Source        source;
    };
    std::vector<Entry> m_entries;
};

} // namespace Slic3r::AIPrintAssistant
