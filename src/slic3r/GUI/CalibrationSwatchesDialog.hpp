#ifndef slic3r_GUI_CalibrationSwatchesDialog_hpp_
#define slic3r_GUI_CalibrationSwatchesDialog_hpp_

#include "GUI_Utils.hpp"

#include "libslic3r/ColorCalibrationSwatches.hpp"

#include <wx/string.h>

class wxCheckBox;
class wxChoice;
class wxDirPickerCtrl;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r {
namespace GUI {

class Plater;

class CalibrationSwatchesDialog : public DPIDialog
{
public:
    CalibrationSwatchesDialog(wxWindow *parent, Plater *plater);

    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    using SwatchGeneratorConfig = ColorCalibrationSwatches::SwatchGeneratorConfig;

    SwatchGeneratorConfig build_config() const;
    void                  update_preview();
    void                  on_generate(wxCommandEvent &event);

    Plater *m_plater = nullptr;

    wxCheckBox *m_family_anchor     = nullptr;
    wxCheckBox *m_family_pair_mix   = nullptr;
    wxCheckBox *m_family_ternary    = nullptr;
    wxCheckBox *m_local_z_enabled   = nullptr;

    wxSpinCtrlDouble *m_chip_width      = nullptr;
    wxSpinCtrlDouble *m_chip_depth      = nullptr;
    wxSpinCtrlDouble *m_spacing         = nullptr;
    wxSpinCtrlDouble *m_strip_spacing   = nullptr;
    wxSpinCtrlDouble *m_anchor_thick    = nullptr;
    wxSpinCtrlDouble *m_plate_buffer    = nullptr;
    wxSpinCtrlDouble *m_prime_tower_width = nullptr;
    wxSpinCtrlDouble *m_prime_tower_depth = nullptr;
    wxSpinCtrlDouble *m_text_size       = nullptr;
    wxSpinCtrlDouble *m_text_depth      = nullptr;
    wxSpinCtrlDouble *m_text_rotation   = nullptr;
    wxSpinCtrlDouble *m_plate_label_size = nullptr;
    wxSpinCtrlDouble *m_plate_label_depth = nullptr;
    wxSpinCtrlDouble *m_plate_label_band = nullptr;
    wxSpinCtrlDouble *m_plate_label_width = nullptr;
    wxSpinCtrlDouble *m_plate_label_stroke_width = nullptr;
    wxSpinCtrlDouble *m_plate_label_margin = nullptr;
    wxSpinCtrlDouble *m_jig_diameter = nullptr;
    wxSpinCtrlDouble *m_jig_clearance = nullptr;
    wxSpinCtrlDouble *m_jig_ring_clearance = nullptr;
    wxSpinCtrlDouble *m_jig_wall_thickness = nullptr;
    wxSpinCtrlDouble *m_jig_wall_height = nullptr;
    wxSpinCtrlDouble *m_jig_wall_arc = nullptr;
    wxSpinCtrl       *m_max_chars       = nullptr;

    wxSpinCtrl *m_pair_layer_limit = nullptr;

    wxTextCtrl *m_separator         = nullptr;
    wxTextCtrl *m_plate_label_title = nullptr;

    wxChoice   *m_jig_filament = nullptr;
    wxCheckBox *m_plate_label_enabled = nullptr;
    wxCheckBox *m_prime_tower_reserve = nullptr;
    wxCheckBox *m_jig_enabled = nullptr;
    wxCheckBox *m_jig_wall_enabled = nullptr;
    wxCheckBox *m_wrap_text          = nullptr;
    wxCheckBox *m_include_type       = nullptr;
    wxCheckBox *m_include_top        = nullptr;
    wxCheckBox *m_include_backing    = nullptr;
    wxCheckBox *m_include_thickness  = nullptr;
    wxCheckBox *m_use_full_names     = nullptr;
    wxCheckBox *m_mirror_text        = nullptr;
    wxCheckBox *m_emboss_text        = nullptr;
    wxCheckBox *m_write_manifest     = nullptr;

    wxDirPickerCtrl *m_manifest_dir = nullptr;
    wxStaticText    *m_preview      = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_CalibrationSwatchesDialog_hpp_
