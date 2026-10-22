#include "AIModelTools.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GLCanvas3D.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "Selection.hpp"
#include "Gizmos/GLGizmosManager.hpp"
#include "Jobs/Worker.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "MixedFilamentDialog.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/AISliceTools.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/nowide/fstream.hpp>
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace Slic3r::GUI {
namespace {
namespace AI                                       = AIPrintAssistant;
using json                                         = nlohmann::json;
using Gizmos                                       = GLGizmosManager;
const std::map<std::string, Gizmos::EType> editors = {{"move", Gizmos::Move},
                                                      {"rotate", Gizmos::Rotate},
                                                      {"scale", Gizmos::Scale},
                                                      {"lay_flat", Gizmos::Flatten},
                                                      {"cut", Gizmos::Cut},
                                                      {"boolean", Gizmos::MeshBoolean},
                                                      {"supports", Gizmos::FdmSupports},
                                                      {"seam", Gizmos::Seam},
                                                      {"fuzzy_skin", Gizmos::FuzzySkin},
                                                      {"color_paint", Gizmos::MmSegmentation},
                                                      {"text", Gizmos::Emboss},
                                                      {"svg", Gizmos::Svg},
                                                      {"measure", Gizmos::Measure},
                                                      {"assembly", Gizmos::Assembly},
                                                      {"simplify", Gizmos::Simplify},
                                                      {"brim_ears", Gizmos::BrimEars}};

std::vector<std::string> physical_colors()
{
    const auto* option = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    return option ? option->values : std::vector<std::string>{};
}
void ready(Plater& plater)
{
    if (!plater.get_ui_job_worker().is_idle())
        throw std::runtime_error("Wait for the current model operation to finish.");
    if (!plater.is_view3D_shown())
        throw std::runtime_error("Switch to Prepare before editing the model.");
}
void select_target(Plater& plater, const AI::ModelOperation& operation)
{
    if (operation.object_id.empty())
        return;
    auto& selection = plater.canvas3D()->get_selection();
    for (size_t i = 0; i < plater.model().objects.size(); ++i) {
        const auto* object = plater.model().objects[i];
        if (std::to_string(object->id().id) != operation.object_id)
            continue;
        const auto wanted         = operation.parameters.is_object() ? operation.parameters.value("instance_id", "") : std::string();
        int        instance_index = -1;
        if (!wanted.empty()) {
            for (size_t j = 0; j < object->instances.size(); ++j)
                if (std::to_string(object->instances[j]->id().id) == wanted)
                    instance_index = int(j);
            if (instance_index < 0)
                throw std::runtime_error("The requested instance no longer exists.");
        } else if (object->instances.size() == 1)
            instance_index = 0;
        else {
            const auto current = selection.get_content().find(int(i));
            if (current != selection.get_content().end() && current->second.size() == 1)
                instance_index = *current->second.begin();
        }
        if (operation.volume_id.empty()) {
            if (!wanted.empty())
                selection.add_instance(unsigned(i), unsigned(instance_index));
            else
                selection.add_object(unsigned(i));
        } else {
            if (instance_index < 0)
                throw std::runtime_error("Choose an instance before opening this part's editor.");
            for (size_t v = 0; v < object->volumes.size(); ++v)
                if (std::to_string(object->volumes[v]->id().id) == operation.volume_id) {
                    selection.add_volume(unsigned(i), unsigned(v), instance_index);
                    wxGetApp().obj_list()->update_selections();
                    return;
                }
            throw std::runtime_error("The selected part no longer exists.");
        }
        wxGetApp().obj_list()->update_selections();
        return;
    }
    throw std::runtime_error("The selected object no longer exists.");
}
} // namespace

json ai_slice_status(Plater& plater)
{
    // These checks run on the UI thread. Do not inspect a processor result while
    // a worker can reset or append to it (not all processor writes take its mutex).
    auto* plate = plater.get_partplate_list().get_curr_plate();
    if (!plate || plater.printer_technology() != ptFFF)
        return {{"status", "unavailable"}, {"message", "No active FFF plate."}};
    if (plater.is_background_process_slicing() || plater.is_background_process_running() || !plater.get_ui_job_worker().is_idle())
        return {{"status", "processing"}, {"message", "A slice or model operation is running. Wait until it finishes."}};
    if (plater.is_background_process_update_scheduled())
        return {{"status", "stale"}, {"message", "Model or settings updates are pending. Do not use the previous slice."}};
    if (!plate->is_slice_result_valid())
        return {{"status", "needs_slice"},
                {"message", "No current slice is available for this plate. Slice it before inspecting toolpaths."}};
    auto* result = plate->get_slice_result();
    if (!result || result->moves.empty())
        return {{"status", "not_loaded"}, {"message", "A saved slice exists but its toolpaths are not loaded. Open Preview to load it."}};
    return {{"status", "available"},
            {"slice_id", AI::slice_identity(*result, plater.get_partplate_list().get_curr_plate_index())},
            {"message", "Use get_slice_context to inspect the existing slice, optionally at a height_mm, and read_gcode for excerpts."}};
}

json ai_read_slice_tool(Plater& plater, const std::string& name, const json& arguments)
{
    if (name == "get_slice_context")
        AI::validate_slice_query(arguments);
    else if (name == "read_gcode")
        AI::validate_gcode_query(arguments);
    else
        throw std::runtime_error("Unknown slice tool.");
    auto status           = ai_slice_status(plater);
    status["plate_index"] = plater.get_partplate_list().get_curr_plate_index();
    if (status["status"] != "available")
        return status;
    const auto*                 result      = plater.get_partplate_list().get_curr_plate()->get_slice_result();
    const int                   plate_index = plater.get_partplate_list().get_curr_plate_index();
    std::lock_guard<std::mutex> lock(result->result_mutex);
    if (name == "get_slice_context")
        return AI::summarize_slice(*result, plate_index, arguments);
    // The path comes exclusively from the application's current processor result.
    boost::nowide::ifstream file(result->filename, std::ios::binary);
    return AI::read_slice_gcode(file, *result, plate_index, arguments);
}

json ai_plate_context(Plater& plater)
{
    json  result = {{"active_plate", nullptr}, {"objects", json::array()}, {"slice", ai_slice_status(plater)}};
    auto* plate  = plater.get_partplate_list().get_curr_plate();
    if (!plate)
        return result;
    const auto center      = plate->get_center_origin();
    const auto size        = plate->get_size();
    result["active_plate"] = {{"index", plater.get_partplate_list().get_curr_plate_index()},
                              {"surface_center_mm", {center.x(), center.y(), center.z()}},
                              {"size_xy_mm", {size.x(), size.y()}}};
    const auto& objects    = plater.model().objects;
    for (size_t oi = 0; oi < objects.size(); ++oi) {
        const auto* object    = objects[oi];
        json        instances = json::array();
        for (size_t ii = 0; ii < object->instances.size(); ++ii) {
            if (!plate->contain_instance(int(oi), int(ii)))
                continue;
            const auto box = object->instance_bounding_box(ii);
            instances.push_back({{"id", std::to_string(object->instances[ii]->id().id)},
                                 {"size_mm", {box.size().x(), box.size().y(), box.size().z()}},
                                 {"center_mm", {box.center().x(), box.center().y(), box.center().z()}},
                                 {"printable", object->instances[ii]->printable}});
        }
        if (!instances.empty())
            result["objects"].push_back({{"id", std::to_string(object->id().id)},
                                         {"name", object->name},
                                         {"part_count", object->volumes.size()},
                                         {"instances", std::move(instances)}});
    }
    return result;
}

AI::ModelToolContext ai_model_context(Plater& plater, bool include_selection)
{
    std::vector<std::string> selected;
    for (const auto& [index, instances] : plater.get_selection().get_content())
        if (include_selection && index >= 0 && size_t(index) < plater.model().objects.size())
            selected.push_back(std::to_string(plater.model().objects[index]->id().id));
    // Config values participate only in the local fingerprint. Credentials and
    // file paths are not exposed in the model context.
    const auto config       = wxGetApp().preset_bundle->full_config();
    json       fingerprint  = json::object();
    json       active_plate = nullptr;
    if (auto* plate = plater.get_partplate_list().get_curr_plate()) {
        const auto center = plate->get_center_origin();
        const auto bounds = plate->get_bounding_box();
        active_plate      = {{"index", plater.get_partplate_list().get_curr_plate_index()},
                             {"surface_center_mm", {center.x(), center.y(), center.z()}},
                             {"size_xy_mm", {bounds.size().x(), bounds.size().y()}}};
    }
    for (const auto& key : config.keys())
        fingerprint[key] = config.opt_serialize(key);
    // Preset identities and edited values also guard preview rollback, even
    // when a particular edited filament is masked in the composed config.
    for (const auto* presets :
         std::array<const PresetCollection*, 3>{&wxGetApp().preset_bundle->prints, &wxGetApp().preset_bundle->filaments,
                                                &wxGetApp().preset_bundle->printers}) {
        const auto& preset = presets->get_edited_preset();
        json        values;
        for (const auto& key : preset.config.keys())
            values[key] = preset.config.opt_serialize(key);
        fingerprint["edited_presets"].push_back({{"name", preset.name}, {"values", values}});
    }
    if (include_selection) {
        fingerprint["active_plate"]       = active_plate;
        fingerprint["plate"]              = plater.get_partplate_list().get_curr_plate_index();
        fingerprint["selected_volumes"]   = plater.get_selection().get_volume_idxs();
        fingerprint["selected_instances"] = json::array();
        for (const auto& [index, instances] : plater.get_selection().get_content())
            fingerprint["selected_instances"].push_back({{"object_index", index}, {"instances", instances}});
    }
    auto context = AI::capture_model_tool_context(plater.model(), selected, physical_colors(), wxGetApp().preset_bundle->mixed_filaments,
                                                  fingerprint.dump());
    context.data["active_plate"]  = active_plate;
    context.data["plate_context"] = ai_plate_context(plater);
    for (size_t oi = 0; oi < plater.model().objects.size(); ++oi)
        for (size_t ii = 0; ii < plater.model().objects[oi]->instances.size(); ++ii) {
            const int plate_index                                           = plater.get_partplate_list().find_instance(int(oi), int(ii));
            context.data["objects"][oi]["instances"][ii]["plate_index"]     = plate_index;
            context.data["objects"][oi]["instances"][ii]["on_active_plate"] = plate_index >= 0 &&
                                                                              plate_index ==
                                                                                  plater.get_partplate_list().get_curr_plate_index();
        }
    context.data["selected_parts"] = json::array();
    for (const auto index : plater.get_selection().get_volume_idxs()) {
        const auto* selected_volume = plater.get_selection().get_volume(index);
        if (!selected_volume || selected_volume->object_idx() < 0 || selected_volume->volume_idx() < 0 ||
            size_t(selected_volume->object_idx()) >= plater.model().objects.size())
            continue;
        const auto* object = plater.model().objects[size_t(selected_volume->object_idx())];
        if (size_t(selected_volume->volume_idx()) >= object->volumes.size() || selected_volume->instance_idx() < 0 ||
            size_t(selected_volume->instance_idx()) >= object->instances.size())
            continue;
        context.data["selected_parts"].push_back(
            {{"object_id", std::to_string(object->id().id)},
             {"volume_id", std::to_string(object->volumes[size_t(selected_volume->volume_idx())]->id().id)},
             {"instance_id", std::to_string(object->instances[size_t(selected_volume->instance_idx())]->id().id)}});
    }
    return context;
}

json ai_model_tools(Plater& plater)
{
    json  available = json::array();
    auto& gizmos    = plater.canvas3D()->get_gizmos_manager();
    for (const auto& [name, type] : editors) {
        const auto* tool = gizmos.get_gizmo(type);
        available.push_back({{"editor", name}, {"available_for_current_selection", tool && tool->is_activable()}});
    }
    available.push_back(
        {{"editor", "variable_layers"}, {"available_for_current_selection", plater.canvas3D()->is_layers_editing_allowed()}});
    available.push_back({{"editor", "color_mixing"}, {"available_for_current_selection", physical_colors().size() >= 2}});
    return {{"operations", AI::model_tool_catalog()},
            {"native_editors", available},
            {"notes",
             "Parameterized edits update a cumulative live preview. Native editors require a separate interactive handoff. Printing and "
             "file uploads are not model tools."}};
}

AI::PreparedModelEdit ai_prepare_model_edit(Plater& plater, const AI::ModelOperation& operation)
{
    ready(plater);
    if (plater.canvas3D()->is_layers_editing_enabled())
        throw std::runtime_error("Close variable layer height editing before preparing an assistant edit.");
    if (plater.canvas3D()->get_gizmos_manager().get_current_type() != Gizmos::Undefined)
        throw std::runtime_error("Finish or close the active model editor before applying an assistant edit.");
    auto       resolved = operation;
    const auto colors   = physical_colors();
    const auto display  = build_mixed_filament_display_context(colors);
    if (resolved.operation == "match_color") {
        wxColour target;
        if (!try_parse_color_match_hex(wxString::FromUTF8(resolved.parameters.at("color").get<std::string>()), target))
            throw std::runtime_error("Use a target color in #RRGGBB format.");
        const auto recipe = build_best_color_match_recipe(colors, target, resolved.parameters.value("min_component_percent", 0),
                                                          resolved.parameters.value("max_component_percent", 100), true, &display);
        if (!recipe.valid)
            throw std::runtime_error("No compatible mixing recipe was found with the loaded filaments.");
        const auto                weights = expand_color_match_recipe_weights(recipe, colors.size());
        std::vector<unsigned int> ids;
        std::vector<int>          percentages;
        for (size_t i = 0; i < weights.size(); ++i)
            if (weights[i] > 0) {
                ids.push_back(unsigned(i + 1));
                percentages.push_back(weights[i]);
            }
        if (ids.size() == 1) {
            if (resolved.object_id.empty())
                throw std::runtime_error(
                    "The closest color is an existing physical filament. Assign that filament instead of creating a mix.");
            resolved.operation  = "assign_filament";
            resolved.parameters = {{"filament_id", ids.front()}};
        } else {
            resolved.operation  = "create_mix";
            resolved.parameters = {{"components", ids}, {"percentages", percentages}};
        }
    }
    if (resolved.operation == "create_mix") {
        const auto ids = resolved.parameters.at("components").get<std::vector<unsigned int>>();
        for (unsigned int a : ids)
            for (unsigned int b : ids) {
                if (a == 0 || b == 0 || a > colors.size() || b > colors.size())
                    throw std::runtime_error("Choose loaded physical filament IDs.");
                if (a != b && a <= display.compatible_filaments.size() && b <= display.compatible_filaments[a - 1].size() &&
                    !display.compatible_filaments[a - 1][b - 1])
                    throw std::runtime_error("These filaments are not compatible for this mixing recipe.");
            }
    }
    std::optional<Vec3d> plate_center;
    if (auto* plate = plater.get_partplate_list().get_curr_plate())
        plate_center = plate->get_center_origin();
    auto result = AI::prepare_model_edit(plater.model(), wxGetApp().preset_bundle->mixed_filaments, colors, resolved, resources_dir(),
                                         plate_center);
    if (result.mixes_changed) {
        // Use the same calibrated preview pipeline as the native mixing dialog.
        auto&                       mix = result.mixes.mixed_filaments().back();
        MixedColorMatchRecipeResult recipe;
        recipe.valid                      = true;
        recipe.component_a                = mix.component_a;
        recipe.component_b                = mix.component_b;
        recipe.mix_b_percent              = mix.mix_b_percent;
        recipe.gradient_component_ids     = mix.gradient_component_ids;
        recipe.gradient_component_weights = mix.gradient_component_weights;
        mix.display_color = compute_color_match_recipe_display_color(recipe, display).GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
        result.result["preview_color"] = mix.display_color;
        result.result["components"]    = resolved.parameters["components"];
        result.result["percentages"]   = resolved.parameters["percentages"];
    }
    return result;
}

void ai_commit_model_edit(Plater& plater, AI::PreparedModelEdit&& edit, const std::string& name)
{
    ready(plater);
    auto& bundle = *wxGetApp().preset_bundle;
    if (edit.model_changed && plater.canvas3D()->get_gizmos_manager().get_current_type() != Gizmos::Undefined)
        throw std::runtime_error("Finish or close the active model editor before applying this edit.");
    if (edit.model_changed && plater.canvas3D()->is_layers_editing_enabled())
        throw std::runtime_error("Close variable layer height editing before applying this edit.");
    Model before;
    if (edit.model_changed)
        before = plater.model();
    auto       mixes_before   = bundle.mixed_filaments;
    const auto project_before = bundle.project_config;
    plater.take_snapshot(name);
    try {
        if (edit.model_changed) {
            plater.deselect_all();
            plater.model().replace_objects(std::move(edit.model));
            plater.get_partplate_list().reload_all_objects();
        }
        if (edit.mixes_changed) {
            bundle.mixed_filaments = std::move(edit.mixes);
            bundle.project_config.set_key_value("mixed_filament_definitions",
                                                new ConfigOptionString(bundle.mixed_filaments.serialize_custom_entries()));
            plater.on_filaments_change(physical_colors().size());
            plater.sidebar().update_color_mix_panel();
        }
        plater.on_config_change(bundle.full_config());
        plater.update(false, true);
        wxGetApp().obj_list()->update_after_undo_redo();
        if (edit.result.contains("object_id")) {
            AI::ModelOperation target;
            target.object_id = edit.result["object_id"];
            try {
                select_target(plater, target);
            } catch (const std::exception&) {}
        }
        plater.object_list_changed();
        plater.post_slice_state_change_update();
        plater.update_title_dirty_status();
        if (edit.model_changed)
            for (auto* object : plater.model().objects)
                Slic3r::save_object_mesh(*object);
    } catch (...) {
        if (edit.model_changed) {
            plater.deselect_all();
            plater.model().replace_objects(std::move(before));
            plater.get_partplate_list().reload_all_objects();
        }
        if (edit.mixes_changed) {
            bundle.mixed_filaments = std::move(mixes_before);
            bundle.project_config  = project_before;
        }
        // Restore model data even if a UI refresh failed. Preserve the original error.
        try {
            if (edit.mixes_changed) {
                plater.on_filaments_change(physical_colors().size());
                plater.sidebar().update_color_mix_panel();
            }
            plater.on_config_change(bundle.full_config());
            plater.update(false, true);
            wxGetApp().obj_list()->update_after_undo_redo();
        } catch (...) {}
        throw;
    }
}

json ai_run_native_model_tool(Plater& plater, const AI::ModelOperation& operation)
{
    ready(plater);
    if (plater.canvas3D()->is_layers_editing_enabled() || plater.canvas3D()->get_gizmos_manager().get_current_type() != Gizmos::Undefined)
        throw std::runtime_error("Finish or close the current model editor before starting another workspace action.");
    select_target(plater, operation);
    if (operation.operation == "open_editor") {
        const auto editor = operation.parameters.at("editor").get<std::string>();
        auto&      gizmos = plater.canvas3D()->get_gizmos_manager();
        const auto found  = editors.find(editor);
        if (found != editors.end()) {
            if (gizmos.get_current_type() != found->second && !gizmos.open_gizmo(found->second))
                throw std::runtime_error("This editor is unavailable for the current selection.");
        } else if (editor == "variable_layers") {
            if (!plater.canvas3D()->is_layers_editing_allowed())
                throw std::runtime_error("Select an object that supports variable layer heights.");
            plater.canvas3D()->enable_layers_editing(true);
        } else if (editor == "repair") {
            wxGetApp().obj_list()->fix_through_netfabb();
        } else if (editor == "color_mixing") {
            const auto colors = physical_colors();
            if (colors.size() < 2)
                throw std::runtime_error("Add at least two physical filaments first.");
            MixedFilamentDialog dialog(&plater, colors);
            if (dialog.ShowModal() != wxID_OK)
                return {{"status", "cancelled"}, {"applied", false}};
            AI::PreparedModelEdit edit;
            edit.mixes            = wxGetApp().preset_bundle->mixed_filaments;
            const auto&  chosen   = dialog.GetResult();
            const size_t old_size = edit.mixes.mixed_filaments().size();
            edit.mixes.add_custom_filament(chosen.component_a, chosen.component_b, chosen.mix_b_percent, colors);
            if (edit.mixes.mixed_filaments().size() == old_size)
                throw std::runtime_error("The mixed filament could not be added.");
            const auto stable_id                          = edit.mixes.mixed_filaments().back().stable_id;
            edit.mixes.mixed_filaments().back()           = chosen;
            edit.mixes.mixed_filaments().back().stable_id = stable_id;
            edit.mixes.mixed_filaments().back().custom    = true;
            edit.mixes_changed                            = true;
            ai_commit_model_edit(plater, std::move(edit), "Add mixed filament");
            return {{"status", "applied_in_native_dialog"}, {"applied", true}};
        } else
            throw std::runtime_error("Unknown native editor.");
        plater.canvas3D()->set_as_dirty();
        return {{"status", "editor_opened"}, {"editor", editor}, {"requires_interaction", true}};
    }
    const auto action = operation.parameters.at("action").get<std::string>();
    if (action == "arrange") {
        if (!plater.can_arrange())
            throw std::runtime_error("Arrange is unavailable.");
        plater.arrange();
    } else if (action == "orient")
        plater.orient();
    else if (action == "split_objects") {
        if (!plater.can_split_to_objects())
            throw std::runtime_error("This selection cannot be split into objects.");
        wxGetApp().obj_list()->split();
    } else if (action == "merge_objects") {
        if (!wxGetApp().obj_list()->can_merge_to_multipart_object())
            throw std::runtime_error("Select multiple objects to merge.");
        wxGetApp().obj_list()->merge(true);
    } else if (action == "split_instances") {
        if (!wxGetApp().obj_list()->can_split_instances())
            throw std::runtime_error("This selection has no instances to separate.");
        wxGetApp().obj_list()->split_instances();
    } else if (action == "fill_bed")
        plater.fill_bed_with_instances();
    else if (action == "fit_to_bed")
        plater.scale_selection_to_fit_print_volume();
    else
        throw std::runtime_error("Unknown native action.");
    return {{"status", "native_action_dispatched"},
            {"action", action},
            {"note", "Inspect the workspace; native jobs or dialogs may still be active."}};
}
} // namespace Slic3r::GUI
