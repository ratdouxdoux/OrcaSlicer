#include "AIModelTools.hpp"
#include "AIPrintAssistant.hpp"
#include "Emboss.hpp"
#include "CutUtils.hpp"
#include "MeshBoolean.hpp"
#include "QuadricEdgeCollapse.hpp"
#include <openssl/sha.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <boost/nowide/convert.hpp>
#include <cereal/types/vector.hpp>

namespace Slic3r::AIPrintAssistant {
namespace {
using json = nlohmann::json;
json str(const char* description = "") { return {{"type", "string"}, {"description", description}}; }
json number(double min, double max) { return {{"type", "number"}, {"minimum", min}, {"maximum", max}}; }
json enumeration(std::initializer_list<const char*> values) { return {{"type", "string"}, {"enum", values}}; }
json vector_schema(double min, double max, int size = 3)
{
    return {{"type", "array"}, {"items", number(min, max)}, {"minItems", size}, {"maxItems", size}};
}
json object_schema(json properties, json required = json::array())
{
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}};
}
json        vec(const Vec3d& v) { return json::array({v.x(), v.y(), v.z()}); }
std::string id(ObjectID value) { return std::to_string(value.id); }

void validate(const json& value, const json& schema, const std::string& path)
{
    const auto type  = schema.value("type", "");
    bool       valid = type == "object"  ? value.is_object() :
                       type == "array"   ? value.is_array() :
                       type == "string"  ? value.is_string() :
                       type == "boolean" ? value.is_boolean() :
                       type == "integer" ? value.is_number_integer() :
                                           value.is_number();
    if (!valid)
        throw std::runtime_error("Invalid type for " + path + ".");
    if (schema.contains("enum") && std::find(schema["enum"].begin(), schema["enum"].end(), value) == schema["enum"].end())
        throw std::runtime_error("Unsupported value for " + path + ".");
    if (value.is_number()) {
        const double n = value.get<double>();
        if (!std::isfinite(n) || (schema.contains("minimum") && n < schema["minimum"].get<double>()) ||
            (schema.contains("maximum") && n > schema["maximum"].get<double>()))
            throw std::runtime_error("Out-of-range value for " + path + ".");
    } else if (value.is_string()) {
        if (value.get_ref<const std::string&>().size() > 2048)
            throw std::runtime_error("Text is too long for " + path + ".");
    } else if (value.is_array()) {
        if (value.size() < schema.value("minItems", size_t(0)) || value.size() > schema.value("maxItems", size_t(64)))
            throw std::runtime_error("Invalid array length for " + path + ".");
        for (const auto& element : value)
            validate(element, schema.at("items"), path);
    } else if (value.is_object()) {
        for (const auto& key : schema.at("required"))
            if (!value.contains(key.get<std::string>()))
                throw std::runtime_error("Missing " + path + "." + key.get<std::string>() + ".");
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!schema.at("properties").contains(it.key()))
                throw std::runtime_error("Unknown argument: " + path + "." + it.key());
            validate(it.value(), schema["properties"][it.key()], path + "." + it.key());
        }
    }
}

Vec3d vector_value(const json& p, const char* key, const Vec3d& fallback = Vec3d::Zero())
{
    if (!p.contains(key))
        return fallback;
    return Vec3d(p[key][0].get<double>(), p[key][1].get<double>(), p[key][2].get<double>());
}
Vec3d face_normal(const std::string& face)
{
    if (face == "top")
        return Vec3d::UnitZ();
    if (face == "bottom")
        return -Vec3d::UnitZ();
    if (face == "right")
        return Vec3d::UnitX();
    if (face == "left")
        return -Vec3d::UnitX();
    if (face == "back")
        return Vec3d::UnitY();
    if (face == "front")
        return -Vec3d::UnitY();
    throw std::runtime_error("Choose top, bottom, front, back, left or right.");
}
ModelObject& find_object(Model& model, const std::string& object_id)
{
    for (auto* object : model.objects)
        if (id(object->id()) == object_id)
            return *object;
    throw std::runtime_error("The target object no longer exists. Read get_model_context again.");
}
ModelVolume& find_volume(ModelObject& object, const std::string& volume_id)
{
    for (auto* volume : object.volumes)
        if (id(volume->id()) == volume_id || (volume_id.empty() && object.volumes.size() == 1))
            return *volume;
    throw std::runtime_error("Specify a volume_id from get_model_context for this part.");
}
ModelInstance& instance(ModelObject& object, const json& parameters)
{
    const auto wanted = parameters.value("instance_id", "");
    for (auto* item : object.instances)
        if (id(item->id()) == wanted || (wanted.empty() && object.instances.size() == 1))
            return *item;
    throw std::runtime_error("Specify the instance_id when an object has several instances.");
}
void replace_mesh(ModelVolume& volume, TriangleMesh mesh)
{
    if (mesh.empty())
        throw std::runtime_error("This operation produced empty geometry.");
    volume.set_mesh(std::move(mesh));
    volume.set_new_unique_id();
    volume.supported_facets.reset();
    volume.seam_facets.reset();
    volume.mmu_segmentation_facets.reset();
    volume.fuzzy_skin_facets.reset();
    volume.text_configuration.reset();
    volume.emboss_shape.reset();
    volume.get_object()->invalidate_bounding_box();
}
ModelVolumeType volume_type(const std::string& value)
{
    if (value == "negative")
        return ModelVolumeType::NEGATIVE_VOLUME;
    if (value == "modifier")
        return ModelVolumeType::PARAMETER_MODIFIER;
    if (value == "support_blocker")
        return ModelVolumeType::SUPPORT_BLOCKER;
    if (value == "support_enforcer")
        return ModelVolumeType::SUPPORT_ENFORCER;
    return ModelVolumeType::MODEL_PART;
}
} // namespace

json model_tool_catalog()
{
    json catalog = json::array();
    auto add     = [&](const char* name, const char* description, json properties, json required = json::array()) {
        catalog.push_back({{"operation", name}, {"description", description}, {"parameters", object_schema(properties, required)}});
    };
    const auto face      = enumeration({"top", "bottom", "front", "back", "left", "right"});
    const auto part_type = enumeration({"part", "negative", "modifier", "support_blocker", "support_enforcer"});
    const json boolean   = {{"type", "boolean"}};
    add("transform",
        "Move, rotate, scale, resize or mirror an object instance or a part. Units mm/degrees; rotation and scaling use the target centre.",
        {{"mode", enumeration({"move", "rotate", "scale", "size", "mirror"})},
         {"value", vector_schema(-10000, 10000)},
         {"instance_id", str()},
         {"drop_to_bed", boolean}},
        {"mode", "value"});
    add("drop_to_bed", "Place the object's instances on the bed.", json::object());
    add("add_primitive",
        "Create a new object with empty object_id, or add a part/negative/modifier to an existing object_id. Size is XYZ in mm. "
        "Omit position_mm to centre a new object on the active plate with its bottom on the bed; an added part defaults to its parent "
        "object's centre. Only supply position_mm for an explicitly requested position; it is the shape's centre in world coordinates.",
        {{"shape", enumeration({"cube", "cylinder", "sphere", "cone", "torus", "wedge"})},
         {"size_mm", vector_schema(0.1, 2000)},
         {"position_mm", vector_schema(-10000, 10000)},
         {"type", part_type},
         {"name", str()},
         {"instance_id", str()}},
        {"shape", "size_mm"});
    const json text_properties = {{"text", str("Literal text to create, up to 256 UTF-8 bytes.")},
                                  {"face", face},
                                  {"height_mm", number(0.5, 200)},
                                  {"depth_mm", number(0.1, 50)},
                                  {"bold", boolean},
                                  {"mode", enumeration({"raised", "engraved"})},
                                  {"offset_mm", vector_schema(-2000, 2000, 2)},
                                  {"rotation_degrees", number(-360, 360)},
                                  {"instance_id", str()}};
    add("add_text",
        "Add editable native embossed/engraved text, centred on a flat axis-facing surface. For a cube's middle use face=top and "
        "offset_mm=[0,0]. Curved surface projection is available in the text editor.",
        text_properties, {"text", "face", "height_mm", "depth_mm"});
    add("edit_text", "Replace an existing native text part and place it on the specified face.", text_properties,
        {"text", "face", "height_mm", "depth_mm"});
    add("duplicate", "Duplicate an object with unique IDs and a world-space offset for each copy.",
        {{"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 20}}}, {"offset_mm", vector_schema(-2000, 2000)}},
        {"count", "offset_mm"});
    add("delete", "Delete object_id, or only volume_id when supplied. This is undoable.", json::object());
    add("rename", "Rename an object or part.", {{"name", str()}}, {"name"});
    add("set_part_type", "Convert a part to a model part, negative, modifier, support blocker or enforcer.", {{"type", part_type}},
        {"type"});
    add("assign_filament", "Assign a physical or mixed filament ID to an object or part.",
        {{"filament_id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}}}, {"filament_id"});
    add("paint_surface",
        "Paint whole mesh facets on a face or within a world-space box: color, support, seam or fuzzy skin. Existing painting elsewhere is "
        "preserved. Box selection uses triangle centres; use the native editor for precise brush strokes.",
        {{"channel", enumeration({"color", "support", "seam", "fuzzy_skin"})},
         {"state", enumeration({"paint", "enforce", "block", "erase"})},
         {"face", enumeration({"all", "top", "bottom", "front", "back", "left", "right"})},
         {"angle_degrees", number(0, 90)},
         {"box_min_mm", vector_schema(-10000, 10000)},
         {"box_max_mm", vector_schema(-10000, 10000)},
         {"filament_id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}},
         {"instance_id", str()}},
        {"channel", "state", "face"});
    add("split_parts", "Split a disconnected volume into parts, preserving its transform.", json::object());
    add("boolean",
        "Union, subtract or intersect two parts of the same object; consumes the second part. Replaces meshes and clears painting/text "
        "metadata on the result.",
        {{"other_volume_id", str()}, {"mode", enumeration({"union", "difference", "intersection"})}}, {"other_volume_id", "mode"});
    add("simplify", "Reduce a part's triangle count using the native quadric algorithm. Clears painting/text metadata on that part.",
        {{"reduction_percent", number(1, 95)}}, {"reduction_percent"});
    add("cut", "Cut an object with a world-space plane. The native cut editor additionally supports connectors and dovetails.",
        {{"position_mm", vector_schema(-10000, 10000)},
         {"normal", vector_schema(-1, 1)},
         {"keep", enumeration({"both", "upper", "lower"})},
         {"instance_id", str()}},
        {"position_mm", "normal", "keep"});
    add("create_mix",
        "Create a native mixed filament. Components are 1-based physical IDs; percentages sum to 100. Optionally assign it to "
        "object_id/volume_id. Gradient endpoints are first-component percentages across object height. Simple is the default for a pair; "
        "layer_cycle is the default for three or more components. Pattern tokens 1/2 refer to the first two components, higher tokens to "
        "physical filament IDs. Mixing is a print recipe, not merely a display color.",
        {{"components",
          {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}}, {"minItems", 2}, {"maxItems", 16}}},
         {"percentages",
          {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}, {"maximum", 100}}}, {"minItems", 2}, {"maxItems", 16}}},
         {"distribution", enumeration({"layer_cycle", "simple"})},
         {"pattern", str()},
         {"gradient", boolean},
         {"gradient_start_percent", number(0, 100)},
         {"gradient_end_percent", number(0, 100)}},
        {"components", "percentages"});
    add("match_color",
        "Find a native color-mixing recipe for a target #RRGGBB, then create it. Preview color is approximate. Optionally assign to "
        "object_id/volume_id.",
        {{"color", str()},
         {"min_component_percent", {{"type", "integer"}, {"minimum", 0}, {"maximum", 50}}},
         {"max_component_percent", {{"type", "integer"}, {"minimum", 50}, {"maximum", 100}}}},
        {"color"});
    add("open_editor", "Open any native model editor for interactive completion. Opening an editor does not mean an edit was applied.",
        {{"editor",
          enumeration({"move", "rotate", "scale", "lay_flat", "cut", "boolean", "supports", "seam", "fuzzy_skin", "color_paint", "text",
                       "svg", "measure", "assembly", "simplify", "brim_ears", "variable_layers", "color_mixing", "repair"})},
         {"instance_id", str()}},
        {"editor"});
    add("native_action",
        "Run a native selection action. Arrange/orient run as native background jobs; split/merge preserve native dialogs and validation.",
        {{"action", enumeration({"arrange", "orient", "split_objects", "merge_objects", "split_instances", "fill_bed", "fit_to_bed"})}},
        {"action"});
    return catalog;
}

json model_operation_schema()
{
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties",
             {{"context_id", str()},
              {"operation", str("An operation from list_model_tools.")},
              {"object_id", str("Object ID, or empty for a new object/mix/native selection action.")},
              {"volume_id", str("Part ID, or empty for an object operation.")},
              {"parameters", {{"type", "object"}}}}},
            {"required", {"context_id", "operation", "object_id", "volume_id", "parameters"}}};
}

ModelOperation parse_model_operation(const json& arguments)
{
    if (!arguments.is_object() || arguments.size() != 5)
        throw std::runtime_error("A model operation needs context_id, operation, object_id, volume_id and parameters.");
    for (const auto* key : {"context_id", "operation", "object_id", "volume_id"})
        if (!arguments.contains(key) || !arguments[key].is_string() || arguments[key].get_ref<const std::string&>().size() > 128)
            throw std::runtime_error("Invalid model operation identity.");
    ModelOperation result{arguments["context_id"], arguments["operation"], arguments["object_id"], arguments["volume_id"],
                          arguments.at("parameters")};
    for (const auto& entry : model_tool_catalog()) {
        if (entry["operation"] == result.operation) {
            validate(result.parameters, entry["parameters"], "parameters");
            return result;
        }
    }
    throw std::runtime_error("Unknown model operation. Use list_model_tools.");
}

ModelToolContext capture_model_tool_context(const Model&                    model,
                                            const std::vector<std::string>& selected,
                                            const std::vector<std::string>& colors,
                                            const MixedFilamentManager&     mixes,
                                            const std::string&              project_state)
{
    SHA256_CTX hash;
    SHA256_Init(&hash);
    auto hash_text = [&](const std::string& text) {
        const auto size = text.size();
        SHA256_Update(&hash, &size, sizeof(size));
        SHA256_Update(&hash, text.data(), text.size());
    };
    json objects = json::array();
    for (const auto* object : model.objects) {
        json volumes = json::array(), instances = json::array();
        for (const auto& key : object->config.keys()) {
            hash_text(key);
            hash_text(object->config.get().opt_serialize(key));
        }
        hash_text(std::to_string(object->layer_height_profile.timestamp()));
        hash_text(std::to_string(static_cast<const ModelConfig&>(object->config).timestamp()));
        for (const auto& [range, config] : object->layer_config_ranges) {
            hash_text(json::array({range.first, range.second}).dump());
            hash_text(std::to_string(config.timestamp()));
            for (const auto& key : config.keys()) {
                hash_text(key);
                hash_text(config.get().opt_serialize(key));
            }
        }
        std::ostringstream metadata;
        {
            cereal::BinaryOutputArchive archive(metadata);
            archive(object->printable, object->origin_translation, object->brim_points, object->cut_connectors, object->cut_id.id().id,
                    object->cut_id.check_sum(), object->cut_id.connectors_cnt(), object->sla_support_points, object->sla_points_status,
                    object->sla_drain_holes);
        }
        hash_text(metadata.str());
        for (const auto* volume : object->volumes) {
            hash_text(std::to_string(static_cast<const ModelConfig&>(volume->config).timestamp()));
            const auto& mesh = volume->mesh().its;
            SHA256_Update(&hash, mesh.vertices.data(), mesh.vertices.size() * sizeof(stl_vertex));
            SHA256_Update(&hash, mesh.indices.data(), mesh.indices.size() * sizeof(stl_triangle_vertex_indices));
            for (const auto& key : volume->config.keys()) {
                hash_text(key);
                hash_text(volume->config.get().opt_serialize(key));
            }
            const auto matrix = volume->get_matrix();
            SHA256_Update(&hash, matrix.data(), sizeof(double) * 16);
            std::ostringstream part_metadata;
            {
                cereal::BinaryOutputArchive archive(part_metadata);
                archive(volume->text_configuration, volume->emboss_shape, volume->cut_info);
            }
            hash_text(part_metadata.str());
            hash_text(volume->material_id());
            hash_text(std::to_string(volume->supported_facets.timestamp()) + ":" + std::to_string(volume->seam_facets.timestamp()) + ":" +
                      std::to_string(volume->mmu_segmentation_facets.timestamp()) + ":" +
                      std::to_string(volume->fuzzy_skin_facets.timestamp()));
            json item = {{"id", id(volume->id())},
                         {"name", volume->name},
                         {"type", int(volume->type())},
                         {"triangles", mesh.indices.size()},
                         {"filament_id", volume->extruder_id()},
                         {"painted", volume->is_mm_painted()},
                         {"editable_text", volume->text_configuration.has_value()}};
            if (volume->text_configuration)
                item["text"] = volume->text_configuration->text;
            volumes.push_back(std::move(item));
        }
        for (const auto* inst : object->instances) {
            const auto matrix = inst->get_matrix();
            SHA256_Update(&hash, matrix.data(), sizeof(double) * 16);
            instances.push_back({{"id", id(inst->id())}, {"position_mm", vec(inst->get_offset())}, {"printable", inst->printable}});
        }
        const auto box = object->bounding_box_exact();
        objects.push_back({{"id", id(object->id())},
                           {"name", object->name},
                           {"bounds_min_mm", vec(box.min)},
                           {"bounds_max_mm", vec(box.max)},
                           {"size_mm", vec(box.size())},
                           {"volumes", volumes},
                           {"instances", instances}});
    }
    json filament_data = json::array();
    for (size_t i = 0; i < colors.size(); ++i)
        filament_data.push_back({{"id", i + 1}, {"color", colors[i]}, {"kind", "physical"}});
    auto mix_copy = mixes;
    hash_text(mix_copy.serialize_custom_entries());
    for (size_t filament = colors.size() + 1; filament <= mixes.total_filaments(colors.size()); ++filament) {
        const auto* mix = mixes.mixed_filament_from_id(unsigned(filament), colors.size());
        if (mix)
            filament_data.push_back({{"id", filament},
                                     {"color", mix->display_color},
                                     {"kind", "mixed"},
                                     {"component_a", mix->component_a},
                                     {"component_b", mix->component_b},
                                     {"mix_b_percent", mix->mix_b_percent},
                                     {"component_ids", mix->gradient_component_ids},
                                     {"component_percentages", mix->gradient_component_weights},
                                     {"pattern", mix->manual_pattern},
                                     {"gradient", mix->gradient_enabled}});
    }
    ModelToolContext result;
    result.data = {{"objects", objects},
                   {"selected_object_ids", selected},
                   {"filaments", filament_data},
                   {"coordinates", "World millimetres. Top=+Z, front=-Y, right=+X. Model edits require a reviewed proposal."}};
    hash_text(result.data.dump());
    hash_text(project_state);
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &hash);
    std::ostringstream encoded;
    for (unsigned char c : digest)
        encoded << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    result.id                 = encoded.str();
    result.data["context_id"] = result.id;
    return result;
}

namespace {
void validate_filament(int filament, const std::vector<std::string>& colors, const MixedFilamentManager& mixes)
{
    if (filament < 1)
        throw std::runtime_error("Choose a filament from get_model_context.");
    if (size_t(filament) <= colors.size())
        return;
    if (!mixes.mixed_filament_from_id(unsigned(filament), colors.size()))
        throw std::runtime_error("That mixed filament is unavailable. Read get_model_context again.");
}
void assign_filament(ModelObject& object, const std::string& volume_id, int filament)
{
    auto& config = volume_id.empty() ? object.config : find_volume(object, volume_id).config;
    config.set_key_value("extruder", new ConfigOptionInt(filament));
}

// Ray-cast against actual geometry instead of placing text on an empty portion
// of a bounding box. A planar footprint is required for this automatic mode.
Vec3d surface_point(const TriangleMesh& body, const Vec3d& point, const Vec3d& normal)
{
    const auto  box       = body.bounding_box();
    const Vec3d origin    = point + normal * (box.size().norm() + 10.);
    const Vec3d direction = -normal;
    double      nearest   = std::numeric_limits<double>::max();
    Vec3d       hit = Vec3d::Zero(), hit_normal = Vec3d::Zero();
    for (const auto& triangle : body.its.indices) {
        const Vec3d  a   = body.its.vertices[triangle[0]].cast<double>();
        const Vec3d  e1  = body.its.vertices[triangle[1]].cast<double>() - a;
        const Vec3d  e2  = body.its.vertices[triangle[2]].cast<double>() - a;
        const Vec3d  h   = direction.cross(e2);
        const double det = e1.dot(h);
        if (std::abs(det) < 1e-10)
            continue;
        const Vec3d  s = origin - a;
        const double u = s.dot(h) / det;
        if (u < -1e-6 || u > 1. + 1e-6)
            continue;
        const Vec3d  q = s.cross(e1);
        const double v = direction.dot(q) / det;
        if (v < -1e-6 || u + v > 1. + 1e-6)
            continue;
        const double t = e2.dot(q) / det;
        if (t >= 0 && t < nearest) {
            nearest    = t;
            hit        = origin + t * direction;
            hit_normal = e1.cross(e2).normalized();
        }
    }
    if (nearest == std::numeric_limits<double>::max() || hit_normal.dot(normal) < 0.999)
        throw std::runtime_error(
            "Text needs a flat surface here. Choose another face/offset or use the native text editor for surface projection.");
    return hit;
}

ModelVolume& add_text(ModelObject& object, const ModelOperation& operation, const std::string& resources)
{
    const auto&       p    = operation.parameters;
    const std::string text = p.at("text");
    if (text.empty() || text.size() > 256 || text.find('\0') != std::string::npos)
        throw std::runtime_error("Use 1 to 256 UTF-8 bytes of text.");
    auto&        inst = instance(object, p);
    ModelVolume* old  = nullptr;
    if (operation.operation == "edit_text") {
        old = &find_volume(object, operation.volume_id);
        if (!old->text_configuration)
            throw std::runtime_error("The selected volume is not editable text.");
    }
    TriangleMesh body;
    for (const auto* volume : object.volumes) {
        if (!volume->is_model_part() || volume == old || volume->text_configuration)
            continue;
        auto mesh = volume->mesh();
        mesh.transform(inst.get_matrix() * volume->get_matrix(), true);
        body.merge(mesh);
    }
    if (body.empty())
        throw std::runtime_error("Select an object with a solid surface to add text.");
    const std::string         filename = p.value("bold", true) ? "HarmonyOS_Sans_SC_Bold.ttf" : "HarmonyOS_Sans_SC_Regular.ttf";
    Emboss::FontFileWithCache font(Emboss::create_font_file((resources + "/fonts/" + filename).c_str()));
    if (!font.font_file)
        throw std::runtime_error("The bundled text font could not be loaded.");
    TextConfiguration tc;
    tc.text                  = text;
    tc.style.name            = "Snorca Sans";
    tc.style.type            = EmbossStyle::Type::file_path;
    tc.style.path            = resources + "/fonts/" + filename;
    tc.style.prop            = FontProp{};
    tc.style.prop.face_name  = "HarmonyOS Sans SC";
    tc.style.prop.family     = "swiss";
    tc.style.prop.style      = "normal";
    tc.style.prop.weight     = p.value("bold", true) ? "bold" : "normal";
    tc.style.prop.per_glyph  = false;
    tc.style.prop.size_in_mm = p.at("height_mm").get<float>();
    EmbossShape shape;
    shape.scale            = Emboss::get_text_shape_scale(tc.style.prop, *font.font_file);
    shape.projection.depth = p.at("depth_mm");
    shape.shapes_with_ids  = Emboss::text2vshapes(font, boost::nowide::widen(text), tc.style.prop);
    shape.final_shape      = Emboss::text2shapes(font, text.c_str(), tc.style.prop);
    Emboss::ProjectTransform projection(std::make_unique<Emboss::ProjectZ>(shape.projection.depth / shape.scale),
                                        Transform3d(Eigen::Scaling(shape.scale)));
    TriangleMesh             mesh(Emboss::polygons2model(shape.final_shape.expolygons, projection));
    if (mesh.empty())
        throw std::runtime_error("The text produced no geometry. Check the characters and font.");
    const auto  text_box = mesh.bounding_box();
    const auto  normal   = face_normal(p.at("face"));
    Transform3d frame    = Emboss::create_transformation_onto_surface(body.bounding_box().center(), normal);
    frame.rotate(Eigen::AngleAxisd(p.value("rotation_degrees", 0.) * PI / 180., Vec3d::UnitZ()));
    Vec3d point = frame.translation();
    if (p.contains("offset_mm"))
        point += frame.linear().col(0) * p["offset_mm"][0].get<double>() + frame.linear().col(1) * p["offset_mm"][1].get<double>();
    point = surface_point(body, point, normal);
    for (double x : {-0.5, 0.5})
        for (double y : {-0.5, 0.5}) {
            const auto corner = point + frame.linear().col(0) * (x * text_box.size().x()) +
                                frame.linear().col(1) * (y * text_box.size().y());
            if (std::abs((surface_point(body, corner, normal) - point).dot(normal)) > 0.05)
                throw std::runtime_error("The text extends beyond this flat surface. Reduce its height or use the text editor.");
        }
    const bool engraved = p.value("mode", "raised") == "engraved";
    frame.translation() = point;
    frame.translate(Vec3d(-text_box.center().x(), -text_box.center().y(), engraved ? -shape.projection.depth + 0.05 : -0.05));
    ModelVolume* volume = old;
    if (volume)
        replace_mesh(*volume, std::move(mesh));
    else
        volume = object.add_volume(std::move(mesh), engraved ? ModelVolumeType::NEGATIVE_VOLUME : ModelVolumeType::MODEL_PART, false);
    volume->set_type(engraved ? ModelVolumeType::NEGATIVE_VOLUME : ModelVolumeType::MODEL_PART);
    volume->name = text;
    volume->set_transformation(inst.get_matrix().inverse() * frame);
    volume->text_configuration = std::move(tc);
    volume->emboss_shape       = std::move(shape);
    object.sort_volumes(true);
    object.invalidate_bounding_box();
    return *volume;
}
} // namespace

PreparedModelEdit prepare_model_edit(const Model&                    original,
                                     const MixedFilamentManager&     mixes,
                                     const std::vector<std::string>& colors,
                                     const ModelOperation&           operation,
                                     const std::string&              resources,
                                     const std::optional<Vec3d>&     new_object_plate_center)
{
    // The same strict validation applies to model proposals and offline callers.
    parse_model_operation({{"context_id", operation.context_id},
                           {"operation", operation.operation},
                           {"object_id", operation.object_id},
                           {"volume_id", operation.volume_id},
                           {"parameters", operation.parameters}});
    PreparedModelEdit edit;
    edit.model         = original;
    edit.mixes         = mixes;
    edit.result        = {{"operation", operation.operation}};
    const auto& p      = operation.parameters;
    const auto& action = operation.operation;
    if (action == "open_editor" || action == "native_action" || action == "match_color")
        throw std::runtime_error("This operation requires the native application adapter.");
    if (action == "create_mix") {
        const auto ids     = p.at("components").get<std::vector<unsigned int>>();
        const auto weights = p.at("percentages").get<std::vector<int>>();
        if (ids.size() != weights.size() || std::set<unsigned int>(ids.begin(), ids.end()).size() != ids.size() ||
            *std::max_element(ids.begin(), ids.end()) > colors.size() || std::accumulate(weights.begin(), weights.end(), 0) != 100)
            throw std::runtime_error("Choose distinct physical filaments and percentages totalling 100.");
        if (mixes.total_filaments(colors.size()) >= MAXIMUM_FILAMENT_NUMBER)
            throw std::runtime_error("The filament limit has been reached.");
        if (std::any_of(weights.begin(), weights.end(), [](int weight) { return weight == 0; }))
            throw std::runtime_error("Omit zero-percent components from the recipe.");
        const int pair_percent = int(std::lround(100. * weights[1] / (weights[0] + weights[1])));
        edit.mixes.add_custom_filament(ids[0], ids[1], pair_percent, colors);
        if (edit.mixes.mixed_filaments().size() <= mixes.mixed_filaments().size())
            throw std::runtime_error("The mixed filament could not be created.");
        auto& mix = edit.mixes.mixed_filaments().back();
        if (ids.size() > 2 || p.value("gradient", false))
            mix.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ids);
        for (size_t i = 0; !mix.gradient_component_ids.empty() && i < weights.size(); ++i) {
            if (i)
                mix.gradient_component_weights += "/";
            mix.gradient_component_weights += std::to_string(weights[i]);
        }
        const auto distribution = p.value("distribution", ids.size() > 2 ? "layer_cycle" : "simple");
        if (ids.size() > 2 && distribution == "simple")
            throw std::runtime_error("Use layer_cycle for recipes with three or more components.");
        mix.distribution_mode = distribution == "simple" ? MixedFilament::Simple : MixedFilament::LayerCycle;
        mix.manual_pattern    = p.value("pattern", "");
        if (!mix.manual_pattern.empty()) {
            const auto normalized = MixedFilamentManager::normalize_manual_pattern(mix.manual_pattern);
            if (normalized.empty())
                throw std::runtime_error("The mixing pattern is invalid.");
            mix.manual_pattern = normalized;
            const auto pattern = mixed_filament_manual_pattern_from_string(normalized, ids[0], ids[1], 99);
            if (!pattern)
                throw std::runtime_error("The mixing pattern contains unavailable filaments.");
            for (const auto& group : pattern->groups)
                for (const auto& ref : group)
                    if (std::find(ids.begin(), ids.end(), ref.id) == ids.end())
                        throw std::runtime_error("The pattern must use only the recipe's components.");
            auto pattern_row = mix;
            pattern_row.gradient_component_ids.clear();
            pattern_row.gradient_component_weights.clear();
            const auto actual = mixed_filament_definition_from_legacy_row(pattern_row, colors.size());
            for (size_t i = 0; i < ids.size(); ++i) {
                const auto component = std::find_if(actual.recipe.blend.components.begin(), actual.recipe.blend.components.end(),
                                                    [&](const auto& item) { return item.filament.id == ids[i]; });
                if (component == actual.recipe.blend.components.end() || std::abs(component->percent - weights[i]) > 1)
                    throw std::runtime_error("The pattern proportions do not match the recipe percentages.");
            }
            if (p.value("gradient", false))
                throw std::runtime_error("Choose either a manual pattern or a gradient.");
        }
        mix.gradient_enabled = p.value("gradient", false);
        mix.gradient_start   = p.value("gradient_start_percent", 80.f) / 100.f;
        mix.gradient_end     = p.value("gradient_end_percent", 20.f) / 100.f;
        if (mix.gradient_enabled && std::abs(mix.gradient_end - mix.gradient_start) < MixedFilament::k_min_gradient_difference)
            throw std::runtime_error("Gradient endpoints must differ by at least five percentage points.");
        mix.ui_mode                = mix.gradient_enabled ? 3 : mix.manual_pattern.empty() ? 0 : 1;
        mix.custom                 = true;
        const int filament         = int(edit.mixes.total_filaments(colors.size()));
        edit.result["filament_id"] = filament;
        edit.mixes_changed         = true;
        if (!operation.object_id.empty()) {
            assign_filament(find_object(edit.model, operation.object_id), operation.volume_id, filament);
            edit.model_changed = true;
        }
        return edit;
    }
    if (action == "add_primitive") {
        if (operation.object_id.empty() && p.value("type", "part") != "part")
            throw std::runtime_error("Add negative volumes and modifiers to an existing solid object.");
        const Vec3d       size   = vector_value(p, "size_mm");
        const std::string shape  = p.at("shape");
        TriangleMesh      mesh   = shape == "sphere"   ? make_sphere(0.5) :
                                   shape == "cylinder" ? make_cylinder(0.5, 1.) :
                                   shape == "cone"     ? make_cone(0.5, 1.) :
                                   shape == "torus"    ? make_torus(0.35, 0.15) :
                                   shape == "wedge"    ? make_prism(1, 1, 1) :
                                                         make_cube(1, 1, 1);
        const auto        bounds = mesh.bounding_box();
        mesh.translate(-bounds.center().cast<float>());
        mesh.scale((size.cwiseQuotient(bounds.size())).cast<float>());
        ModelObject* object = operation.object_id.empty() ? edit.model.add_object() : &find_object(edit.model, operation.object_id);
        if (operation.object_id.empty()) {
            object->name = p.value("name", shape);
            object->add_instance();
        }
        auto& inst             = instance(*object, p);
        Vec3d default_position = Vec3d::Zero();
        if (!p.contains("position_mm")) {
            if (operation.object_id.empty()) {
                if (!new_object_plate_center || !new_object_plate_center->allFinite())
                    throw std::runtime_error("Select a build plate before creating an object without an explicit position.");
                default_position = *new_object_plate_center + Vec3d(0., 0., size.z() / 2.);
            } else
                default_position = object->bounding_box_exact().center();
        }
        const auto position = vector_value(p, "position_mm", default_position);
        auto*      volume   = object->add_volume(std::move(mesh), volume_type(p.value("type", "part")), false);
        volume->name        = p.value("name", shape);
        volume->set_transformation(inst.get_matrix().inverse() * Transform3d(Eigen::Translation3d(position)));
        object->sort_volumes(true);
        object->invalidate_bounding_box();
        edit.model_changed       = true;
        edit.result["object_id"] = id(object->id());
        edit.result["volume_id"] = id(volume->id());
        return edit;
    }
    auto& object             = find_object(edit.model, operation.object_id);
    edit.result["object_id"] = operation.object_id;
    size_t triangles         = 0;
    for (const auto* volume : object.volumes)
        triangles += volume->mesh().facets_count();
    if ((action == "boolean" || action == "simplify" || action == "cut" || action == "add_text" || action == "edit_text") &&
        triangles > 500000)
        throw std::runtime_error("Use the native editor for this operation on meshes over 500,000 triangles.");
    if (action == "add_text" || action == "edit_text") {
        edit.result["volume_id"] = id(add_text(object, operation, resources).id());
    } else if (action == "transform") {
        auto&        inst   = instance(object, p);
        ModelVolume* part   = operation.volume_id.empty() ? nullptr : &find_volume(object, operation.volume_id);
        const auto   world  = part ? Transform3d(inst.get_matrix() * part->get_matrix()) : inst.get_matrix();
        const auto   bounds = part ? part->mesh().transformed_bounding_box(world) :
                                     object.instance_bounding_box(size_t(std::find(object.instances.begin(), object.instances.end(), &inst) -
                                                                         object.instances.begin()));
        Vec3d        value  = vector_value(p, "value");
        const auto   mode   = p.at("mode").get<std::string>();
        Transform3d  delta  = Transform3d::Identity();
        if (mode == "move")
            delta.translate(value);
        else {
            delta.translate(bounds.center());
            if (mode == "rotate") {
                for (int axis = 0; axis < 3; ++axis)
                    delta.rotate(Eigen::AngleAxisd(value[axis] * PI / 180., Vec3d::Unit(axis)));
            } else {
                if (mode == "mirror") {
                    for (int axis = 0; axis < 3; ++axis)
                        if (value[axis] != 1. && value[axis] != -1.)
                            throw std::runtime_error("Mirror values must be 1 or -1 per axis.");
                } else {
                    if ((value.array() <= 0.).any())
                        throw std::runtime_error("Scale and size must be positive.");
                    if (mode == "size")
                        value = value.cwiseQuotient(bounds.size());
                    if ((value.array() > 1000.).any() || (value.array() < 0.001).any())
                        throw std::runtime_error("Scale is outside the supported range.");
                }
                delta.scale(value);
            }
            delta.translate(-bounds.center());
        }
        if (part)
            part->set_transformation(inst.get_matrix().inverse() * delta * world);
        else
            inst.set_transformation(Geometry::Transformation(delta * world));
        object.invalidate_bounding_box();
        if (p.value("drop_to_bed", false))
            object.ensure_on_bed();
    } else if (action == "drop_to_bed")
        object.ensure_on_bed();
    else if (action == "rename") {
        if (p.at("name").get<std::string>().empty())
            throw std::runtime_error("The name cannot be empty.");
        if (operation.volume_id.empty())
            object.name = p.at("name");
        else
            find_volume(object, operation.volume_id).name = p.at("name");
    } else if (action == "duplicate") {
        for (int i = 1; i <= p.at("count").get<int>(); ++i) {
            auto* copy = edit.model.add_object(object);
            for (auto* inst : copy->instances)
                inst->set_offset(inst->get_offset() + double(i) * vector_value(p, "offset_mm"));
            copy->invalidate_bounding_box();
        }
    } else if (action == "delete") {
        if (operation.volume_id.empty())
            edit.model.delete_object(&object);
        else {
            auto* volume = &find_volume(object, operation.volume_id);
            if (object.volumes.size() == 1 ||
                (volume->is_model_part() &&
                 std::count_if(object.volumes.begin(), object.volumes.end(), [](const auto* v) { return v->is_model_part(); }) == 1))
                throw std::runtime_error("Delete the whole object to remove its last solid part.");
            object.delete_volume(size_t(std::find(object.volumes.begin(), object.volumes.end(), volume) - object.volumes.begin()));
        }
    } else if (action == "set_part_type") {
        auto&      volume = find_volume(object, operation.volume_id);
        const auto type   = volume_type(p.at("type"));
        if (volume.is_model_part() && type != ModelVolumeType::MODEL_PART &&
            std::count_if(object.volumes.begin(), object.volumes.end(), [](const auto* v) { return v->is_model_part(); }) == 1)
            throw std::runtime_error("An object needs at least one solid model part.");
        volume.set_type(type);
        object.sort_volumes(true);
        object.invalidate_bounding_box();
    } else if (action == "assign_filament") {
        const int filament = p.at("filament_id");
        validate_filament(filament, colors, mixes);
        assign_filament(object, operation.volume_id, filament);
    } else if (action == "paint_surface") {
        auto&      volume  = find_volume(object, operation.volume_id);
        auto&      inst    = instance(object, p);
        const auto channel = p.at("channel").get<std::string>();
        const auto state   = p.at("state").get<std::string>();
        int        paint   = state == "erase" ? 0 : state == "block" ? 2 : 1;
        if (channel == "color" && paint) {
            paint = p.value("filament_id", 0);
            validate_filament(paint, colors, mixes);
        }
        if (channel == "fuzzy_skin" && state == "block")
            throw std::runtime_error("Fuzzy skin supports paint or erase.");
        auto&            annotation = channel == "color"   ? volume.mmu_segmentation_facets :
                                      channel == "support" ? volume.supported_facets :
                                      channel == "seam"    ? volume.seam_facets :
                                                             volume.fuzzy_skin_facets;
        TriangleSelector selector(volume.mesh());
        selector.deserialize(annotation.get_data());
        const Transform3d world     = inst.get_matrix() * volume.get_matrix();
        const auto        face      = p.at("face").get<std::string>();
        const auto        normal    = face == "all" ? Vec3d::Zero().eval() : face_normal(face);
        const double      dot_limit = std::cos(p.value("angle_degrees", 10.) * PI / 180.);
        const Vec3d       lower     = vector_value(p, "box_min_mm", Vec3d::Constant(-10000));
        const Vec3d       upper     = vector_value(p, "box_max_mm", Vec3d::Constant(10000));
        if ((lower.array() > upper.array()).any())
            throw std::runtime_error("The painting box has inverted bounds.");
        size_t      count = 0;
        const auto& mesh  = volume.mesh().its;
        for (size_t i = 0; i < mesh.indices.size(); ++i) {
            const auto  tri    = mesh.indices[i];
            const Vec3d a      = world * mesh.vertices[tri[0]].cast<double>();
            const Vec3d b      = world * mesh.vertices[tri[1]].cast<double>();
            const Vec3d c      = world * mesh.vertices[tri[2]].cast<double>();
            const Vec3d center = (a + b + c) / 3.;
            if ((center.array() < lower.array()).any() || (center.array() > upper.array()).any())
                continue;
            Vec3d n = (b - a).cross(c - a).normalized();
            if (world.linear().determinant() < 0)
                n = -n;
            if (face != "all" && n.dot(normal) < dot_limit)
                continue;
            selector.set_facet(int(i), EnforcerBlockerType(paint));
            ++count;
        }
        if (count == 0)
            throw std::runtime_error("No facets match this surface selection.");
        annotation.set(selector);
        edit.result["painted_facets"] = count;
    } else if (action == "split_parts") {
        auto& volume = find_volume(object, operation.volume_id);
        if (!volume.mesh().is_splittable())
            throw std::runtime_error("This part has no disconnected components.");
        edit.result["parts"] = volume.split(unsigned(std::max(size_t(1), colors.size())));
        object.invalidate_bounding_box();
    } else if (action == "simplify") {
        auto&      volume = find_volume(object, operation.volume_id);
        auto       mesh   = volume.mesh().its;
        const auto target = uint32_t(std::max(4., mesh.indices.size() * (1. - p.at("reduction_percent").get<double>() / 100.)));
        its_quadric_edge_collapse(mesh, target);
        replace_mesh(volume, TriangleMesh(std::move(mesh)));
    } else if (action == "boolean") {
        auto& volume = find_volume(object, operation.volume_id);
        auto& other  = find_volume(object, p.at("other_volume_id"));
        if (&volume == &other)
            throw std::runtime_error("Choose two different parts for a boolean operation.");
        if (!volume.is_model_part())
            throw std::runtime_error("Choose a solid model part as the boolean result target.");
        auto a = volume.mesh(), b = other.mesh();
        b.transform(volume.get_matrix().inverse() * other.get_matrix(), true);
        const auto mode = p.at("mode").get<std::string>();
        if (mode == "union")
            MeshBoolean::cgal::plus(a, b);
        else if (mode == "difference")
            MeshBoolean::cgal::minus(a, b);
        else
            MeshBoolean::cgal::intersect(a, b);
        replace_mesh(volume, std::move(a));
        object.delete_volume(size_t(std::find(object.volumes.begin(), object.volumes.end(), &other) - object.volumes.begin()));
    } else if (action == "cut") {
        auto& inst   = instance(object, p);
        Vec3d normal = vector_value(p, "normal");
        if (normal.norm() < 1e-6)
            throw std::runtime_error("The cut normal cannot be zero.");
        Transform3d              plane = Emboss::create_transformation_onto_surface(vector_value(p, "position_mm"), normal.normalized());
        ModelObjectCutAttributes flags;
        const auto               keep = p.at("keep").get<std::string>();
        if (keep != "lower")
            flags = flags | ModelObjectCutAttribute::KeepUpper;
        if (keep != "upper")
            flags = flags | ModelObjectCutAttribute::KeepLower;
        Cut cutter(&object, int(std::find(object.instances.begin(), object.instances.end(), &inst) - object.instances.begin()), plane,
                   flags);
        const auto& cut_objects = cutter.perform_with_plane();
        if (cut_objects.empty())
            throw std::runtime_error("The cut produced no geometry.");
        for (const auto* cut : cut_objects)
            edit.model.add_object(*cut);
        edit.model.delete_object(&object);
    } else
        throw std::runtime_error("This model operation is not implemented.");
    edit.model_changed = true;
    return edit;
}

LivePreview::LivePreview(PreviewCheckpoint before, std::string state_id) : m_before(std::move(before)), m_state_id(std::move(state_id)) {}

void LivePreview::verify(const std::string& state_id) const
{
    if (state_id != m_state_id)
        throw std::runtime_error("The project was edited outside this preview. Undo those later edits before applying or dismissing the "
                                 "preview, so they are not overwritten.");
}

void LivePreview::record(const std::string& state_id)
{
    if (!m_pending)
        throw std::runtime_error("Start a new preview before making more changes.");
    m_state_id = state_id;
    ++m_steps;
}

void LivePreview::accept(const std::string& state_id)
{
    verify(state_id);
    if (!m_pending || m_steps == 0)
        throw std::runtime_error("There are no pending preview changes to apply.");
    m_pending = false;
}

const PreviewCheckpoint& LivePreview::before(const std::string& state_id) const
{
    verify(state_id);
    return m_before;
}

} // namespace Slic3r::AIPrintAssistant
