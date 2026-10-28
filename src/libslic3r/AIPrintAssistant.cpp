#include "AIPrintAssistant.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>

#include <openssl/evp.h>

#include "Model.hpp"

namespace Slic3r::AIPrintAssistant {
namespace {
using json = nlohmann::json;

const char* type_name(ConfigOptionType type)
{
    switch (type) {
    case coFloat: return "number";
    case coInt: return "integer";
    case coBool: return "boolean";
    case coString: return "string";
    case coPercent: return "percent";
    case coFloatOrPercent: return "number_or_percent";
    case coEnum: return "enum";
    case coFloats: return "number_array";
    case coInts: return "integer_array";
    case coBools: return "boolean_array";
    case coStrings: return "string_array";
    case coPercents: return "percent_array";
    case coFloatsOrPercents: return "number_or_percent_array";
    case coEnums: return "enum_array";
    case coPoint: return "point_2d";
    case coPoint3: return "point_3d";
    case coPoints: return "point_2d_array";
    default: throw std::runtime_error("Unsupported setting type.");
    }
}

void require_fields(const json& value, std::initializer_list<const char*> fields)
{
    if (!value.is_object() || value.size() != fields.size())
        throw std::runtime_error("The proposal has missing or unexpected fields.");
    for (const char* field : fields)
        if (!value.contains(field))
            throw std::runtime_error("The proposal is missing a required field.");
}

std::string string_field(const json& value, const char* key, size_t limit, bool allow_empty = false)
{
    const auto& item = value.at(key);
    if (!item.is_string())
        throw std::runtime_error("A proposal text field has the wrong type.");
    auto text = item.get<std::string>();
    if ((!allow_empty && text.empty()) || text.size() > limit || text.find('\0') != std::string::npos)
        throw std::runtime_error("A proposal text field is empty or too long.");
    return text;
}

std::string serialized_config(const DynamicPrintConfig& config)
{
    // Canonicalize logical values: the preset text format rounds doubles and
    // assumes enum-name maps; the archive format may include representation
    // details. Neither is suitable for a stable exact-value fingerprint.
    json values = json::object();
    for (const auto& key : config.keys()) {
        const auto& option = *config.option(key);
        const auto* def    = print_config_def.get(key);
        if (def == nullptr || def->type != option.type())
            throw std::runtime_error("The current configuration contains an unsupported option: " + key);
        json value;
        switch (option.type()) {
        case coFloat:
        case coPercent: value = option.getFloat(); break;
        case coInt:
        case coEnum: value = option.getInt(); break;
        case coBool: value = option.getBool(); break;
        case coString: value = dynamic_cast<const ConfigOptionString&>(option).value; break;
        case coFloats:
        case coPercents: value = dynamic_cast<const ConfigOptionVector<double>&>(option).values; break;
        case coInts:
        case coEnums: value = dynamic_cast<const ConfigOptionVector<int>&>(option).values; break;
        case coBools: value = dynamic_cast<const ConfigOptionVector<unsigned char>&>(option).values; break;
        case coStrings: value = dynamic_cast<const ConfigOptionVector<std::string>&>(option).values; break;
        case coFloatOrPercent: {
            const auto& number = dynamic_cast<const ConfigOptionFloatOrPercent&>(option);
            value              = {number.value, number.percent};
            break;
        }
        case coFloatsOrPercents: {
            value = json::array();
            for (const auto& number : dynamic_cast<const ConfigOptionVector<FloatOrPercent>&>(option).values)
                value.push_back({number.value, number.percent});
            break;
        }
        case coPoint: {
            const auto& point = dynamic_cast<const ConfigOptionPoint&>(option).value;
            value             = {point.x(), point.y()};
            break;
        }
        case coPoints: {
            value = json::array();
            for (const auto& point : dynamic_cast<const ConfigOptionPoints&>(option).values)
                value.push_back({point.x(), point.y()});
            break;
        }
        case coPoint3: {
            const auto& point = dynamic_cast<const ConfigOptionPoint3&>(option).value;
            value             = {point.x(), point.y(), point.z()};
            break;
        }
        default: throw std::runtime_error("The current configuration contains an unsupported option: " + key);
        }
        values[key] = std::move(value);
    }
    return values.dump();
}

json matrix_json(const Transform3d& transform)
{
    json values = json::array();
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            values.push_back(transform.matrix()(row, col));
    return values;
}

std::string fingerprint(const json& basis, const Context& context, const ModelObject* object)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Could not capture the current project state.");
    auto update = [&](const void* data, size_t length) {
        if (EVP_DigestUpdate(digest.get(), data, length) != 1)
            throw std::runtime_error("Could not capture the current project state.");
    };
    const auto text = basis.dump();
    update(text.data(), text.size());
    for (const auto* config : {&context.inherited, &context.overrides, object ? &object->config.get() : nullptr}) {
        if (!config)
            continue;
        const auto   bytes  = serialized_config(*config);
        const size_t length = bytes.size();
        update(&length, sizeof(length));
        update(bytes.data(), bytes.size());
    }
    // Bounds and triangle counts alone miss edits within the same bounding box.
    if (object)
        for (const auto* part : object->volumes) {
            const auto part_state = json({{"id", part->id().id},
                                          {"type", int(part->type())},
                                          {"transform", matrix_json(part->get_matrix())},
                                          {"config", serialized_config(part->config.get())}})
                                        .dump();
            update(part_state.data(), part_state.size());
            const auto& mesh = part->mesh().its;
            for (const auto& vertex : mesh.vertices)
                update(vertex.data(), 3 * sizeof(float));
            for (const auto& triangle : mesh.indices)
                update(triangle.data(), 3 * sizeof(int));
            const auto& volume = *part;
            for (const auto* facets :
                 {&volume.supported_facets, &volume.seam_facets, &volume.mmu_segmentation_facets, &volume.fuzzy_skin_facets}) {
                const auto&  data  = facets->get_data();
                const size_t count = data.triangles_to_split.size();
                update(&count, sizeof(count));
                for (const auto& triangle : data.triangles_to_split) {
                    update(&triangle.triangle_idx, sizeof(triangle.triangle_idx));
                    update(&triangle.bitstream_start_idx, sizeof(triangle.bitstream_start_idx));
                }
                const auto bits = json(data.bitstream).dump();
                update(bits.data(), bits.size());
            }
        }
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned int  length = 0;
    if (EVP_DigestFinal_ex(digest.get(), bytes, &length) != 1)
        throw std::runtime_error("Could not capture the current project state.");
    static constexpr char HEX[] = "0123456789abcdef";
    std::string           result;
    result.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) {
        result.push_back(HEX[bytes[i] >> 4]);
        result.push_back(HEX[bytes[i] & 15]);
    }
    return result;
}

void validate_candidate(const Context& context, const DynamicPrintConfig& overrides, const t_config_option_keys& keys)
{
    DynamicPrintConfig candidate = context.inherited;
    candidate.apply(overrides);
    const bool arachne = candidate.opt_enum<PerimeterGeneratorType>("wall_generator") == PerimeterGeneratorType::Arachne;
    if (!arachne && candidate.opt_enum<FuzzySkinMode>("fuzzy_skin_mode") != FuzzySkinMode::Displacement)
        throw std::runtime_error("This fuzzy skin mode requires Arachne. The wall generator cannot be changed to Classic.");
    for (const auto& key : keys) {
        if ((key == "min_feature_size" || key == "min_bead_width" || key == "wall_transition_angle") && !arachne)
            throw std::runtime_error("Arachne detail settings require the Arachne wall generator.");
    }
    // Never validate a sparse patch: validate() fills missing fields with defaults.
    const auto errors = candidate.validate(true);
    if (!errors.empty())
        throw std::runtime_error("Configuration checks failed for " + errors.begin()->first + ": " + errors.begin()->second);
}

void populate_settings(Context& context)
{
    json catalog = json::array();
    for (const auto& key : context.effective.keys()) {
        if (!setting_in_scope(key, context.scope) || (context.scope != Scope::Object && !context.overrides.has(key)))
            continue;
        const auto* def   = print_config_def.get(key);
        const auto  value = setting_value(context.effective, key);
        json        entry = {{"key", key},
                             {"label", def->label},
                             {"description", def->tooltip},
                             {"type", type_name(def->type)},
                             {"unit", def->sidetext},
                             {"category", def->category},
                             {"scope", scope_name(context.scope)},
                             {"current_value", value},
                             {"nullable", def->nullable},
                             {"minimum", def->min},
                             {"maximum", def->max},
                             {"is_code", def->is_code},
                             {"ratio_over", def->ratio_over},
                             {"value_source", context.scope != Scope::Object ? "edited_preset" :
                                              context.overrides.has(key)     ? "object_override" :
                                                                               "inherited"}};
        if (!def->enum_values.empty())
            entry["allowed_values"] = def->enum_values;
        if (value.is_array() && (def->type != coPoints || key == "extruder_offset"))
            entry["array_length"] = value.size();
        context.editable_keys.push_back(key);
        catalog.push_back(std::move(entry));
    }
    json read_only = json::object();
    for (const auto& key : context.effective.keys())
        if (std::find(context.editable_keys.begin(), context.editable_keys.end(), key) == context.editable_keys.end() &&
            (setting_in_scope(key, Scope::Process) || setting_in_scope(key, Scope::Filament) || setting_in_scope(key, Scope::Printer)))
            read_only[key] = setting_value(context.effective, key);

    context.public_context["settings"]           = std::move(catalog);
    context.public_context["read_only_settings"] = std::move(read_only);
}
} // namespace

Context capture_context(const DynamicPrintConfig& inherited,
                        const ModelObject&        object,
                        Scope                     scope,
                        const DynamicPrintConfig* target,
                        const std::string&        target_identity,
                        const json&               image_context)
{
    if (scope != Scope::Object) {
        if (!target)
            throw std::runtime_error("Select a settings target before requesting changes.");
        return capture_preset_context(inherited, scope, *target, target_identity, image_context);
    }
    // FullPrintConfig's FFF defaults omit printer_technology. Match validate()
    // and treat an absent technology as FFF instead of dereferencing nullptr.
    const auto* technology = inherited.option("printer_technology");
    if (technology != nullptr && static_cast<PrinterTechnology>(technology->getInt()) != ptFFF)
        throw std::runtime_error("The print assistant currently supports filament printing only.");
    if (object.volumes.empty() || object.instances.size() != 1 ||
        std::none_of(object.volumes.begin(), object.volumes.end(), [](const auto* part) { return part->is_model_part(); }))
        throw std::runtime_error("Select an object with solid geometry and one instance.");
    if (!object.layer_config_ranges.empty() || !object.layer_height_profile.empty())
        throw std::runtime_error("Objects with height-range overrides or variable layer height are not supported yet.");
    json   parts     = json::array();
    size_t triangles = 0;
    for (const auto* part : object.volumes) {
        const auto& volume = *part;
        if (!volume.is_model_part() && volume.type() != ModelVolumeType::NEGATIVE_VOLUME)
            throw std::runtime_error("Objects with settings modifiers or support volumes are not supported by the settings assistant yet.");
        if (!volume.material_id().empty())
            throw std::runtime_error("Objects with material-specific overrides are not supported yet.");
        // A part's extruder assignment is read as context. Other part overrides may
        // shadow object settings or change dependencies; do not misrepresent them.
        for (const auto& key : volume.config.keys())
            if (key != "extruder")
                throw std::runtime_error("Remove part-specific setting overrides before using the print assistant.");
        if (volume.mesh().empty())
            throw std::runtime_error("The selected object has no printable geometry.");
        triangles += volume.mesh().facets_count();
        parts.push_back({{"id", std::to_string(volume.id().id)},
                         {"name", volume.name},
                         {"negative", !volume.is_model_part()},
                         {"filament_id", volume.extruder_id()},
                         {"triangles", volume.mesh().facets_count()}});
    }
    const auto& volume = *object.volumes.front();

    Context context;
    context.scope           = scope;
    context.target_identity = target_identity;
    context.object_id       = object.id();
    context.inherited       = inherited;
    context.overrides       = object.config.get();
    context.effective       = inherited;
    context.effective.apply(context.overrides);

    const auto size = object.instance_bounding_box(size_t(0)).size();

    context.public_context =
        {{"schema_version", 1},
         {"object_id", std::to_string(object.id().id)},
         {"dimensions_mm", {size.x(), size.y(), size.z()}},
         {"triangle_count", triangles},
         {"parts", parts},
         {"instance_transform", matrix_json(object.instances.front()->get_matrix())},
         {"part_extruder", volume.config.has("extruder") ? volume.config.opt_serialize("extruder") : "inherited"},
         {"edit_scope", scope_name(scope)},
         {"images", image_context},
         {"limitations",
          {"Only explicitly attached images are available. Use get_slice_context separately to inspect an existing slice.",
           "Dimensions do not establish the size or printability of individual features.",
           "Only settings in the selected edit scope can be changed.",
           "Preset edits can affect all objects using the preset. Object or plate overrides may shadow them."}}};

    populate_settings(context);

    // Complete configuration stays local. Changes to inherited values invalidate
    // the request even when preset names and dirty flags remain the same.
    json basis                           = {{"context", context.public_context},
                                            {"target_identity", target_identity},
                                            {"model_id", object.get_model()->id().id},
                                            {"volume_id", volume.id().id},
                                            {"instance_id", object.instances.front()->id().id},
                                            {"volume_transform", matrix_json(volume.get_matrix())}};
    context.id                           = fingerprint(basis, context, &object);
    context.public_context["context_id"] = context.id;
    return context;
}

ModelObject* resolve_settings_object(const std::vector<ModelObject*>& plate_objects, const std::string& object_id)
{
    ModelObject* sole     = nullptr;
    bool         multiple = false;
    for (auto* object : plate_objects) {
        if (!object_id.empty() && std::to_string(object->id().id) == object_id)
            return object;
        if (sole && sole->id() != object->id())
            multiple = true;
        sole = object;
    }
    if (!object_id.empty())
        throw std::runtime_error("This object is not on the active plate. Read get_model_context again for current object IDs.");
    return multiple ? nullptr : sole;
}

Context capture_preset_context(const DynamicPrintConfig& inherited,
                               Scope                     scope,
                               const DynamicPrintConfig& target,
                               const std::string&        target_identity,
                               const json&               image_context)
{
    if (scope == Scope::Object)
        throw std::runtime_error("Object settings require an object context.");
    auto context                             = capture_read_context(inherited, nullptr, scope, &target, target_identity, image_context);
    context.read_only                        = false;
    context.public_context["read_only"]      = false;
    context.public_context["settings_basis"] = "Global edited preset; object, part and plate overrides are not applied.";
    context.public_context["limitations"] =
        {"Preset edits affect all objects using the preset. Object, part or plate overrides may shadow them.",
         "Copy the empty object_id for preset changes; no object selection is required.",
         "Use get_slice_context separately to inspect an existing slice."};
    context.public_context.erase("context_id");
    context.id = fingerprint({{"context", context.public_context}, {"target_identity", target_identity}}, context, nullptr);
    context.public_context["context_id"] = context.id;
    return context;
}

Context capture_read_context(const DynamicPrintConfig& inherited,
                             const ModelObject*        object,
                             Scope                     scope,
                             const DynamicPrintConfig* target,
                             const std::string&        target_identity,
                             const json&               image_context)
{
    const auto* technology = inherited.option("printer_technology");
    if (technology && static_cast<PrinterTechnology>(technology->getInt()) != ptFFF)
        throw std::runtime_error("The print assistant currently supports filament printing only.");
    Context context;
    context.read_only       = true;
    context.scope           = scope;
    context.target_identity = target_identity;
    context.inherited       = inherited;
    if (scope == Scope::Object && object)
        context.overrides = object->config.get();
    else if (scope != Scope::Object && target)
        context.overrides = *target;
    context.effective = inherited;
    context.effective.apply(context.overrides);
    if (object)
        context.object_id = object->id();
    context.public_context = {{"schema_version", 1},
                              {"read_only", true},
                              {"object_id", object ? std::to_string(object->id().id) : ""},
                              {"edit_scope", scope_name(scope)},
                              {"images", image_context},
                              {"settings_basis", object ? "Object base settings; region and part overrides are listed separately." :
                                                          "Shared preset and plate settings; object overrides are not applied."},
                              {"limitations",
                               {"This inspection context cannot authorize changes.",
                                "Use get_print_context with an object_id from the active plate for targeted settings.",
                                "Use get_slice_context separately to inspect an existing slice."}}};
    if (object) {
        context.public_context["instance_count"]            = object->instances.size();
        context.public_context["has_variable_layer_height"] = !object->layer_height_profile.empty();
        auto public_overrides                               = [](const DynamicPrintConfig& config) {
            json values = json::object();
            for (const auto& key : config.keys())
                if (setting_in_scope(key, Scope::Object) || setting_in_scope(key, Scope::Process) ||
                    setting_in_scope(key, Scope::Filament) || setting_in_scope(key, Scope::Printer))
                    values[key] = setting_value(config, key);
            return values;
        };
        context.public_context["object_overrides"] = public_overrides(object->config.get());
        context.public_context["part_overrides"]   = json::array();
        for (const auto* part : object->volumes)
            context.public_context["part_overrides"].push_back({{"part_id", std::to_string(part->id().id)},
                                                                {"type", int(part->type())},
                                                                {"settings", public_overrides(part->config.get())}});
        context.public_context["height_range_overrides"] = json::array();
        for (const auto& [range, config] : object->layer_config_ranges)
            context.public_context["height_range_overrides"].push_back(
                {{"height_mm", {range.first, range.second}}, {"settings", public_overrides(config.get())}});
    }
    populate_settings(context);
    context.id = fingerprint({{"context", context.public_context}, {"target_identity", target_identity}}, context, object);
    context.public_context["context_id"] = context.id;
    return context;
}

json parse_json(const std::string& text, size_t size_limit)
{
    if (text.empty() || text.size() > size_limit)
        throw std::runtime_error("The response is empty or exceeds the size limit.");
    std::vector<std::set<std::string>> object_keys;
    try {
        return json::parse(text, [&](int depth, json::parse_event_t event, json& parsed) {
            if (depth > 32)
                throw std::runtime_error("The response contains too many nested fields.");
            if (event == json::parse_event_t::object_start)
                object_keys.emplace_back();
            else if (event == json::parse_event_t::object_end)
                object_keys.pop_back();
            else if (event == json::parse_event_t::key && !object_keys.back().insert(parsed.get<std::string>()).second)
                throw std::runtime_error("The response contains duplicate JSON fields.");
            return true;
        });
    } catch (const json::exception&) {
        // Do not echo input: API responses and imported files are untrusted.
        throw std::runtime_error("The response is not a complete, valid JSON document.");
    }
}

json proposal_schema()
{
    // A string payload holds typed JSON. This avoids unbounded recursive schemas
    // on providers while allowing every slicer scalar, vector and point type.
    const json change_properties = {{"key", {{"type", "string"}}}, {"value", {{"type", {"number", "string", "boolean"}}}}};
    const json change_schema     = {{"type", "object"},
                                    {"additionalProperties", false},
                                    {"required", {"key", "value"}},
                                    {"properties", change_properties}};
    const json properties        = {{"schema_version", {{"type", "integer"}, {"enum", {1}}}},
                                    {"context_id", {{"type", "string"}}},
                                    {"object_id", {{"type", "string"}}},
                                    {"outcome", {{"type", "string"}, {"enum", {"propose_changes", "ask_question", "no_change"}}}},
                                    {"explanation", {{"type", "string"}}},
                                    {"changes", {{"type", "array"}, {"items", change_schema}}}};
    return {{"type", "object"},
            {"additionalProperties", false},
            {"required", {"schema_version", "context_id", "object_id", "outcome", "explanation", "changes"}},
            {"properties", properties}};
}

Proposal parse_proposal(const std::string& text)
{
    const json document = parse_json(text);
    require_fields(document, {"schema_version", "context_id", "object_id", "outcome", "explanation", "changes"});
    if (!document["schema_version"].is_number_integer() || document["schema_version"] != 1)
        throw std::runtime_error("The proposal uses an unsupported schema version.");
    Proposal proposal;
    proposal.context_id  = string_field(document, "context_id", 64);
    proposal.object_id   = string_field(document, "object_id", 32, true);
    proposal.outcome     = string_field(document, "outcome", 32);
    proposal.explanation = string_field(document, "explanation", 8000);
    if (proposal.outcome != "propose_changes" && proposal.outcome != "ask_question" && proposal.outcome != "no_change")
        throw std::runtime_error("The proposal has an unsupported outcome.");
    const auto& changes = document["changes"];
    if (!changes.is_array() || changes.size() > print_config_def.options.size() ||
        (proposal.outcome == "propose_changes") == changes.empty())
        throw std::runtime_error("The proposal outcome does not match its setting changes.");
    for (const auto& change : changes) {
        require_fields(change, {"key", "value"});
        const auto key = string_field(change, "key", 80);
        if (proposal.changes.has(key))
            throw std::runtime_error("The proposal changes the same setting more than once.");
        const auto* def = print_config_def.get(key);
        if (!def)
            throw std::runtime_error("Unknown print setting: " + key);
        auto value = change["value"];
        // Arrays and points use a JSON-encoded string on the wire; scalar strings
        // remain literal (including G-code and percent-or-absolute values).
        if ((def->type & coVectorType) || def->type == coPoint || def->type == coPoint3) {
            if (!value.is_string())
                throw std::runtime_error("Array settings require a JSON-encoded array string.");
            value = parse_json(value.get<std::string>());
        }
        assign_setting(proposal.changes, key, value);
    }
    return proposal;
}

DynamicPrintConfig prepare_apply(const Context& current, const Proposal& proposal, Transaction& transaction)
{
    if (current.read_only)
        throw std::runtime_error("This is a read-only plate overview. Read get_print_context with an object_id before proposing settings.");
    if (proposal.context_id != current.id || proposal.object_id != (current.object_id.valid() ? std::to_string(current.object_id.id) : ""))
        throw std::runtime_error("The object or its settings changed. Ask again or export a fresh context.");
    if (proposal.outcome != "propose_changes" || proposal.changes.empty())
        throw std::runtime_error("This response has no changes to apply.");
    DynamicPrintConfig candidate = current.overrides;
    Transaction        prepared;
    prepared.object_id       = current.object_id;
    prepared.scope           = current.scope;
    prepared.target_identity = current.target_identity;
    prepared.before          = current.overrides;
    for (const auto& key : proposal.changes.keys()) {
        if (std::find(current.editable_keys.begin(), current.editable_keys.end(), key) == current.editable_keys.end())
            throw std::runtime_error("The setting does not belong to the selected scope: " + key);
        const auto* before = current.effective.option(key);
        const auto* after  = proposal.changes.option(key);
        if (before->type() != after->type())
            throw std::runtime_error("Setting type changed: " + key);
        if (!before->is_scalar() && (before->type() != coPoints || key == "extruder_offset") &&
            dynamic_cast<const ConfigOptionVectorBase*>(before)->size() != dynamic_cast<const ConfigOptionVectorBase*>(after)->size())
            throw std::runtime_error("Keep the number and order of array elements unchanged: " + key);
        if (key == "printable_area") {
            const auto& points = dynamic_cast<const ConfigOptionPoints*>(after)->values;
            double      area   = 0.;
            for (size_t i = 0; i < points.size(); ++i) {
                const auto& a = points[i];
                const auto& b = points[(i + 1) % points.size()];
                area += a.x() * b.y() - b.x() * a.y();
            }
            if (points.size() < 3 || !std::isfinite(area) || std::abs(area) < 1e-6)
                throw std::runtime_error("The printable area must be a polygon with nonzero area.");
        }
        if (*before == *after)
            continue;
        candidate.set_key_value(key, proposal.changes.option(key)->clone());
        prepared.changed_keys.push_back(key);
    }
    if (prepared.changed_keys.empty())
        throw std::runtime_error("These values are already active. No changes are needed.");
    validate_candidate(current, candidate, proposal.changes.keys());
    prepared.after = candidate;
    transaction    = std::move(prepared);
    return candidate;
}

DynamicPrintConfig prepare_revert(const Context& current, const Transaction& transaction)
{
    if (current.object_id != transaction.object_id || current.scope != transaction.scope ||
        current.target_identity != transaction.target_identity || transaction.changed_keys.empty())
        throw std::runtime_error("Select the object whose assistant changes you want to revert.");
    DynamicPrintConfig candidate = current.overrides;
    for (const auto& key : transaction.changed_keys) {
        const auto* applied = transaction.after.option(key);
        const auto* now     = current.overrides.option(key);
        if (now == nullptr || applied == nullptr || *now != *applied)
            throw std::runtime_error("An assistant setting was edited or undone. Revert would overwrite a later change.");
        if (const auto* before = transaction.before.option(key))
            candidate.set_key_value(key, before->clone());
        else
            candidate.erase(key);
    }
    // Restoring inactive Arachne options is legitimate when returning to Classic.
    validate_candidate(current, candidate, {});
    return candidate;
}
} // namespace Slic3r::AIPrintAssistant
