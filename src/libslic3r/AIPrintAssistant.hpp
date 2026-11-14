#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ObjectID.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {
class ModelObject;

namespace AIPrintAssistant {

// Printer settings are classified so they can be shown, but the assistant never
// edits them: no context is captured in Scope::Printer.
enum class Scope { Object, Process, Filament, Printer };
const char*    scope_name(Scope scope);
bool           setting_in_scope(const std::string& key, Scope scope);
// Process settings whose defaults already print well, from Snapmaker's print
// experts. The assistant changes one only after the user confirms that change.
const std::vector<std::string>& keep_default_settings();
bool                            keep_default_setting(const std::string& key);
nlohmann::json setting_value(const ConfigBase& config, const std::string& key);
void           assign_setting(DynamicPrintConfig& config, const std::string& key, const nlohmann::json& value);

// All capture/commit operations run on the UI thread. Only public_context is
// sent to the provider; the complete configuration below never leaves the app.
struct Context
{
    bool                 read_only = false;
    Scope                scope     = Scope::Object;
    std::string          target_identity;
    t_config_option_keys editable_keys;
    ObjectID             object_id;
    std::string          id;
    nlohmann::json       public_context;
    DynamicPrintConfig   inherited;
    DynamicPrintConfig   effective;
    DynamicPrintConfig   overrides;
};

struct Proposal
{
    std::string        context_id;
    std::string        object_id;
    std::string        outcome;
    std::string        explanation;
    DynamicPrintConfig changes;
};

struct Transaction
{
    Scope                scope = Scope::Object;
    std::string          target_identity;
    ObjectID             object_id;
    DynamicPrintConfig   before;
    DynamicPrintConfig   after;
    t_config_option_keys changed_keys;
};

// Throws std::runtime_error for unsupported geometry/configuration or proposals.
Context capture_context(const DynamicPrintConfig& inherited,
                        const ModelObject&        object,
                        Scope                     scope           = Scope::Object,
                        const DynamicPrintConfig* target          = nullptr,
                        const std::string&        target_identity = {},
                        const nlohmann::json&     image_context   = nlohmann::json::array());
// Global presets do not require printable geometry or a selected object.
Context capture_preset_context(const DynamicPrintConfig& inherited,
                               Scope                     scope,
                               const DynamicPrintConfig& target,
                               const std::string&        target_identity = {},
                               const nlohmann::json&     image_context   = nlohmann::json::array());
// Resolve a stable ID among the active plate's objects. Without an ID, only a
// sole distinct object is inferred; empty/ambiguous plates return nullptr.
ModelObject* resolve_settings_object(const std::vector<ModelObject*>& plate_objects, const std::string& object_id = {});
// Inspection remains available on empty/multi-object plates and for geometry
// that is not yet supported for automatic settings edits.
Context        capture_read_context(const DynamicPrintConfig& inherited,
                                    const ModelObject*        object,
                                    Scope                     scope,
                                    const DynamicPrintConfig* target          = nullptr,
                                    const std::string&        target_identity = {},
                                    const nlohmann::json&     image_context   = nlohmann::json::array());
nlohmann::json proposal_schema();
nlohmann::json parse_json(const std::string& text, size_t size_limit = 64 * 1024);
Proposal       parse_proposal(const std::string& text);

// The keep-default settings a proposal would change in this context.
t_config_option_keys keep_default_changes(const Context& current, const Proposal& proposal);
// Prepare sparse object overrides or a complete edited preset, without mutating
// live state. The GUI commits through the corresponding native configuration UI.
// A change to a keep-default setting is refused unless its key is in
// confirmed_defaults, the changes the user has just approved.
DynamicPrintConfig prepare_apply(const Context&              current,
                                 const Proposal&             proposal,
                                 Transaction&                transaction,
                                 const t_config_option_keys& confirmed_defaults = {});
DynamicPrintConfig prepare_revert(const Context& current, const Transaction& transaction);
// A modifier's settings after applying `settings` (key: value, null removes a
// key) to `current`. Only per-region keys, and no keep-default settings;
// validated over the object's settings.
DynamicPrintConfig prepare_modifier_settings(const DynamicPrintConfig& object_settings,
                                             const DynamicPrintConfig& current,
                                             const nlohmann::json&     settings);

} // namespace AIPrintAssistant
} // namespace Slic3r
