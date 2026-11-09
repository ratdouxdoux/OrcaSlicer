#include "GLGizmoGloss.hpp"

#include "libslic3r/Flow.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r::GUI {
namespace {
const std::vector<ColorRGBA> GLOSS_COLORS{GLVolume::NEUTRAL_COLOR,
                                          {0.15f, 0.65f, 1.f, 1.f},
                                          {0.65f, 0.35f, 0.95f, 1.f},
                                          {1.f, 0.55f, 0.15f, 1.f},
                                          {0.2f, 0.8f, 0.5f, 1.f}};
}

GLGizmoGloss::GLGizmoGloss(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoGloss::on_init()
{
    m_shortcut_key = WXK_CONTROL_G;
    return true;
}

void GLGizmoGloss::on_shutdown()
{
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

void GLGizmoGloss::render_painter_gizmo()
{
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));
    render_triangles(m_parent.get_selection());
    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();
    glsafe(::glDisable(GL_BLEND));
}

GlossSpeedRange GLGizmoGloss::estimated_speed_range() const
{
    FullPrintConfig config;
    config.apply(wxGetApp().preset_bundle->full_config(), true);
    const ModelObject* object = m_c->selection_info()->model_object();
    config.apply(object->config.get(), true);
    // This is a nominal estimate. G-code uses each path's actual cross-section,
    // active filament/flow variant, surface speed and first-layer/overhang caps.
    double minimum = 1., maximum = 10000.;
    for (const ModelVolume* volume : object->volumes) {
        if (!volume->is_model_part())
            continue;
        const size_t extruder = size_t(std::max(1, volume->extruder_id()) - 1);
        const double nozzle   = config.nozzle_diameter.get_at(extruder);
        double       width    = config.outer_wall_line_width.get_abs_value(nozzle);
        if (width <= 0.)
            width = nozzle;
        const double cross_section = Flow(float(width), float(config.layer_height.value), float(nozzle)).mm3_per_mm() *
                                     config.filament_flow_ratio.get_at(extruder) * config.print_flow_ratio.value;
        const double diameter      = config.filament_diameter.get_at(extruder);
        const double flow_limit    = config.filament_max_volumetric_speed.get_at(extruder);
        double       feature_speed = config.outer_wall_speed.get_at(0);
        if (feature_speed <= 0.)
            feature_speed = flow_limit > 0. ? flow_limit / cross_section : 0.;
        if (config.top_surface_speed.get_at(0) > 0.)
            feature_speed = std::min(feature_speed, config.top_surface_speed.get_at(0));
        const auto range = gloss_speed_range(feature_speed, config.slow_down_min_speed.get_at(extruder), 0.,
                                             config.machine_max_speed_x.get_at(0), config.machine_max_speed_y.get_at(0),
                                             config.machine_max_speed_e.get_at(0), flow_limit, cross_section,
                                             cross_section / (0.25 * M_PI * diameter * diameter));
        minimum          = std::max(minimum, range.min_speed);
        maximum          = std::min(maximum, range.max_speed);
    }
    return {std::min(minimum, maximum), maximum};
}

void GLGizmoGloss::save_times()
{
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Change gloss surface time"), UndoRedo::SnapshotType::GizmoAction);
    std::vector<double>  values(m_times.begin(), m_times.end());
    m_c->selection_info()->model_object()->config.set_key_value("gloss_paint_times", new ConfigOptionFloats(values));
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    m_parent.set_as_dirty();
}

void GLGizmoGloss::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c->selection_info()->model_object())
        return;
    y = std::min(y, bottom_limit - m_imgui->scaled(28.f));
#if BBS_TOOLBAR_ON_TOP
    GizmoImguiSetNextWIndowPos(x, y, std::max(last_input_window_width, m_imgui->scaled(25.f)), 0.f, ImGuiCond_Always, 0.f, 0.f);
#else
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 1.f, 0.f);
#endif
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::PushItemWidth(m_imgui->scaled(12.f));
    m_imgui->text(_L("Gloss colors"));
    for (size_t color = 0; color < GLOSS_PAINT_COLORS; ++color) {
        ImGui::PushID(int(color));
        if (color != 0)
            ImGui::SameLine();
        const ColorRGBA& c = GLOSS_COLORS[color + 1];
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c.r(), c.g(), c.b(), 1.f));
        if (ImGui::Button(std::to_string(color + 1).c_str(), ImVec2(m_imgui->scaled(2.5f), 0.f)))
            m_color = color;
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::Text("%s %u", _u8L("Selected color:").c_str(), unsigned(m_color + 1));
    const auto range = estimated_speed_range();
    m_times[m_color] = float(std::clamp(double(m_times[m_color]), range.min_time(), range.max_time()));
    m_imgui->text(_L("Surface time (s / 100 mm)"));
    ImGui::SliderFloat("##gloss_time", &m_times[m_color], float(range.min_time()), float(range.max_time()), "%.3f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (!std::isfinite(m_times[m_color]))
            m_times[m_color] = float(range.min_time());
        m_times[m_color] = float(std::clamp(double(m_times[m_color]), range.min_time(), range.max_time()));
        save_times();
    }
    ImGui::Text("%s %.2f - %.2f s / 100 mm", _u8L("Estimated range:").c_str(), range.min_time(), range.max_time());
    ImGui::Text("%s %.1f mm/s", _u8L("Limited surface speed:").c_str(), range.speed_for_time(m_times[m_color]));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + m_imgui->scaled(23.f));
    m_imgui->text(_L("Paint outer walls and top surfaces. Longer times mean slower surface printing. "
                     "Calibrate each finish for your filament."));
    m_imgui->text(
        _L("Actual times depend on acceleration, flow and cooling. First layers, bridges and ironing keep their normal settings."));
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    int                           brush = m_tool_type == ToolType::SMART_FILL       ? 3 :
                                          m_cursor_type == TriangleSelector::CIRCLE ? 0 :
                                          m_cursor_type == TriangleSelector::SPHERE ? 1 :
                                                                                      2;
    const std::array<wxString, 4> labels{_L("Circle"), _L("Sphere"), _L("Triangles"), _L("Smart fill")};
    for (int i = 0; i < int(labels.size()); ++i) {
        if (i % 2 != 0)
            ImGui::SameLine();
        if (ImGui::RadioButton(into_u8(labels[i]).c_str(), brush == i)) {
            m_tool_type   = i == 3 ? ToolType::SMART_FILL : ToolType::BRUSH;
            m_cursor_type = i == 0 ? TriangleSelector::CIRCLE : i == 1 ? TriangleSelector::SPHERE : TriangleSelector::POINTER;
            for (auto& selector : m_triangle_selectors) {
                selector->seed_fill_unselect_all_triangles();
                selector->request_update_render_data();
            }
        }
    }
    if (m_tool_type == ToolType::SMART_FILL) {
        if (ImGui::SliderFloat(_u8L("Fill angle").c_str(), &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, "%.0f"))
            for (auto& selector : m_triangle_selectors) {
                selector->seed_fill_unselect_all_triangles();
                selector->request_update_render_data();
            }
    } else if (m_cursor_type != TriangleSelector::POINTER)
        ImGui::SliderFloat(_u8L("Brush size").c_str(), &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f");

    float clipping = float(m_c->object_clipper()->get_position());
    if (ImGui::SliderFloat(_u8L("Section view").c_str(), &clipping, 0.f, 1.f, "%.2f"))
        m_c->object_clipper()->set_position_by_ratio(clipping, true);
    if (clipping > 0.f && m_imgui->button(_L("Reset direction")))
        m_c->object_clipper()->set_position_by_ratio(-1., false);
    m_imgui->text(_L("Left click: paint. Right click or Shift: erase."));
    if (m_imgui->button(_L("Erase all gloss painting"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Erase gloss painting"), UndoRedo::SnapshotType::GizmoAction);
        for (auto& selector : m_triangle_selectors) {
            selector->reset();
            selector->request_update_render_data(true);
        }
        update_model_object();
    }
    ImGui::PopItemWidth();
    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoGloss::update_model_object()
{
    ModelObject* object  = m_c->selection_info()->model_object();
    bool         updated = false;
    size_t       index   = 0;
    for (ModelVolume* volume : object->volumes)
        if (volume->is_model_part())
            updated |= volume->gloss_facets.set(*m_triangle_selectors[index++]);
    if (updated) {
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        m_parent.set_as_dirty();
    }
}

void GLGizmoGloss::update_from_model_object(bool first_update)
{
    const ModelObject* object = m_c->selection_info()->model_object();
    DynamicPrintConfig config = wxGetApp().preset_bundle->full_config();
    config.apply(object->config.get());
    const auto* times = config.option<ConfigOptionFloats>("gloss_paint_times");
    for (size_t color = 0; color < GLOSS_PAINT_COLORS; ++color)
        m_times[color] = float(times && color < times->values.size() && std::isfinite(times->values[color]) && times->values[color] > 0. ?
                                   times->values[color] :
                                   GLOSS_DEFAULT_TIMES[color]);
    m_triangle_selectors.clear();
    for (const ModelVolume* volume : object->volumes) {
        if (!volume->is_model_part())
            continue;
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(volume->mesh(), GLOSS_COLORS));
        m_triangle_selectors.back()->deserialize(volume->gloss_facets.get_data(), false);
        m_triangle_selectors.back()->request_update_render_data();
    }
}

wxString GLGizmoGloss::handle_snapshot_action_name(bool shift_down, Button button_down) const
{
    return shift_down || button_down == Button::Right ? _L("Erase gloss painting") : _L("Paint gloss color");
}

} // namespace Slic3r::GUI
