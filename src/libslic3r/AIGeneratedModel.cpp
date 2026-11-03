#include "AIGeneratedModel.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>

namespace Slic3r::AIPrintAssistant {
namespace {
using json = nlohmann::json;

bool identifier_start(char ch) { return std::isalpha((unsigned char) ch) || ch == '_' || ch == '$'; }
bool identifier_char(char ch) { return std::isalnum((unsigned char) ch) || ch == '_' || ch == '$'; }

std::string one_line(const std::string& text)
{
    std::string result;
    for (char ch : text)
        result += (unsigned char) ch < 0x20 ? ' ' : ch;
    result.erase(0, result.find_first_not_of(' '));
    result.erase(result.find_last_not_of(' ') + 1);
    return result;
}

size_t characters(const std::string& text)
{
    return size_t(std::count_if(text.begin(), text.end(), [](char ch) { return (ch & 0xC0) != 0x80; }));
}

ModelObject& find_object(Model& model, const std::string& object_id)
{
    for (auto* object : model.objects)
        if (std::to_string(object->id().id) == object_id)
            return *object;
    throw std::runtime_error("The object no longer exists. Read get_model_context again.");
}

void replace_all(std::string& text, const std::string& from, const std::string& to)
{
    for (size_t at = 0; !from.empty() && (at = text.find(from, at)) != std::string::npos; at += to.size())
        text.replace(at, from.size(), to);
}
} // namespace

json generate_model_schema()
{
    return {{"type", "object"},
            {"properties",
             {{"name", {{"type", "string"}, {"maxLength", 64}, {"description", "Short object name shown in the object list."}}},
              {"code",
               {{"type", "string"},
                {"maxLength", int(MAX_OPENSCAD_CODE_BYTES)},
                {"description", "Complete OpenSCAD program in millimetres with +Z up that produces one 3D solid. Define every module "
                                "and function inline."}}},
              {"object_id",
               {{"type", "string"},
                {"maxLength", 32},
                {"description", "Empty for a new object, or the ID of an object made by generate_model to rebuild it in place."}}}}},
            {"required", {"name", "code", "object_id"}},
            {"additionalProperties", false}};
}

GenerateModelRequest parse_generate_model(const json& arguments)
{
    if (!arguments.is_object())
        throw std::runtime_error("Tool arguments must be an object.");
    for (auto it = arguments.begin(); it != arguments.end(); ++it)
        if (it.key() != "name" && it.key() != "code" && it.key() != "object_id")
            throw std::runtime_error("Unexpected generate_model argument.");
    for (const char* key : {"name", "code", "object_id"})
        if (!arguments.contains(key) || !arguments[key].is_string())
            throw std::runtime_error(std::string("generate_model requires ") + key + " as a string.");
    GenerateModelRequest request{one_line(arguments["name"].get<std::string>()), arguments["code"].get<std::string>(),
                                 arguments["object_id"].get<std::string>()};
    if (request.name.empty() || characters(request.name) > 64)
        throw std::runtime_error("The generate_model name must be 1 to 64 characters.");
    if (request.object_id.size() > 20 ||
        !std::all_of(request.object_id.begin(), request.object_id.end(), [](char ch) { return std::isdigit((unsigned char) ch); }))
        throw std::runtime_error("object_id must be empty or an object ID from get_model_context.");
    check_openscad_code(request.code);
    return request;
}

void check_openscad_code(const std::string& code)
{
    if (code.empty() || code.size() > MAX_OPENSCAD_CODE_BYTES)
        throw std::runtime_error("The OpenSCAD code must be 1 to 65536 bytes.");
    if (code.find('\0') != std::string::npos)
        throw std::runtime_error("The OpenSCAD code contains a NUL character.");
    // Words that read files. Legacy linear_extrude and rotate_extrude take file=.
    static const std::set<std::string> blocked = {"import",     "import_dxf", "import_off", "import_stl",         "include",
                                                  "use",        "surface",    "file",       "dxf_linear_extrude", "dxf_rotate_extrude",
                                                  "dxf_dim",    "dxf_cross"};
    // Comments and string literals are skipped the way OpenSCAD's lexer skips them.
    for (size_t i = 0; i < code.size();) {
        const char ch = code[i];
        if (ch == '/' && i + 1 < code.size() && code[i + 1] == '/') {
            i = code.find('\n', i);
            if (i == std::string::npos)
                break;
        } else if (ch == '/' && i + 1 < code.size() && code[i + 1] == '*') {
            // OpenSCAD reports an unterminated comment itself.
            const auto end = code.find("*/", i + 2);
            i              = end == std::string::npos ? code.size() : end + 2;
        } else if (ch == '"') {
            for (++i; i < code.size() && code[i] != '"'; i += code[i] == '\\' ? 2 : 1)
                ;
            ++i;
        } else if (identifier_start(ch)) {
            size_t end = i;
            while (end < code.size() && identifier_char(code[end]))
                ++end;
            const auto word = code.substr(i, end - i);
            if (blocked.count(word))
                throw std::runtime_error("The OpenSCAD code uses '" + word +
                                         "', which reads files. Write every shape, module and function inline instead of importing "
                                         "or including files.");
            i = end;
        } else
            ++i;
    }
}

std::string openscad_messages(const std::string& log, const std::string& directory)
{
    std::string text = log;
    // OpenSCAD prints file names with forward slashes on every platform.
    auto folder = directory;
    std::replace(folder.begin(), folder.end(), '\\', '/');
    while (!folder.empty() && folder.back() == '/')
        folder.pop_back();
    if (!folder.empty()) {
        replace_all(text, folder + "/", "");
        auto native = folder;
        std::replace(native.begin(), native.end(), '/', '\\');
        replace_all(text, native + "\\", "");
    }
    std::istringstream       lines(text);
    std::vector<std::string> kept;
    size_t                   bytes = 0, omitted = 0;
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line.rfind("Fontconfig", 0) == 0)
            continue;
        if (kept.size() >= 60 || bytes + line.size() > 6000) {
            ++omitted;
            continue;
        }
        bytes += line.size() + 1;
        kept.push_back(std::move(line));
    }
    std::string result;
    for (const auto& line : kept)
        result += line + "\n";
    if (omitted)
        result += "... " + std::to_string(omitted) + " more lines\n";
    return result;
}

PreparedModelEdit prepare_generated_model(const Model&                original,
                                          TriangleMesh                mesh,
                                          const GenerateModelRequest& request,
                                          const std::string&          volume_id,
                                          const std::optional<Vec3d>& plate_center)
{
    if (mesh.empty())
        throw std::runtime_error("OpenSCAD produced no geometry.");
    if (mesh.facets_count() > MAX_GENERATED_TRIANGLES)
        throw std::runtime_error("The model has " + std::to_string(mesh.facets_count()) +
                                 " triangles, more than 1,000,000. Lower $fn or simplify the design.");
    const auto box = mesh.bounding_box();
    if (!box.size().allFinite() || box.size().maxCoeff() > 2000.)
        throw std::runtime_error("The model is larger than 2 metres. OpenSCAD units are millimetres.");
    mesh.translate(-float(box.center().x()), -float(box.center().y()), -float(box.min.z()));
    PreparedModelEdit edit;
    edit.model         = original;
    edit.model_changed = true;
    edit.result        = {{"operation", "generate_model"}};
    ModelObject* object;
    ModelVolume* volume = nullptr;
    if (request.object_id.empty()) {
        if (!plate_center || !plate_center->allFinite())
            throw std::runtime_error("Select a build plate before generating a model.");
        object       = edit.model.add_object();
        object->name = request.name;
        volume       = object->add_volume(std::move(mesh), ModelVolumeType::MODEL_PART, false);
        object->add_instance()->set_offset(Vec3d(plate_center->x(), plate_center->y(), 0.));
    } else {
        object = &find_object(edit.model, request.object_id);
        for (auto* candidate : object->volumes)
            if (std::to_string(candidate->id().id) == volume_id)
                volume = candidate;
        if (!volume)
            throw std::runtime_error("The generated part no longer exists. Read get_model_context again.");
        volume->set_mesh(std::move(mesh));
        // A new ID makes the scene rebuild the part. Painting described the old mesh.
        volume->set_new_unique_id();
        volume->supported_facets.reset();
        volume->seam_facets.reset();
        volume->mmu_segmentation_facets.reset();
        volume->fuzzy_skin_facets.reset();
    }
    volume->name = request.name;
    volume->calculate_convex_hull();
    object->invalidate_bounding_box();
    object->ensure_on_bed();
    edit.result["object_id"] = std::to_string(object->id().id);
    edit.result["volume_id"] = std::to_string(volume->id().id);
    return edit;
}

std::uint64_t mesh_fingerprint(const indexed_triangle_set& mesh)
{
    // FNV-1a over the raw vertex and index data.
    std::uint64_t hash = 14695981039346656037ull;
    auto          feed = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * 1099511628211ull;
    };
    feed(mesh.vertices.data(), mesh.vertices.size() * sizeof(stl_vertex));
    feed(mesh.indices.data(), mesh.indices.size() * sizeof(stl_triangle_vertex_indices));
    return hash;
}

void GeneratedModels::record(const ModelVolume& volume, std::string name, std::string code)
{
    m_entries.push_back({volume.id().id, mesh_fingerprint(volume.mesh().its), {std::move(name), std::move(code)}});
}

const GeneratedModels::Source* GeneratedModels::find(const ModelVolume& volume) const
{
    std::optional<std::uint64_t> mesh;
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
        if (it->volume_id != volume.id().id)
            continue;
        if (!mesh)
            mesh = mesh_fingerprint(volume.mesh().its);
        if (it->mesh == *mesh)
            return &it->source;
    }
    return nullptr;
}

} // namespace Slic3r::AIPrintAssistant
