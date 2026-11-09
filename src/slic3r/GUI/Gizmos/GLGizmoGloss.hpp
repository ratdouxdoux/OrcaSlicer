#ifndef slic3r_GLGizmoGloss_hpp_
#define slic3r_GLGizmoGloss_hpp_

#include "GLGizmoPainterBase.hpp"
#include "libslic3r/GlossPainting.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r::GUI {

class GLGizmoGloss : public GLGizmoPainterBase
{
public:
    GLGizmoGloss(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    void render_painter_gizmo() override;

protected:
    bool                on_init() override;
    std::string         on_get_name() const override { return _u8L("Gloss painting"); }
    void                on_render_input_window(float x, float y, float bottom_limit) override;
    void                update_model_object() override;
    void                update_from_model_object(bool first_update) override;
    void                on_opening() override {}
    void                on_shutdown() override;
    PainterGizmoType    get_painter_type() const override { return PainterGizmoType::GLOSS; }
    EnforcerBlockerType get_left_button_state_type() const override { return static_cast<EnforcerBlockerType>(m_color + 1); }
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType::NONE; }
    wxString            handle_snapshot_action_name(bool shift_down, Button button_down) const override;
    std::string         get_gizmo_entering_text() const override { return _u8L("Entering gloss painting"); }
    std::string         get_gizmo_leaving_text() const override { return _u8L("Leaving gloss painting"); }
    std::string         get_action_snapshot_name() const override { return _u8L("Gloss painting"); }

private:
    GlossSpeedRange                       estimated_speed_range() const;
    void                                  save_times();
    size_t                                m_color = 0;
    std::array<float, GLOSS_PAINT_COLORS> m_times{1.f, 2.f, 4.f, 8.f};
};

} // namespace Slic3r::GUI
#endif
