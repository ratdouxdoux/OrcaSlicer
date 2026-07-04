#ifndef slic3r_GUI_CalibrationSwatchesDialog_hpp_
#define slic3r_GUI_CalibrationSwatchesDialog_hpp_

#include "GUI_Utils.hpp"

#include "libslic3r/ColorCalibrationSwatches.hpp"

#include <wx/string.h>

#include <vector>

class wxCheckBox;
class wxChoice;
class wxButton;
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
    bool                  apply_filament_tds(std::vector<ColorCalibrationSwatches::FilamentSlot> &filaments,
                                             wxString *error = nullptr) const;
    void                  update_preview();
    void                  on_open_ratio_file(wxCommandEvent &event);
    void                  on_clear_ratio_file(wxCommandEvent &event);
    void                  on_generate(wxCommandEvent &event);

    Plater *m_plater = nullptr;

    wxCheckBox *m_family_anchor     = nullptr;
    wxCheckBox *m_family_td_ladder  = nullptr;
    wxCheckBox *m_family_pair_mix   = nullptr;
    wxCheckBox *m_family_ternary    = nullptr;
    wxCheckBox *m_family_quaternary = nullptr;
    wxCheckBox *m_local_z_enabled   = nullptr;
    wxCheckBox *m_direct_multicolor_solver = nullptr;

    wxSpinCtrlDouble *m_chip_width      = nullptr;
    wxSpinCtrlDouble *m_chip_depth      = nullptr;
    wxSpinCtrlDouble *m_spacing         = nullptr;
    wxSpinCtrlDouble *m_strip_spacing   = nullptr;
    wxSpinCtrlDouble *m_anchor_thick    = nullptr;
    wxSpinCtrlDouble *m_plate_buffer    = nullptr;
    wxSpinCtrlDouble *m_prime_tower_width = nullptr;
    wxSpinCtrlDouble *m_prime_tower_depth = nullptr;
    wxSpinCtrlDouble *m_reference_text_size = nullptr;
    wxSpinCtrlDouble *m_reference_text_depth = nullptr;
    wxSpinCtrlDouble *m_reference_text_stroke_width = nullptr;
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

    wxSpinCtrl *m_pair_layer_limit = nullptr;
    wxSpinCtrl *m_quaternary_layer_limit = nullptr;

    wxButton     *m_ratio_file_open   = nullptr;
    wxButton     *m_ratio_file_clear  = nullptr;
    wxStaticText *m_ratio_file_status = nullptr;
    wxString      m_ratio_file_path;
    std::vector<std::vector<int>> m_ratio_file_rows;

    wxTextCtrl *m_plate_reference   = nullptr;
    wxTextCtrl *m_plate_label_title = nullptr;
    wxTextCtrl *m_td_ladder_widths  = nullptr;
    std::vector<wxTextCtrl*> m_filament_td_inputs;
    std::vector<unsigned int> m_filament_td_slots;

    wxChoice   *m_jig_filament = nullptr;
    wxCheckBox *m_plate_label_enabled = nullptr;
    wxCheckBox *m_prime_tower_reserve = nullptr;
    wxCheckBox *m_jig_enabled = nullptr;
    wxCheckBox *m_jig_wall_enabled = nullptr;
    wxCheckBox *m_write_manifest     = nullptr;

    wxDirPickerCtrl *m_manifest_dir = nullptr;
    wxStaticText    *m_preview      = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_CalibrationSwatchesDialog_hpp_
