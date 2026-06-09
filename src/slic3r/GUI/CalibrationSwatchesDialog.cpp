#include "CalibrationSwatchesDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Widgets/Label.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filepicker.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace GUI {

namespace {

static std::string wx_to_u8(const wxString &value)
{
    return value.ToUTF8().data();
}

static wxSpinCtrlDouble *make_spin(wxWindow *parent, double min, double max, double value, double increment, int digits = 1)
{
    auto *spin = new wxSpinCtrlDouble(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, parent->FromDIP(wxSize(90, -1)));
    spin->SetRange(min, max);
    spin->SetIncrement(increment);
    spin->SetDigits(digits);
    spin->SetValue(value);
    return spin;
}

static void add_labeled_control(wxWindow *parent, wxFlexGridSizer *grid, const wxString &label, wxWindow *control)
{
    auto *text = new wxStaticText(parent, wxID_ANY, label);
    grid->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, parent->FromDIP(6));
    grid->Add(control, 0, wxEXPAND | wxBOTTOM, parent->FromDIP(6));
}

static std::vector<ColorCalibrationSwatches::FilamentSlot> current_filaments()
{
    std::vector<ColorCalibrationSwatches::FilamentSlot> filaments;
    const PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return filaments;

    const DynamicPrintConfig &project_config = preset_bundle->project_config;
    const ConfigOptionStrings *color_opt = project_config.option<ConfigOptionStrings>("filament_colour");
    std::vector<std::string> colors = color_opt ? color_opt->values : std::vector<std::string>();

    const size_t filament_count = static_cast<size_t>(std::max(wxGetApp().filaments_cnt(), 0));
    colors.resize(filament_count, "#26A69A");

    filaments.reserve(filament_count);
    for (size_t i = 0; i < filament_count; ++i) {
        ColorCalibrationSwatches::FilamentSlot slot;
        slot.slot        = static_cast<unsigned int>(i + 1);
        slot.name        = i < preset_bundle->filament_presets.size() ?
                               preset_bundle->filament_presets[i] :
                               std::string("Filament ") + std::to_string(i + 1);
        slot.short_label = std::to_string(i + 1);
        slot.color_hex   = colors[i];
        filaments.emplace_back(std::move(slot));
    }
    return filaments;
}

static wxString default_manifest_dir()
{
    wxString dir = wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Documents);
    if (dir.empty())
        dir = wxGetHomeDir();
    return dir;
}

static wxChoice *make_filament_choice(wxWindow *parent, const std::vector<ColorCalibrationSwatches::FilamentSlot> &filaments)
{
    auto *choice = new wxChoice(parent, wxID_ANY);
    for (const ColorCalibrationSwatches::FilamentSlot &filament : filaments) {
        wxString label = wxString::Format("%u", filament.slot);
        if (!filament.color_hex.empty())
            label += " " + wxString::FromUTF8(filament.color_hex.c_str());
        if (!filament.name.empty())
            label += " " + wxString::FromUTF8(filament.name.c_str());
        choice->Append(label);
    }
    if (!filaments.empty())
        choice->SetSelection(0);
    return choice;
}

static Vec2d current_bed_size_mm(const Plater *plater)
{
    if (plater != nullptr) {
        const BoundingBoxf bed_box = plater->build_volume().bounding_volume2d();
        const Vec2d       size = bed_box.size();
        if (std::isfinite(size.x()) && std::isfinite(size.y()) && size.x() > 0.0 && size.y() > 0.0)
            return size;
    }

    const PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    const ConfigOptionPoints *area = preset_bundle != nullptr ?
        preset_bundle->project_config.option<ConfigOptionPoints>("printable_area") :
        nullptr;
    if (area != nullptr && !area->values.empty()) {
        Vec2d min = area->values.front();
        Vec2d max = area->values.front();
        for (const Vec2d &point : area->values) {
            min = min.cwiseMin(point);
            max = max.cwiseMax(point);
        }

        const Vec2d size = max - min;
        if (size.x() > 0.0 && size.y() > 0.0)
            return size;
    }

    return Vec2d(220.0, 220.0);
}

static double current_layer_height_mm()
{
    const PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle != nullptr) {
        DynamicPrintConfig full_config = preset_bundle->full_config();
        if (full_config.has("layer_height")) {
            const double layer_height = full_config.get_abs_value("layer_height");
            if (std::isfinite(layer_height) && layer_height > 0.0)
                return layer_height;
        }
    }

    return 0.2;
}

static bool current_local_z_enabled()
{
    const PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return false;

    if (const ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>("dithering_local_z_mode"))
        return opt->value;

    DynamicPrintConfig full_config = preset_bundle->full_config();
    if (const ConfigOptionBool *opt = full_config.option<ConfigOptionBool>("dithering_local_z_mode"))
        return opt->value;

    return false;
}

} // namespace

CalibrationSwatchesDialog::CalibrationSwatchesDialog(wxWindow *parent, Plater *plater)
    : DPIDialog(parent, wxID_ANY, _L("Calibration swatches"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_plater(plater)
{
    SetFont(Label::Body_14);
    const std::vector<ColorCalibrationSwatches::FilamentSlot> dialog_filaments = current_filaments();

    auto *root = new wxBoxSizer(wxVERTICAL);
    SetSizer(root);

    auto *settings_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxTAB_TRAVERSAL);
    settings_scroll->SetScrollRate(0, FromDIP(16));
    auto *settings_sizer = new wxBoxSizer(wxVERTICAL);
    settings_scroll->SetSizer(settings_sizer);
    root->Add(settings_scroll, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto *families_box = new wxStaticBoxSizer(wxVERTICAL, settings_scroll, _L("Swatches"));
    auto *families_grid = new wxFlexGridSizer(2, FromDIP(12), FromDIP(16));
    m_family_anchor     = new wxCheckBox(settings_scroll, wxID_ANY, _L("Anchor chips"));
    m_family_pair_mix   = new wxCheckBox(settings_scroll, wxID_ANY, _L("Pair mixes"));
    m_family_ternary    = new wxCheckBox(settings_scroll, wxID_ANY, _L("Ternary mixes"));
    m_family_anchor->SetValue(true);
    m_family_pair_mix->SetValue(true);
    m_family_ternary->SetValue(false);
    families_grid->Add(m_family_anchor, 0, wxBOTTOM, FromDIP(4));
    families_grid->Add(m_family_pair_mix, 0, wxBOTTOM, FromDIP(4));
    families_grid->Add(m_family_ternary, 0, wxBOTTOM, FromDIP(4));
    families_box->Add(families_grid, 0, wxALL, FromDIP(8));
    settings_sizer->Add(families_box, 0, wxEXPAND);

    auto *layout_box = new wxStaticBoxSizer(wxVERTICAL, settings_scroll, _L("Layout"));
    auto *layout_grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    layout_grid->AddGrowableCol(1);
    const ColorCalibrationSwatches::SwatchLayoutOptions default_layout;
    m_chip_width   = make_spin(settings_scroll, 5.0, 80.0, 20.0, 1.0);
    m_chip_depth   = make_spin(settings_scroll, 5.0, 80.0, 20.0, 1.0);
    m_spacing      = make_spin(settings_scroll, 0.0, 30.0, 4.0, 0.5);
    m_strip_spacing = make_spin(settings_scroll, 0.0, 30.0, 2.0, 0.25, 2);
    m_anchor_thick = make_spin(settings_scroll, 0.2, 20.0, 6.0, 0.2);
    m_plate_buffer = make_spin(settings_scroll, 0.0, 30.0, 8.0, 0.5, 1);
    m_prime_tower_reserve = new wxCheckBox(settings_scroll, wxID_ANY, _L("Reserve prime tower"));
    m_prime_tower_reserve->SetValue(default_layout.reserve_prime_tower);
    m_prime_tower_width = make_spin(settings_scroll, 0.0, 160.0, default_layout.prime_tower_width_mm, 1.0, 1);
    m_prime_tower_depth = make_spin(settings_scroll, 0.0, 160.0, default_layout.prime_tower_depth_mm, 1.0, 1);
    add_labeled_control(settings_scroll, layout_grid, _L("Swatch width"), m_chip_width);
    add_labeled_control(settings_scroll, layout_grid, _L("Face height"), m_chip_depth);
    add_labeled_control(settings_scroll, layout_grid, _L("Column spacing"), m_spacing);
    add_labeled_control(settings_scroll, layout_grid, _L("Strip spacing"), m_strip_spacing);
    add_labeled_control(settings_scroll, layout_grid, _L("Swatch depth"), m_anchor_thick);
    add_labeled_control(settings_scroll, layout_grid, _L("Plate buffer"), m_plate_buffer);
    layout_grid->Add(m_prime_tower_reserve, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    layout_grid->AddSpacer(0);
    add_labeled_control(settings_scroll, layout_grid, _L("Prime tower width"), m_prime_tower_width);
    add_labeled_control(settings_scroll, layout_grid, _L("Prime tower depth"), m_prime_tower_depth);
    layout_box->Add(layout_grid, 0, wxEXPAND | wxALL, FromDIP(8));
    settings_sizer->Add(layout_box, 0, wxEXPAND | wxTOP, FromDIP(12));

    auto *values_box = new wxStaticBoxSizer(wxVERTICAL, settings_scroll, _L("Pair proportions"));
    auto *values_grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    values_grid->AddGrowableCol(1);
    m_pair_layer_limit = new wxSpinCtrl(settings_scroll,
                                        wxID_ANY,
                                        wxEmptyString,
                                        wxDefaultPosition,
                                        FromDIP(wxSize(90, -1)),
                                        wxSP_ARROW_KEYS,
                                        1,
                                        20,
                                        7);
    m_local_z_enabled = new wxCheckBox(settings_scroll, wxID_ANY, _L("Local Z enabled"));
    m_local_z_enabled->SetValue(current_local_z_enabled());
    add_labeled_control(settings_scroll, values_grid, _L("Max 1-to-many ratio"), m_pair_layer_limit);
    values_grid->Add(m_local_z_enabled, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    values_grid->AddSpacer(0);
    values_box->Add(values_grid, 0, wxEXPAND | wxALL, FromDIP(8));
    settings_sizer->Add(values_box, 0, wxEXPAND | wxTOP, FromDIP(12));

    auto *plate_label_box = new wxStaticBoxSizer(wxVERTICAL, settings_scroll, _L("Plate label"));
    auto *plate_label_grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    plate_label_grid->AddGrowableCol(1);
    const ColorCalibrationSwatches::PlateLabelOptions default_plate_label;
    m_plate_label_enabled = new wxCheckBox(settings_scroll, wxID_ANY, _L("Add plate label"));
    m_plate_label_enabled->SetValue(true);
    m_plate_label_title = new wxTextCtrl(settings_scroll,
                                         wxID_ANY,
                                         wxString::FromUTF8(default_plate_label.title.c_str()),
                                         wxDefaultPosition,
                                         FromDIP(wxSize(420, -1)));
    m_plate_label_size = make_spin(settings_scroll, 2.0, 20.0, default_plate_label.text_size_mm, 0.25, 2);
    m_plate_label_depth = make_spin(settings_scroll, 0.05, 2.0, default_plate_label.text_depth_mm, 0.05, 2);
    m_plate_label_band = make_spin(settings_scroll, 0.0, 80.0, default_plate_label.reserved_height_mm, 1.0, 1);
    m_plate_label_width = make_spin(settings_scroll, 20.0, 200.0, default_plate_label.reserved_width_mm, 1.0, 1);
    m_plate_label_stroke_width = make_spin(settings_scroll, 0.0, 2.0, default_plate_label.stroke_width_mm, 0.05, 2);
    m_plate_label_stroke_width->SetToolTip(_L("Additional stroke thickening for the plate label text. Use 0 for the font's normal stroke."));
    m_plate_label_margin = make_spin(settings_scroll, 0.0, 40.0, default_plate_label.margin_x_mm, 1.0, 1);
    plate_label_grid->Add(m_plate_label_enabled, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    plate_label_grid->AddSpacer(0);
    add_labeled_control(settings_scroll, plate_label_grid, _L("Title"), m_plate_label_title);
    add_labeled_control(settings_scroll, plate_label_grid, _L("Text size"), m_plate_label_size);
    add_labeled_control(settings_scroll, plate_label_grid, _L("Text depth"), m_plate_label_depth);
    add_labeled_control(settings_scroll, plate_label_grid, _L("Reserved height"), m_plate_label_band);
    add_labeled_control(settings_scroll, plate_label_grid, _L("Reserved width"), m_plate_label_width);
    add_labeled_control(settings_scroll, plate_label_grid, _L("Text stroke width"), m_plate_label_stroke_width);
    add_labeled_control(settings_scroll, plate_label_grid, _L("Side margin"), m_plate_label_margin);
    plate_label_box->Add(plate_label_grid, 0, wxEXPAND | wxALL, FromDIP(8));
    settings_sizer->Add(plate_label_box, 0, wxEXPAND | wxTOP, FromDIP(12));

    auto *jig_box = new wxStaticBoxSizer(wxVERTICAL, settings_scroll, _L("Spectro jig"));
    auto *jig_grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    jig_grid->AddGrowableCol(1);
    const ColorCalibrationSwatches::SpectroJigOptions default_jig;
    m_jig_enabled = new wxCheckBox(settings_scroll, wxID_ANY, _L("Generate spectro jig"));
    m_jig_enabled->SetValue(true);
    m_jig_diameter = make_spin(settings_scroll, 20.0, 100.0, default_jig.diameter_mm, 0.5, 1);
    m_jig_clearance = make_spin(settings_scroll, 0.0, 10.0, default_jig.clearance_mm, 0.25, 2);
    m_jig_ring_clearance = make_spin(settings_scroll, 0.0, 5.0, default_jig.ring_clearance_mm, 0.1, 2);
    m_jig_wall_enabled = new wxCheckBox(settings_scroll, wxID_ANY, wxEmptyString);
    m_jig_wall_enabled->SetValue(default_jig.wall_enabled);
    m_jig_wall_thickness = make_spin(settings_scroll, 0.0, 12.0, default_jig.wall_thickness_mm, 0.25, 2);
    m_jig_wall_height = make_spin(settings_scroll, 0.0, 30.0, default_jig.wall_height_mm, 0.5, 1);
    m_jig_wall_arc = make_spin(settings_scroll, 15.0, 360.0, default_jig.wall_arc_degrees, 5.0, 1);
    m_jig_filament = make_filament_choice(settings_scroll, dialog_filaments);
    jig_grid->Add(m_jig_enabled, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    jig_grid->AddSpacer(0);
    add_labeled_control(settings_scroll, jig_grid, _L("Diameter"), m_jig_diameter);
    add_labeled_control(settings_scroll, jig_grid, _L("Ring clearance"), m_jig_ring_clearance);
    add_labeled_control(settings_scroll, jig_grid, _L("Cutout clearance"), m_jig_clearance);
    add_labeled_control(settings_scroll, jig_grid, _L("Locator wall"), m_jig_wall_enabled);
    add_labeled_control(settings_scroll, jig_grid, _L("Wall thickness"), m_jig_wall_thickness);
    add_labeled_control(settings_scroll, jig_grid, _L("Wall height"), m_jig_wall_height);
    add_labeled_control(settings_scroll, jig_grid, _L("Wall arc"), m_jig_wall_arc);
    add_labeled_control(settings_scroll, jig_grid, _L("Filament"), m_jig_filament);
    jig_box->Add(jig_grid, 0, wxEXPAND | wxALL, FromDIP(8));
    settings_sizer->Add(jig_box, 0, wxEXPAND | wxTOP, FromDIP(12));

    auto *text_box = new wxStaticBoxSizer(wxVERTICAL, settings_scroll, _L("Back text"));
    auto *text_grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    text_grid->AddGrowableCol(1);
    const ColorCalibrationSwatches::BackTextFormatOptions default_back_text;
    m_separator     = new wxTextCtrl(settings_scroll, wxID_ANY, "_", wxDefaultPosition, FromDIP(wxSize(90, -1)));
    m_max_chars = new wxSpinCtrl(settings_scroll,
                                 wxID_ANY,
                                 wxEmptyString,
                                 wxDefaultPosition,
                                 FromDIP(wxSize(90, -1)),
                                 wxSP_ARROW_KEYS,
                                 3,
                                 40,
                                 12);
    m_text_size     = make_spin(settings_scroll, 1.0, 16.0, default_back_text.text_size_mm, 0.1, 1);
    m_text_depth    = make_spin(settings_scroll, 0.05, 2.0, default_back_text.text_depth_mm, 0.05, 2);
    m_text_rotation = make_spin(settings_scroll, -180.0, 180.0, 0.0, 5.0, 1);
    add_labeled_control(settings_scroll, text_grid, _L("Separator"), m_separator);
    add_labeled_control(settings_scroll, text_grid, _L("Max chars per line"), m_max_chars);
    add_labeled_control(settings_scroll, text_grid, _L("Text size"), m_text_size);
    add_labeled_control(settings_scroll, text_grid, _L("Text depth"), m_text_depth);
    add_labeled_control(settings_scroll, text_grid, _L("Orientation"), m_text_rotation);
    text_box->Add(text_grid, 0, wxEXPAND | wxALL, FromDIP(8));

    auto *text_flags = new wxFlexGridSizer(2, FromDIP(6), FromDIP(16));
    m_wrap_text         = new wxCheckBox(settings_scroll, wxID_ANY, _L("Wrap lines"));
    m_include_type      = new wxCheckBox(settings_scroll, wxID_ANY, _L("Type prefix"));
    m_include_top       = new wxCheckBox(settings_scroll, wxID_ANY, _L("Top material"));
    m_include_backing   = new wxCheckBox(settings_scroll, wxID_ANY, _L("Backing"));
    m_include_thickness = new wxCheckBox(settings_scroll, wxID_ANY, _L("Thickness"));
    m_use_full_names    = new wxCheckBox(settings_scroll, wxID_ANY, _L("Full names"));
    m_mirror_text       = new wxCheckBox(settings_scroll, wxID_ANY, _L("Mirror"));
    m_emboss_text       = new wxCheckBox(settings_scroll, wxID_ANY, _L("Emboss"));
    m_wrap_text->SetValue(true);
    m_include_type->SetValue(false);
    m_include_top->SetValue(true);
    m_include_backing->SetValue(true);
    m_include_thickness->SetValue(false);
    m_use_full_names->SetValue(false);
    m_mirror_text->SetValue(true);
    m_emboss_text->SetValue(false);
    text_flags->Add(m_wrap_text, 0, wxBOTTOM, FromDIP(4));
    text_flags->Add(m_include_type, 0, wxBOTTOM, FromDIP(4));
    text_flags->Add(m_include_top, 0, wxBOTTOM, FromDIP(4));
    text_flags->Add(m_include_backing, 0, wxBOTTOM, FromDIP(4));
    text_flags->Add(m_include_thickness, 0, wxBOTTOM, FromDIP(4));
    text_flags->Add(m_use_full_names, 0, wxBOTTOM, FromDIP(4));
    text_flags->Add(m_mirror_text, 0, wxBOTTOM, FromDIP(4));
    text_flags->Add(m_emboss_text, 0, wxBOTTOM, FromDIP(4));
    text_box->Add(text_flags, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    settings_sizer->Add(text_box, 0, wxEXPAND | wxTOP, FromDIP(12));

    auto *manifest_box = new wxStaticBoxSizer(wxVERTICAL, settings_scroll, _L("Manifest"));
    m_write_manifest = new wxCheckBox(settings_scroll, wxID_ANY, _L("Write JSON and CSV"));
    m_write_manifest->SetValue(true);
    m_manifest_dir = new wxDirPickerCtrl(settings_scroll,
                                         wxID_ANY,
                                         default_manifest_dir(),
                                         _L("Select manifest folder"),
                                         wxDefaultPosition,
                                         FromDIP(wxSize(420, -1)));
    manifest_box->Add(m_write_manifest, 0, wxALL, FromDIP(8));
    manifest_box->Add(m_manifest_dir, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    settings_sizer->Add(manifest_box, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));
    settings_scroll->FitInside();

    m_preview = new wxStaticText(this, wxID_ANY, wxEmptyString);
    root->Add(m_preview, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto *button_sizer = new wxBoxSizer(wxHORIZONTAL);
    button_sizer->AddStretchSpacer();
    auto *cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    auto *generate = new wxButton(this, wxID_OK, _L("Generate"));
    button_sizer->Add(cancel, 0, wxRIGHT, FromDIP(8));
    button_sizer->Add(generate, 0);
    root->Add(button_sizer, 0, wxEXPAND | wxALL, FromDIP(12));

    const auto bind_preview = [this](wxEvent &) { update_preview(); };
    for (wxWindow *control : { static_cast<wxWindow*>(m_family_anchor),
                               static_cast<wxWindow*>(m_family_pair_mix),
                               static_cast<wxWindow*>(m_family_ternary),
                               static_cast<wxWindow*>(m_chip_width),
                               static_cast<wxWindow*>(m_chip_depth),
                               static_cast<wxWindow*>(m_spacing),
                               static_cast<wxWindow*>(m_strip_spacing),
                               static_cast<wxWindow*>(m_anchor_thick),
                               static_cast<wxWindow*>(m_plate_buffer),
                               static_cast<wxWindow*>(m_prime_tower_reserve),
                               static_cast<wxWindow*>(m_prime_tower_width),
                               static_cast<wxWindow*>(m_prime_tower_depth),
                               static_cast<wxWindow*>(m_pair_layer_limit),
                               static_cast<wxWindow*>(m_local_z_enabled),
                               static_cast<wxWindow*>(m_plate_label_enabled),
                               static_cast<wxWindow*>(m_plate_label_title),
                               static_cast<wxWindow*>(m_plate_label_size),
                               static_cast<wxWindow*>(m_plate_label_depth),
                               static_cast<wxWindow*>(m_plate_label_band),
                               static_cast<wxWindow*>(m_plate_label_width),
                               static_cast<wxWindow*>(m_plate_label_stroke_width),
                               static_cast<wxWindow*>(m_plate_label_margin),
                               static_cast<wxWindow*>(m_jig_enabled),
                               static_cast<wxWindow*>(m_jig_diameter),
                               static_cast<wxWindow*>(m_jig_clearance),
                               static_cast<wxWindow*>(m_jig_ring_clearance),
                               static_cast<wxWindow*>(m_jig_wall_enabled),
                               static_cast<wxWindow*>(m_jig_wall_thickness),
                               static_cast<wxWindow*>(m_jig_wall_height),
                               static_cast<wxWindow*>(m_jig_wall_arc),
                               static_cast<wxWindow*>(m_jig_filament),
                               static_cast<wxWindow*>(m_separator),
                               static_cast<wxWindow*>(m_max_chars),
                               static_cast<wxWindow*>(m_text_size),
                               static_cast<wxWindow*>(m_text_depth),
                               static_cast<wxWindow*>(m_text_rotation),
                               static_cast<wxWindow*>(m_wrap_text),
                               static_cast<wxWindow*>(m_include_type),
                               static_cast<wxWindow*>(m_include_top),
                               static_cast<wxWindow*>(m_include_backing),
                               static_cast<wxWindow*>(m_include_thickness),
                               static_cast<wxWindow*>(m_use_full_names),
                               static_cast<wxWindow*>(m_mirror_text),
                               static_cast<wxWindow*>(m_emboss_text) }) {
        control->Bind(wxEVT_CHECKBOX, bind_preview);
        control->Bind(wxEVT_CHOICE, bind_preview);
        control->Bind(wxEVT_TEXT, bind_preview);
        control->Bind(wxEVT_SPINCTRL, bind_preview);
        control->Bind(wxEVT_SPINCTRLDOUBLE, bind_preview);
    }
    generate->Bind(wxEVT_BUTTON, &CalibrationSwatchesDialog::on_generate, this);

    update_preview();
    SetMinSize(FromDIP(wxSize(560, 520)));
    SetSize(FromDIP(wxSize(640, 620)));
    Layout();
}

void CalibrationSwatchesDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    SetSize(suggested_rect);
    Layout();
}

CalibrationSwatchesDialog::SwatchGeneratorConfig CalibrationSwatchesDialog::build_config() const
{
    SwatchGeneratorConfig config;
    config.filaments = current_filaments();

    config.families.reflective_anchor = m_family_anchor->GetValue();
    config.families.td_ladder         = false;
    config.families.pair_mix          = m_family_pair_mix->GetValue();
    config.families.pair_order        = false;
    config.families.ternary_mix       = m_family_ternary->GetValue();
    config.families.layer_line_strip  = false;

    const Vec2d bed_size = current_bed_size_mm(m_plater);
    const double plate_buffer = std::max(0.0, m_plate_buffer->GetValue());
    const double swatch_depth = std::max(0.2, m_anchor_thick->GetValue());
    const double layer_height = std::max(0.01, current_layer_height_mm());

    config.layout.chip_width_mm  = m_chip_width->GetValue();
    config.layout.chip_depth_mm  = m_chip_depth->GetValue();
    config.layout.spacing_x_mm   = m_spacing->GetValue();
    config.layout.spacing_y_mm   = m_strip_spacing->GetValue();
    config.layout.footprint_depth_mm = swatch_depth;
    config.layout.margin_x_mm    = plate_buffer;
    config.layout.margin_y_mm    = plate_buffer;
    config.layout.plate_width_mm = std::max(50.0, bed_size.x());
    config.layout.plate_depth_mm = std::max(50.0, bed_size.y());
    config.layout.reserve_prime_tower = m_prime_tower_reserve->GetValue();
    config.layout.prime_tower_width_mm = m_prime_tower_width->GetValue();
    config.layout.prime_tower_depth_mm = m_prime_tower_depth->GetValue();
    config.layout.multi_plate    = true;
    config.nominal_layer_height_mm = layer_height;
    config.local_z_enabled = m_local_z_enabled->GetValue();
    config.plate_label.enabled   = m_plate_label_enabled->GetValue();
    config.plate_label.title     = wx_to_u8(m_plate_label_title->GetValue());
    config.plate_label.text_size_mm = m_plate_label_size->GetValue();
    config.plate_label.text_depth_mm = m_plate_label_depth->GetValue();
    config.plate_label.reserved_height_mm = m_plate_label_band->GetValue();
    config.plate_label.reserved_width_mm = m_plate_label_width->GetValue();
    config.plate_label.stroke_width_mm = m_plate_label_stroke_width->GetValue();
    config.plate_label.margin_x_mm = m_plate_label_margin->GetValue();
    config.spectro_jig.enabled = m_jig_enabled->GetValue();
    config.spectro_jig.diameter_mm = m_jig_diameter->GetValue();
    config.spectro_jig.thickness_mm = swatch_depth;
    config.spectro_jig.clearance_mm = m_jig_clearance->GetValue();
    config.spectro_jig.ring_clearance_mm = m_jig_ring_clearance->GetValue();
    config.spectro_jig.wall_enabled = m_jig_wall_enabled->GetValue();
    config.spectro_jig.wall_thickness_mm = m_jig_wall_thickness->GetValue();
    config.spectro_jig.wall_height_mm = m_jig_wall_height->GetValue();
    config.spectro_jig.wall_arc_degrees = m_jig_wall_arc->GetValue();
    const int jig_selection = m_jig_filament != nullptr ? m_jig_filament->GetSelection() : wxNOT_FOUND;
    if (jig_selection >= 0 && static_cast<size_t>(jig_selection) < config.filaments.size())
        config.spectro_jig.filament_slot = config.filaments[static_cast<size_t>(jig_selection)].slot;

    config.anchor_thickness_mm    = swatch_depth;
    config.pair_mix_thickness_mm  = swatch_depth;
    config.pair_order_thickness_mm = swatch_depth;
    config.ternary_thickness_mm   = swatch_depth;
    config.pair_mix_layer_height_mm = layer_height;
    config.pair_order_layer_height_mm = layer_height;
    config.ternary_layer_height_mm = layer_height;
    config.layer_line_strip_layer_height_mm = layer_height;
    config.td_ladder_thicknesses = { config.anchor_thickness_mm };
    config.pair_ratio_layer_limit = static_cast<unsigned int>(std::max(m_pair_layer_limit->GetValue(), 1));

    std::string separator = wx_to_u8(m_separator->GetValue());
    if (separator.empty())
        separator = "_";

    config.id_format.separator                    = separator;
    config.id_format.include_swatch_type_prefix   = m_include_type->GetValue();
    config.id_format.include_top_material         = m_include_top->GetValue();
    config.id_format.include_backing              = m_include_backing->GetValue();
    config.id_format.include_thickness            = m_include_thickness->GetValue();

    config.back_text_format.separator                 = separator;
    config.back_text_format.enabled                   = true;
    config.back_text_format.wrap_lines                = m_wrap_text->GetValue();
    config.back_text_format.max_chars_per_line        = static_cast<size_t>(std::max(m_max_chars->GetValue(), 1));
    config.back_text_format.include_swatch_type_prefix = m_include_type->GetValue();
    config.back_text_format.include_top_material      = m_include_top->GetValue();
    config.back_text_format.include_backing           = m_include_backing->GetValue();
    config.back_text_format.include_thickness         = m_include_thickness->GetValue();
    config.back_text_format.use_full_filament_names   = m_use_full_names->GetValue();
    config.back_text_format.mirror                    = m_mirror_text->GetValue();
    config.back_text_format.embossed                  = m_emboss_text->GetValue();
    config.back_text_format.text_size_mm              = m_text_size->GetValue();
    config.back_text_format.text_depth_mm             = m_text_depth->GetValue();
    config.back_text_format.rotation_degrees          = m_text_rotation->GetValue();
    return config;
}

void CalibrationSwatchesDialog::update_preview()
{
    const SwatchGeneratorConfig config = build_config();
    const auto plan = ColorCalibrationSwatches::generate_swatch_plan(config);
    const auto issues = ColorCalibrationSwatches::validate_swatch_plan(plan, config);

    size_t error_count = 0;
    for (const auto &issue : issues)
        if (issue.severity == ColorCalibrationSwatches::ValidationSeverity::Error)
            ++error_count;

    size_t plate_count = 0;
    for (const auto &record : plan.records)
        plate_count = std::max<size_t>(plate_count, record.position.plate_index + 1);

    wxString text;
    text.Printf(_L("%zu swatches%s on %zu plate(s). Bed %.0fx%.0f mm, buffer %.1f mm. Layer %.2f mm. Local Z %s. %zu validation error(s)."),
                plan.records.size(),
                config.spectro_jig.enabled ? _L(" plus jig") : _L(""),
                plate_count,
                config.layout.plate_width_mm,
                config.layout.plate_depth_mm,
                config.layout.margin_x_mm,
                config.nominal_layer_height_mm,
                config.local_z_enabled ? _L("on") : _L("off"),
                error_count);
    m_preview->SetLabel(text);
}

void CalibrationSwatchesDialog::on_generate(wxCommandEvent &)
{
    const SwatchGeneratorConfig config = build_config();
    if (config.filaments.empty()) {
        MessageDialog(this,
                      _L("No physical filaments are available for swatch generation."),
                      _L("Calibration swatches"),
                      wxOK | wxICON_WARNING).ShowModal();
        return;
    }

    const auto plan = ColorCalibrationSwatches::generate_swatch_plan(config);
    const auto issues = ColorCalibrationSwatches::validate_swatch_plan(plan, config);
    for (const auto &issue : issues) {
        if (issue.severity == ColorCalibrationSwatches::ValidationSeverity::Error) {
            MessageDialog(this, wxString::FromUTF8(issue.message.c_str()), _L("Calibration swatches"), wxOK | wxICON_WARNING).ShowModal();
            return;
        }
    }

    const std::string manifest_dir = m_write_manifest->GetValue() ? wx_to_u8(m_manifest_dir->GetPath()) : std::string();
    if (m_plater != nullptr)
        m_plater->generate_calibration_swatches(config, manifest_dir);
    EndModal(wxID_OK);
}

} // namespace GUI
} // namespace Slic3r
