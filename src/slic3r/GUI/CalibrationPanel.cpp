#include <wx/dcgraph.h>
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "CalibrationPanel.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "SelectMachine.hpp"
#include "SelectMachinePop.hpp"

#include <nlohmann/json.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clrpicker.h>
#include <wx/collpane.h>
#include <wx/datetime.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/grid.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <regex>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Slic3r { namespace GUI {

#define REFRESH_INTERVAL       1000

#define INITIAL_NUMBER_OF_MACHINES 0
#define LIST_REFRESH_INTERVAL 200
#define MACHINE_LIST_REFRESH_INTERVAL 2000
wxDEFINE_EVENT(EVT_FINISHED_UPDATE_MLIST, wxCommandEvent);
wxDEFINE_EVENT(EVT_UPDATE_USER_MLIST, wxCommandEvent);

namespace {

static std::string wx_to_u8(const wxString &value)
{
    return value.ToUTF8().data();
}

static wxString u8_to_wx(const std::string &value)
{
    return wxString::FromUTF8(value.c_str());
}

static wxString json_scalar_to_text(const nlohmann::json &value)
{
    if (value.is_string())
        return u8_to_wx(value.get<std::string>());
    if (value.is_number_integer())
        return wxString::Format("%lld", value.get<long long>());
    if (value.is_number_unsigned())
        return wxString::Format("%llu", value.get<unsigned long long>());
    if (value.is_number_float())
        return wxString::Format("%.3f", value.get<double>());
    if (value.is_boolean())
        return value.get<bool>() ? _L("true") : _L("false");
    if (value.is_null())
        return wxString();
    return u8_to_wx(value.dump());
}

static wxString json_array_to_text(const nlohmann::json &record, const char *key)
{
    if (!record.contains(key))
        return wxString();

    const nlohmann::json &value = record[key];
    if (!value.is_array())
        return json_scalar_to_text(value);

    wxString out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i > 0)
            out += "_";
        out += json_scalar_to_text(value[i]);
    }
    return out;
}

static wxString json_string_value(const nlohmann::json &record, const char *key)
{
    if (!record.contains(key))
        return wxString();
    return json_scalar_to_text(record[key]);
}

static wxString manifest_display_id(const nlohmann::json &record)
{
    const wxString printed_reference = json_string_value(record, "printed_reference");
    return printed_reference.empty() ? json_string_value(record, "swatch_id") : printed_reference;
}

static std::optional<std::string> normalize_hex_color(std::string value)
{
    value.erase(std::remove_if(value.begin(), value.end(), [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch)) != 0;
    }), value.end());
    if (!value.empty() && value.front() != '#')
        value.insert(value.begin(), '#');
    if (value.size() != 7 && value.size() != 9)
        return std::nullopt;

    std::string out = "#";
    for (size_t i = 1; i < 7; ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (!std::isxdigit(ch))
            return std::nullopt;
        out.push_back(char(std::toupper(ch)));
    }
    for (size_t i = 7; i < value.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (!std::isxdigit(ch))
            return std::nullopt;
    }
    return out;
}

static wxColour colour_from_hex(const std::string &value)
{
    const std::optional<std::string> normalized = normalize_hex_color(value);
    wxColour colour(normalized ? u8_to_wx(*normalized) : wxString("#808080"));
    return colour.IsOk() ? colour : wxColour(128, 128, 128);
}

static std::string colour_to_hex(const wxColour &colour)
{
    return wx_to_u8(wxString::Format("#%02X%02X%02X", colour.Red(), colour.Green(), colour.Blue()));
}

static std::optional<unsigned int> json_slot_value(const nlohmann::json &value)
{
    if (value.is_number_unsigned())
        return value.get<unsigned int>();
    if (value.is_number_integer()) {
        const int slot = value.get<int>();
        return slot > 0 ? std::optional<unsigned int>(static_cast<unsigned int>(slot)) : std::nullopt;
    }
    if (value.is_string()) {
        try {
            const unsigned long slot = std::stoul(value.get<std::string>());
            if (slot > 0 && slot <= std::numeric_limits<unsigned int>::max())
                return static_cast<unsigned int>(slot);
        } catch (...) {
        }
    }
    return std::nullopt;
}

static std::optional<double> json_double_value(const nlohmann::json &value)
{
    if (value.is_number())
        return value.get<double>();
    if (value.is_string()) {
        try {
            size_t parsed = 0;
            const double number = std::stod(value.get<std::string>(), &parsed);
            if (parsed == value.get<std::string>().size() && std::isfinite(number))
                return number;
        } catch (...) {
        }
    }
    return std::nullopt;
}

static wxString optional_td_to_text(const std::optional<double> &value)
{
    if (!value)
        return wxString();
    return wxString::Format("%.3f", *value);
}

static bool parse_double_cell(const wxString &value, double &out)
{
    wxString trimmed = value;
    trimmed.Trim(true).Trim(false);
    if (trimmed.empty())
        return false;
    return trimmed.ToDouble(&out);
}

class ColorSwatchMeasurementPage : public wxPanel
{
public:
    explicit ColorSwatchMeasurementPage(wxWindow *parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundColour(*wxWHITE);

        auto *root = new wxBoxSizer(wxVERTICAL);
        SetSizer(root);

        auto *toolbar = new wxBoxSizer(wxHORIZONTAL);
        auto *load_manifest = new wxButton(this, wxID_ANY, _L("Load manifest"));
        auto *import_values = new wxButton(this, wxID_ANY, _L("Import measurements"));
        auto *save_values = new wxButton(this, wxID_ANY, _L("Save measurements"));
        auto *clear_values = new wxButton(this, wxID_ANY, _L("Clear values"));
        toolbar->Add(load_manifest, 0, wxRIGHT, FromDIP(8));
        toolbar->Add(import_values, 0, wxRIGHT, FromDIP(8));
        toolbar->Add(save_values, 0, wxRIGHT, FromDIP(8));
        toolbar->Add(clear_values, 0, wxRIGHT, FromDIP(8));
        root->Add(toolbar, 0, wxEXPAND | wxALL, FromDIP(12));

        auto *metadata_pane = new wxCollapsiblePane(this, wxID_ANY, _L("Manifest and primaries"));
        wxWindow *metadata_parent = metadata_pane->GetPane();
        auto *meta_grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
        meta_grid->AddGrowableCol(1);
        metadata_parent->SetSizer(meta_grid);
        m_manifest_path = new wxTextCtrl(metadata_parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        m_instrument    = new wxTextCtrl(metadata_parent, wxID_ANY, "3nh CR9");
        m_illuminant    = new wxTextCtrl(metadata_parent, wxID_ANY, "D65");
        m_observer      = new wxTextCtrl(metadata_parent, wxID_ANY, "10");
        m_geometry      = new wxTextCtrl(metadata_parent, wxID_ANY, _L("side d/8"));
        m_illuminant->SetToolTip(_L("Reference light used for Lab conversion."));
        m_observer->SetToolTip(_L("CIE standard observer angle for Lab conversion, usually 2 or 10. Use Geometry for d/8."));
        m_geometry->SetToolTip(_L("Measurement geometry and sample face. CR9 reflective measurements use d/8 geometry."));
        add_meta_row(meta_grid, metadata_parent, _L("Manifest"), m_manifest_path);
        add_meta_row(meta_grid, metadata_parent, _L("Instrument"), m_instrument);
        add_meta_row(meta_grid, metadata_parent, _L("Illuminant"), m_illuminant);
        add_meta_row(meta_grid, metadata_parent, _L("CIE observer"), m_observer);
        add_meta_row(meta_grid, metadata_parent, _L("Geometry"), m_geometry);

        auto *primary_colors_row = new wxBoxSizer(wxHORIZONTAL);
        for (size_t i = 0; i < m_primary_color_pickers.size(); ++i) {
            primary_colors_row->Add(new wxStaticText(metadata_parent, wxID_ANY, wxString::Format("%zu", i + 1)),
                                    0,
                                    wxALIGN_CENTER_VERTICAL | wxRIGHT,
                                    FromDIP(4));
            m_primary_color_pickers[i] = new wxColourPickerCtrl(metadata_parent,
                                                                 wxID_ANY,
                                                                 wxColour(128, 128, 128),
                                                                 wxDefaultPosition,
                                                                 FromDIP(wxSize(72, -1)));
            m_primary_color_pickers[i]->Enable(false);
            primary_colors_row->Add(m_primary_color_pickers[i], 0, wxRIGHT, FromDIP(10));
        }
        m_primary_color_status = new wxStaticText(metadata_parent, wxID_ANY, _L("Load manifest"));
        primary_colors_row->Add(m_primary_color_status, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));
        add_meta_row(meta_grid, metadata_parent, _L("Primary colors"), primary_colors_row);

        auto *primary_td_row = new wxBoxSizer(wxHORIZONTAL);
        for (size_t i = 0; i < m_primary_td_inputs.size(); ++i) {
            primary_td_row->Add(new wxStaticText(metadata_parent, wxID_ANY, wxString::Format("%zu", i + 1)),
                                0,
                                wxALIGN_CENTER_VERTICAL | wxRIGHT,
                                FromDIP(4));
            m_primary_td_inputs[i] = new wxTextCtrl(metadata_parent,
                                                    wxID_ANY,
                                                    wxEmptyString,
                                                    wxDefaultPosition,
                                                    FromDIP(wxSize(72, -1)),
                                                    wxTE_PROCESS_ENTER);
            m_primary_td_inputs[i]->Enable(false);
            m_primary_td_inputs[i]->SetToolTip(_L("Transmission distance for this source filament. Leave empty if unknown."));
            primary_td_row->Add(m_primary_td_inputs[i], 0, wxRIGHT, FromDIP(10));
        }
        m_primary_td_status = new wxStaticText(metadata_parent, wxID_ANY, _L("Load manifest"));
        primary_td_row->Add(m_primary_td_status, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));
        add_meta_row(meta_grid, metadata_parent, _L("Primary TD"), primary_td_row);
        root->Add(metadata_pane, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        auto *measure_box = new wxBoxSizer(wxVERTICAL);
        m_next_sample = new wxStaticText(this, wxID_ANY, _L("Next sample: load a manifest"));
        measure_box->Add(m_next_sample, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        auto *capture_grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
        capture_grid->AddGrowableCol(1);
        m_target_takes = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(90, -1)), wxSP_ARROW_KEYS, 1, 99, 3);
        m_pass_delta_e = new wxSpinCtrlDouble(this,
                                             wxID_ANY,
                                             wxEmptyString,
                                             wxDefaultPosition,
                                             FromDIP(wxSize(90, -1)),
                                             wxSP_ARROW_KEYS,
                                             0.0,
                                             100.0,
                                             2.0,
                                             0.1);
        m_pass_delta_e->SetDigits(2);
        m_auto_advance = new wxCheckBox(this, wxID_ANY, _L("Auto advance"));
        m_auto_advance->SetValue(true);
        add_meta_row(capture_grid, _L("Samples per swatch"), m_target_takes);
        add_meta_row(capture_grid, _L("Pass dE*ab"), m_pass_delta_e);
        add_meta_row(capture_grid, wxString(), m_auto_advance);

        auto *white_backing_row = new wxBoxSizer(wxHORIZONTAL);
        m_reference_white_backing = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(180, -1)));
        auto *measure_white_backing = new wxButton(this, wxID_ANY, _L("Measure"));
        white_backing_row->Add(m_reference_white_backing, 0, wxRIGHT, FromDIP(8));
        white_backing_row->Add(measure_white_backing, 0);
        add_meta_row(capture_grid, _L("White backing"), white_backing_row);

        auto *black_backing_row = new wxBoxSizer(wxHORIZONTAL);
        m_reference_black_backing = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(180, -1)));
        auto *measure_black_backing = new wxButton(this, wxID_ANY, _L("Measure"));
        black_backing_row->Add(m_reference_black_backing, 0, wxRIGHT, FromDIP(8));
        black_backing_row->Add(measure_black_backing, 0);
        add_meta_row(capture_grid, _L("Black backing"), black_backing_row);

        auto *sample_row = new wxBoxSizer(wxHORIZONTAL);
        auto *previous_sample = new wxButton(this, wxID_ANY, _L("Previous sample"));
        auto *remove_last_take = new wxButton(this, wxID_ANY, _L("Remove last take"));
        auto *clear_sample = new wxButton(this, wxID_ANY, _L("Clear sample"));
        auto *clear_current_and_measure = new wxButton(this, wxID_ANY, _L("Clear current sample and measure all"));
        auto *clear_previous_and_measure = new wxButton(this, wxID_ANY, _L("Clear previous sample and measure all"));
        sample_row->Add(previous_sample, 0, wxRIGHT, FromDIP(8));
        sample_row->Add(remove_last_take, 0, wxRIGHT, FromDIP(8));
        sample_row->Add(clear_sample, 0, wxRIGHT, FromDIP(8));
        sample_row->Add(clear_current_and_measure, 0, wxRIGHT, FromDIP(8));
        sample_row->Add(clear_previous_and_measure, 0);
        add_meta_row(capture_grid, _L("Sample"), sample_row);

        auto *manual_row = new wxBoxSizer(wxHORIZONTAL);
        m_manual_l = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(72, -1)));
        m_manual_a = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(72, -1)));
        m_manual_b = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(72, -1)));
        auto *add_take = new wxButton(this, wxID_ANY, _L("Add take"));
        manual_row->Add(new wxStaticText(this, wxID_ANY, _L("L*")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        manual_row->Add(m_manual_l, 0, wxRIGHT, FromDIP(8));
        manual_row->Add(new wxStaticText(this, wxID_ANY, _L("a*")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        manual_row->Add(m_manual_a, 0, wxRIGHT, FromDIP(8));
        manual_row->Add(new wxStaticText(this, wxID_ANY, _L("b*")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        manual_row->Add(m_manual_b, 0, wxRIGHT, FromDIP(8));
        manual_row->Add(add_take, 0);
        add_meta_row(capture_grid, _L("Manual Lab"), manual_row);

        auto *serial_row = new wxBoxSizer(wxHORIZONTAL);
        m_port = new wxTextCtrl(this, wxID_ANY, "COM3", wxDefaultPosition, FromDIP(wxSize(80, -1)));
        m_baud = new wxTextCtrl(this, wxID_ANY, "1135104", wxDefaultPosition, FromDIP(wxSize(90, -1)));
        m_connect_serial = new wxButton(this, wxID_ANY, _L("Connect"));
        m_measure_serial = new wxButton(this, wxID_ANY, _L("Measure"));
        m_measure_serial->Enable(false);
        m_measure_all_serial = new wxButton(this, wxID_ANY, _L("Measure All"));
        m_measure_all_serial->Enable(false);
        serial_row->Add(m_port, 0, wxRIGHT, FromDIP(8));
        serial_row->Add(m_baud, 0, wxRIGHT, FromDIP(8));
        serial_row->Add(m_connect_serial, 0, wxRIGHT, FromDIP(8));
        serial_row->Add(m_measure_serial, 0, wxRIGHT, FromDIP(8));
        serial_row->Add(m_measure_all_serial, 0, wxRIGHT, FromDIP(8));
        m_serial_status = new wxStaticText(this, wxID_ANY, _L("Disconnected"));
        serial_row->Add(m_serial_status, 0, wxALIGN_CENTER_VERTICAL);
        add_meta_row(capture_grid, _L("Spectro"), serial_row);

        measure_box->Add(capture_grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        auto *protocol_pane = new wxCollapsiblePane(this, wxID_ANY, _L("Protocol log"));
        wxWindow *protocol_parent = protocol_pane->GetPane();
        auto *protocol_grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
        protocol_grid->AddGrowableCol(1);
        protocol_parent->SetSizer(protocol_grid);
        m_serial_log = new wxTextCtrl(protocol_parent,
                                      wxID_ANY,
                                      wxEmptyString,
                                      wxDefaultPosition,
                                      FromDIP(wxSize(-1, 64)),
                                      wxTE_MULTILINE | wxTE_READONLY);
        add_meta_row(protocol_grid, protocol_parent, _L("Protocol"), m_serial_log);
        measure_box->Add(protocol_pane, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        root->Add(measure_box, 0, wxEXPAND);

        auto relayout_after_collapse = [this](wxCollapsiblePaneEvent &event) {
            Layout();
            event.Skip();
        };
        metadata_pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, relayout_after_collapse);
        protocol_pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, relayout_after_collapse);
        metadata_pane->Collapse(true);
        protocol_pane->Collapse(true);

        m_summary = new wxStaticText(this, wxID_ANY, _L("No manifest loaded"));
        root->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        m_grid = new wxGrid(this, wxID_ANY);
        m_grid->CreateGrid(0, ColCount);
        m_grid->EnableEditing(true);
        m_grid->EnableDragGridSize(false);
        m_grid->SetRowLabelSize(FromDIP(42));
        m_grid->SetColLabelValue(ColId, _L("ID"));
        m_grid->SetColLabelValue(ColType, _L("Type"));
        m_grid->SetColLabelValue(ColSlots, _L("Slots"));
        m_grid->SetColLabelValue(ColRatios, _L("Ratios"));
        m_grid->SetColLabelValue(ColPercentages, _L("%"));
        m_grid->SetColLabelValue(ColColors, _L("Colors"));
        m_grid->SetColLabelValue(ColTd, _L("TD"));
        m_grid->SetColLabelValue(ColBacking, _L("Backing"));
        m_grid->SetColLabelValue(ColMeasurement, _L("Measure"));
        m_grid->SetColLabelValue(ColThickness, _L("mm"));
        m_grid->SetColLabelValue(ColPlate, _L("Plate"));
        m_grid->SetColLabelValue(ColReadings, _L("Readings"));
        m_grid->SetColLabelValue(ColTakes, _L("Takes"));
        m_grid->SetColLabelValue(ColL, _L("Avg L*"));
        m_grid->SetColLabelValue(ColA, _L("Avg a*"));
        m_grid->SetColLabelValue(ColB, _L("Avg b*"));
        m_grid->SetColLabelValue(ColHex, _L("Hex"));
        m_grid->SetColLabelValue(ColNotes, _L("Notes"));
        m_grid->SetColLabelValue(ColSim, _L("Color Simulation"));
        m_grid->SetColLabelValue(ColDateTime, _L("Date Time"));
        m_grid->SetColLabelValue(ColIllObs, _L("Ill/Obs"));
        m_grid->SetColLabelValue(ColDeltaL, _L("dL*"));
        m_grid->SetColLabelValue(ColDeltaA, _L("da*"));
        m_grid->SetColLabelValue(ColDeltaB, _L("db*"));
        m_grid->SetColLabelValue(ColOffset, _L("Color Offset"));
        m_grid->SetColLabelValue(ColDeltaE, _L("dE*ab"));
        m_grid->SetColLabelValue(ColJudgement, _L("Judgement"));
        m_grid->SetColSize(ColId, FromDIP(110));
        m_grid->SetColSize(ColType, FromDIP(110));
        m_grid->SetColSize(ColSlots, FromDIP(80));
        m_grid->SetColSize(ColRatios, FromDIP(90));
        m_grid->SetColSize(ColPercentages, FromDIP(95));
        m_grid->SetColSize(ColColors, FromDIP(130));
        m_grid->SetColSize(ColTd, FromDIP(80));
        m_grid->SetColSize(ColBacking, FromDIP(90));
        m_grid->SetColSize(ColMeasurement, FromDIP(105));
        m_grid->SetColSize(ColThickness, FromDIP(70));
        m_grid->SetColSize(ColPlate, FromDIP(70));
        m_grid->SetColSize(ColReadings, FromDIP(260));
        m_grid->SetColSize(ColTakes, FromDIP(60));
        m_grid->SetColSize(ColL, FromDIP(80));
        m_grid->SetColSize(ColA, FromDIP(80));
        m_grid->SetColSize(ColB, FromDIP(80));
        m_grid->SetColSize(ColHex, FromDIP(90));
        m_grid->SetColSize(ColNotes, FromDIP(180));
        m_grid->SetColSize(ColSim, FromDIP(120));
        m_grid->SetColSize(ColDateTime, FromDIP(145));
        m_grid->SetColSize(ColIllObs, FromDIP(70));
        m_grid->SetColSize(ColDeltaL, FromDIP(70));
        m_grid->SetColSize(ColDeltaA, FromDIP(70));
        m_grid->SetColSize(ColDeltaB, FromDIP(70));
        m_grid->SetColSize(ColOffset, FromDIP(155));
        m_grid->SetColSize(ColDeltaE, FromDIP(80));
        m_grid->SetColSize(ColJudgement, FromDIP(85));
        root->Add(m_grid, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        m_serial_timer = new wxTimer(this);

        load_manifest->Bind(wxEVT_BUTTON, &ColorSwatchMeasurementPage::on_load_manifest, this);
        import_values->Bind(wxEVT_BUTTON, &ColorSwatchMeasurementPage::on_import_measurements, this);
        save_values->Bind(wxEVT_BUTTON, &ColorSwatchMeasurementPage::on_save_measurements, this);
        clear_values->Bind(wxEVT_BUTTON, &ColorSwatchMeasurementPage::on_clear_values, this);
        add_take->Bind(wxEVT_BUTTON, &ColorSwatchMeasurementPage::on_add_manual_take, this);
        previous_sample->Bind(wxEVT_BUTTON, &ColorSwatchMeasurementPage::on_previous_sample, this);
        remove_last_take->Bind(wxEVT_BUTTON, &ColorSwatchMeasurementPage::on_remove_last_take, this);
        clear_sample->Bind(wxEVT_BUTTON, &ColorSwatchMeasurementPage::on_clear_current_sample, this);
        clear_current_and_measure->Bind(wxEVT_BUTTON, &ColorSwatchMeasurementPage::on_clear_current_sample_and_measure_all, this);
        clear_previous_and_measure->Bind(wxEVT_BUTTON, &ColorSwatchMeasurementPage::on_clear_previous_sample_and_measure_all, this);
        m_connect_serial->Bind(wxEVT_BUTTON, &ColorSwatchMeasurementPage::on_toggle_serial, this);
        m_measure_serial->Bind(wxEVT_BUTTON, &ColorSwatchMeasurementPage::on_measure_serial, this);
        m_measure_all_serial->Bind(wxEVT_BUTTON, &ColorSwatchMeasurementPage::on_measure_all_serial, this);
        measure_white_backing->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_measure_reference(ReferenceMeasurementTarget::WhiteBacking); });
        measure_black_backing->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_measure_reference(ReferenceMeasurementTarget::BlackBacking); });
        Bind(wxEVT_TIMER, &ColorSwatchMeasurementPage::on_serial_timer, this, m_serial_timer->GetId());
        for (size_t i = 0; i < m_primary_color_pickers.size(); ++i) {
            if (m_primary_color_pickers[i] == nullptr)
                continue;
            const unsigned int slot = static_cast<unsigned int>(i + 1);
            m_primary_color_pickers[i]->Bind(wxEVT_COLOURPICKER_CHANGED, [this, slot](wxColourPickerEvent &event) {
                on_primary_color_changed(slot);
                event.Skip();
            });
        }
        for (size_t i = 0; i < m_primary_td_inputs.size(); ++i) {
            if (m_primary_td_inputs[i] == nullptr)
                continue;
            const unsigned int slot = static_cast<unsigned int>(i + 1);
            m_primary_td_inputs[i]->Bind(wxEVT_TEXT, [this, slot](wxCommandEvent &event) {
                on_primary_td_changed(slot);
                event.Skip();
            });
        }
        m_target_takes->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) {
            update_summary();
            update_next_sample();
        });
        m_pass_delta_e->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent &) {
            refresh_measurement_grid();
            update_summary();
            update_next_sample();
        });
        m_grid->Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent &event) {
            if (m_refreshing_grid) {
                event.Skip();
                return;
            }
            const std::optional<size_t> swatch = swatch_for_grid_row(event.GetRow());
            if (swatch && event.GetCol() == ColReadings) {
                sync_row_takes_from_grid(static_cast<int>(*swatch));
                refresh_measurement_grid();
            } else if (swatch && event.GetCol() == ColHex) {
                m_rows[*swatch].rgb_hex = wxString(m_grid->GetCellValue(event.GetRow(), ColHex)).Trim(true).Trim(false);
            } else if (swatch && event.GetCol() == ColNotes) {
                m_rows[*swatch].notes = m_grid->GetCellValue(event.GetRow(), ColNotes);
            }
            update_summary();
            update_next_sample();
            event.Skip();
        });
        m_grid->Bind(wxEVT_GRID_SELECT_CELL, [this](wxGridEvent &event) {
            if (const std::optional<size_t> swatch = swatch_for_grid_row(event.GetRow())) {
                m_current_swatch = static_cast<int>(*swatch);
                update_grid_row_visibility();
            }
            update_next_sample();
            event.Skip();
        });
    }

    ~ColorSwatchMeasurementPage() override
    {
        disconnect_serial();
    }

private:
    enum Column
    {
        ColId,
        ColType,
        ColSlots,
        ColRatios,
        ColPercentages,
        ColColors,
        ColTd,
        ColBacking,
        ColMeasurement,
        ColThickness,
        ColPlate,
        ColReadings,
        ColTakes,
        ColL,
        ColA,
        ColB,
        ColHex,
        ColNotes,
        ColSim,
        ColDateTime,
        ColIllObs,
        ColDeltaL,
        ColDeltaA,
        ColDeltaB,
        ColOffset,
        ColDeltaE,
        ColJudgement,
        ColCount
    };

    struct LabReading
    {
        double l = 0.0;
        double a = 0.0;
        double b = 0.0;
    };

    struct SpectrumSamples
    {
        std::vector<uint16_t> raw_u16;
        std::vector<double> reflectance;
    };

    struct RawSpectrumData
    {
        uint8_t operation = 0;
        uint8_t data_type = 0;
        uint8_t illuminant = 0;
        uint8_t observer = 1;
        std::optional<SpectrumSamples> sci;
        std::optional<SpectrumSamples> sce;
        std::vector<uint8_t> payload;
    };

    struct MeasurementTake
    {
        LabReading lab;
        std::optional<LabReading> sci_lab;
        std::optional<LabReading> sce_lab;
        wxString source;
        wxString timestamp;
        std::optional<RawSpectrumData> raw_spectrum;
    };

    struct PtsParsedMeasurement
    {
        LabReading lab;
        std::optional<LabReading> sci_lab;
        std::optional<LabReading> sce_lab;
        std::optional<RawSpectrumData> raw_spectrum;
    };

    struct Row
    {
        nlohmann::json manifest_record;
        size_t         source_manifest_index = 0;
        std::string    measurement_condition = "black_backing";
        std::vector<MeasurementTake> takes;
        wxString rgb_hex;
        wxString notes;
    };

    enum class GridRowKind
    {
        Swatch,
        Take
    };

    struct GridRowRef
    {
        GridRowKind kind = GridRowKind::Swatch;
        size_t swatch = 0;
        size_t take = 0;
    };

    struct FailedTake
    {
        size_t index = 0;
        double delta_e = 0.0;
    };

    enum class PtsProtocolState
    {
        Disconnected,
        Handshaking,
        Configuring,
        Ready,
        Measuring
    };

    enum class PtsSpecularMode
    {
        SCI,
        SCE
    };

    enum class ReferenceMeasurementTarget
    {
        None,
        WhiteBacking,
        BlackBacking
    };

    struct PtsCommand
    {
        uint8_t command = 0;
        std::vector<uint8_t> payload;
        wxString label;
    };

    void add_meta_row(wxFlexGridSizer *grid, wxWindow *label_parent, const wxString &label, wxWindow *control)
    {
        grid->Add(new wxStaticText(label_parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        grid->Add(control, 1, wxEXPAND);
    }

    void add_meta_row(wxFlexGridSizer *grid, wxWindow *label_parent, const wxString &label, wxSizer *sizer)
    {
        grid->Add(new wxStaticText(label_parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        grid->Add(sizer, 1, wxEXPAND);
    }

    void add_meta_row(wxFlexGridSizer *grid, const wxString &label, wxWindow *control)
    {
        add_meta_row(grid, this, label, control);
    }

    void add_meta_row(wxFlexGridSizer *grid, const wxString &label, wxSizer *sizer)
    {
        add_meta_row(grid, this, label, sizer);
    }

    static wxString format_reading(const LabReading &reading)
    {
        return wxString::Format("%.3f,%.3f,%.3f", reading.l, reading.a, reading.b);
    }

    static wxString format_readings(const std::vector<LabReading> &readings)
    {
        wxString out;
        for (size_t i = 0; i < readings.size(); ++i) {
            if (i > 0)
                out += "; ";
            out += format_reading(readings[i]);
        }
        return out;
    }

    static std::vector<LabReading> readings_from_takes(const std::vector<MeasurementTake> &takes)
    {
        std::vector<LabReading> readings;
        readings.reserve(takes.size());
        for (const MeasurementTake &take : takes)
            readings.push_back(take.lab);
        return readings;
    }

    static wxString current_timestamp()
    {
        return wxDateTime::Now().Format("%Y-%m-%d %H:%M:%S");
    }

    static MeasurementTake make_table_take(const LabReading &reading)
    {
        MeasurementTake take;
        take.lab = reading;
        take.source = _L("Table");
        take.timestamp = current_timestamp();
        return take;
    }

    static LabReading lab_delta(const LabReading &reading, const LabReading &reference)
    {
        return { reading.l - reference.l, reading.a - reference.a, reading.b - reference.b };
    }

    static double delta_e_ab(const LabReading &delta)
    {
        return std::sqrt(delta.l * delta.l + delta.a * delta.a + delta.b * delta.b);
    }

    static double srgb_channel_from_linear(double value)
    {
        value = std::clamp(value, 0.0, 1.0);
        if (value <= 0.0031308)
            return 12.92 * value;
        return 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    }

    static double lab_inverse_f(double value)
    {
        const double value3 = value * value * value;
        return value3 > 0.008856 ? value3 : (value - 16.0 / 116.0) / 7.787;
    }

    static wxColour lab_to_srgb_colour(const LabReading &lab)
    {
        const double y = (lab.l + 16.0) / 116.0;
        const double x = lab.a / 500.0 + y;
        const double z = y - lab.b / 200.0;

        const double X = 95.047 * lab_inverse_f(x) / 100.0;
        const double Y = 100.000 * lab_inverse_f(y) / 100.0;
        const double Z = 108.883 * lab_inverse_f(z) / 100.0;

        const double r =  3.2406 * X - 1.5372 * Y - 0.4986 * Z;
        const double g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
        const double b =  0.0557 * X - 0.2040 * Y + 1.0570 * Z;

        return wxColour(static_cast<unsigned char>(std::round(srgb_channel_from_linear(r) * 255.0)),
                        static_cast<unsigned char>(std::round(srgb_channel_from_linear(g) * 255.0)),
                        static_cast<unsigned char>(std::round(srgb_channel_from_linear(b) * 255.0)));
    }

    static wxColour readable_text_colour(const wxColour &background)
    {
        const double luminance = 0.2126 * background.Red() + 0.7152 * background.Green() + 0.0722 * background.Blue();
        return luminance < 128.0 ? *wxWHITE : *wxBLACK;
    }

    static wxString color_offset_text(const LabReading &delta)
    {
        std::vector<wxString> parts;
        constexpr double eps = 0.05;
        if (delta.l > eps)
            parts.push_back(_L("Light"));
        else if (delta.l < -eps)
            parts.push_back(_L("Dark"));
        if (delta.a > eps)
            parts.push_back(_L("Red"));
        else if (delta.a < -eps)
            parts.push_back(_L("Green"));
        if (delta.b > eps)
            parts.push_back(_L("Yellow"));
        else if (delta.b < -eps)
            parts.push_back(_L("Blue"));

        if (parts.empty())
            return _L("None");

        wxString out;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0)
                out += _L(", ");
            out += parts[i];
        }
        return out;
    }

    static bool lab_readings_match(const LabReading &lhs, const LabReading &rhs)
    {
        return std::abs(lhs.l - rhs.l) <= 0.001 &&
               std::abs(lhs.a - rhs.a) <= 0.001 &&
               std::abs(lhs.b - rhs.b) <= 0.001;
    }

    static std::vector<LabReading> parse_readings_text(const wxString &value)
    {
        std::string text = wx_to_u8(value);
        std::replace(text.begin(), text.end(), '\n', ';');
        std::replace(text.begin(), text.end(), '\r', ';');

        std::vector<LabReading> readings;
        const std::regex number_re(R"([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)");
        size_t start = 0;
        while (start <= text.size()) {
            const size_t end = text.find(';', start);
            const std::string chunk = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            std::vector<double> numbers;
            for (std::sregex_iterator it(chunk.begin(), chunk.end(), number_re), last; it != last; ++it) {
                try {
                    numbers.push_back(std::stod(it->str()));
                } catch (...) {
                }
            }
            if (numbers.size() >= 3)
                readings.push_back({ numbers[0], numbers[1], numbers[2] });
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return readings;
    }

    static std::optional<LabReading> average_reading(const std::vector<LabReading> &readings)
    {
        if (readings.empty())
            return std::nullopt;

        LabReading avg;
        for (const LabReading &reading : readings) {
            avg.l += reading.l;
            avg.a += reading.a;
            avg.b += reading.b;
        }
        const double count = double(readings.size());
        avg.l /= count;
        avg.a /= count;
        avg.b /= count;
        return avg;
    }

    static bool looks_like_lab_text(const std::string &text)
    {
        std::string lower = text;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        return lower.find("lab") != std::string::npos ||
               lower.find("l*") != std::string::npos ||
               (lower.find("l") != std::string::npos && lower.find("a") != std::string::npos && lower.find("b") != std::string::npos);
    }

    static std::optional<LabReading> parse_lab_reading_from_serial_text(const std::string &text)
    {
        if (!looks_like_lab_text(text))
            return std::nullopt;
        std::vector<LabReading> readings = parse_readings_text(u8_to_wx(text));
        if (readings.empty())
            return std::nullopt;
        return readings.front();
    }

    static void append_le16(std::vector<uint8_t> &bytes, uint16_t value)
    {
        bytes.push_back(uint8_t(value & 0xFF));
        bytes.push_back(uint8_t((value >> 8) & 0xFF));
    }

    static void append_le32(std::vector<uint8_t> &bytes, uint32_t value)
    {
        bytes.push_back(uint8_t(value & 0xFF));
        bytes.push_back(uint8_t((value >> 8) & 0xFF));
        bytes.push_back(uint8_t((value >> 16) & 0xFF));
        bytes.push_back(uint8_t((value >> 24) & 0xFF));
    }

    static uint16_t read_le16(const std::vector<uint8_t> &bytes, size_t offset)
    {
        if (offset + 1 >= bytes.size())
            return 0;
        return uint16_t(bytes[offset]) | (uint16_t(bytes[offset + 1]) << 8);
    }

    static uint32_t read_le32(const std::vector<uint8_t> &bytes, size_t offset)
    {
        if (offset + 3 >= bytes.size())
            return 0;
        return uint32_t(bytes[offset]) |
               (uint32_t(bytes[offset + 1]) << 8) |
               (uint32_t(bytes[offset + 2]) << 16) |
               (uint32_t(bytes[offset + 3]) << 24);
    }

    static float read_float_le(const std::vector<uint8_t> &bytes, size_t offset)
    {
        if (offset + sizeof(float) > bytes.size())
            return 0.0f;
        float value = 0.0f;
        std::memcpy(&value, bytes.data() + offset, sizeof(float));
        return value;
    }

    static uint16_t pts_crc(const std::vector<uint8_t> &bytes)
    {
        uint16_t crc = 0xFFFF;
        for (uint8_t byte : bytes) {
            crc ^= uint16_t(byte) << 8;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc & 0x8000) != 0 ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
        }
        return crc;
    }

    static wxString bytes_to_hex(const std::vector<uint8_t> &bytes, size_t max_bytes = 512)
    {
        wxString out;
        const size_t limit = std::min(bytes.size(), max_bytes);
        for (size_t i = 0; i < limit; ++i) {
            if (i > 0)
                out += " ";
            out += wxString::Format("%02X", unsigned(bytes[i]));
        }
        if (bytes.size() > limit)
            out += wxString::Format(" ... (%zu bytes)", bytes.size());
        return out;
    }

    static bool lab_reading_is_plausible(const LabReading &reading)
    {
        return reading.l >= -0.5 && reading.l <= 110.0 &&
               reading.a >= -180.0 && reading.a <= 180.0 &&
               reading.b >= -180.0 && reading.b <= 180.0;
    }

    static uint8_t pts_record_data_type(const std::vector<uint8_t> &data)
    {
        if (data.size() < 4)
            return 0xFF;
        return uint8_t((read_le32(data, 0) >> 11) & 0x03);
    }

    static bool pts_record_is_spectrum(const std::vector<uint8_t> &data)
    {
        return pts_record_data_type(data) == 0;
    }

    static bool pts_record_is_lab(const std::vector<uint8_t> &data)
    {
        return pts_record_data_type(data) == 1;
    }

    struct CieObserverSample
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    static const std::array<double, 31>& cie_d65_400_700_10nm()
    {
        static const std::array<double, 31> values = {
            82.7549, 91.4860, 93.4318, 86.6823, 104.8650, 117.0080, 117.8120, 114.8610,
            115.9230, 108.8110, 109.3540, 107.8020, 104.7900, 107.6890, 104.4050, 104.0460,
            100.0000, 96.3342, 95.7880, 88.6856, 90.0062, 89.5991, 87.6987, 83.2886,
            83.6992, 80.0268, 80.2146, 82.2778, 78.2842, 69.7213, 71.6091
        };
        return values;
    }

    static const std::array<CieObserverSample, 31>& cie_observer_2deg_400_700_10nm()
    {
        static const std::array<CieObserverSample, 31> values = {{
            {0.014310, 0.000396, 0.067850}, {0.043510, 0.001210, 0.207400},
            {0.134380, 0.004000, 0.645600}, {0.283900, 0.011600, 1.385600},
            {0.348280, 0.023000, 1.747060}, {0.336200, 0.038000, 1.772110},
            {0.290800, 0.060000, 1.669200}, {0.195360, 0.090980, 1.287640},
            {0.095640, 0.139020, 0.812950}, {0.032010, 0.208020, 0.465180},
            {0.004900, 0.323000, 0.272000}, {0.009300, 0.503000, 0.158200},
            {0.063270, 0.710000, 0.078250}, {0.165500, 0.862000, 0.042160},
            {0.290400, 0.954000, 0.020300}, {0.433450, 0.994950, 0.008750},
            {0.594500, 0.995000, 0.003900}, {0.762100, 0.952000, 0.002100},
            {0.916300, 0.870000, 0.001650}, {1.026300, 0.757000, 0.001100},
            {1.062200, 0.631000, 0.000800}, {1.002600, 0.503000, 0.000340},
            {0.854450, 0.381000, 0.000190}, {0.642400, 0.265000, 0.000050},
            {0.447900, 0.175000, 0.000020}, {0.283500, 0.107000, 0.000000},
            {0.164900, 0.061000, 0.000000}, {0.087400, 0.032000, 0.000000},
            {0.046770, 0.017000, 0.000000}, {0.022700, 0.008210, 0.000000},
            {0.011359, 0.004102, 0.000000}
        }};
        return values;
    }

    static const std::array<CieObserverSample, 31>& cie_observer_10deg_400_700_10nm()
    {
        static const std::array<CieObserverSample, 31> values = {{
            {0.019110, 0.002004, 0.086011}, {0.084736, 0.008756, 0.389366},
            {0.204492, 0.021391, 0.972542}, {0.314679, 0.038676, 1.553480},
            {0.383734, 0.062077, 1.967280}, {0.370702, 0.089456, 1.994800},
            {0.302273, 0.128201, 1.745370}, {0.195618, 0.185190, 1.317560},
            {0.080507, 0.253589, 0.772125}, {0.016172, 0.339133, 0.415254},
            {0.003816, 0.460777, 0.218502}, {0.037465, 0.606741, 0.112044},
            {0.117749, 0.761757, 0.060709}, {0.236491, 0.875211, 0.030451},
            {0.376772, 0.961988, 0.013676}, {0.529826, 0.991761, 0.003988},
            {0.705224, 0.997340, 0.000000}, {0.878655, 0.955552, 0.000000},
            {1.014160, 0.868934, 0.000000}, {1.118520, 0.777405, 0.000000},
            {1.124000, 0.658341, 0.000000}, {1.030480, 0.527963, 0.000000},
            {0.856297, 0.398057, 0.000000}, {0.647467, 0.283493, 0.000000},
            {0.431567, 0.179828, 0.000000}, {0.268329, 0.107633, 0.000000},
            {0.152568, 0.060281, 0.000000}, {0.081261, 0.031800, 0.000000},
            {0.040851, 0.015905, 0.000000}, {0.019941, 0.007749, 0.000000},
            {0.009577, 0.003718, 0.000000}
        }};
        return values;
    }

    static double lab_pivot_xyz(double value)
    {
        constexpr double delta = 6.0 / 29.0;
        constexpr double delta3 = delta * delta * delta;
        return value > delta3 ? std::cbrt(value) : value / (3.0 * delta * delta) + 4.0 / 29.0;
    }

    static std::optional<LabReading> lab_from_reflectance_spectrum(const std::array<double, 31> &spectrum, uint8_t illuminant, uint8_t observer)
    {
        if (illuminant != 0) // D65
            return std::nullopt;

        const std::array<double, 31> &d65 = cie_d65_400_700_10nm();
        const std::array<CieObserverSample, 31> &cmf = observer == 0 ? cie_observer_2deg_400_700_10nm() : cie_observer_10deg_400_700_10nm();

        double y_weight = 0.0;
        double xn_weight = 0.0;
        double zn_weight = 0.0;
        for (size_t i = 0; i < spectrum.size(); ++i) {
            y_weight += d65[i] * cmf[i].y;
            xn_weight += d65[i] * cmf[i].x;
            zn_weight += d65[i] * cmf[i].z;
        }
        if (y_weight <= 0.0 || xn_weight <= 0.0 || zn_weight <= 0.0)
            return std::nullopt;

        const double k = 100.0 / y_weight;
        const double white_x = k * xn_weight;
        const double white_y = 100.0;
        const double white_z = k * zn_weight;

        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        for (size_t i = 0; i < spectrum.size(); ++i) {
            const double reflectance = std::max(0.0, spectrum[i]);
            x += reflectance * d65[i] * cmf[i].x;
            y += reflectance * d65[i] * cmf[i].y;
            z += reflectance * d65[i] * cmf[i].z;
        }
        x *= k;
        y *= k;
        z *= k;

        const double fx = lab_pivot_xyz(x / white_x);
        const double fy = lab_pivot_xyz(y / white_y);
        const double fz = lab_pivot_xyz(z / white_z);
        LabReading reading { 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz) };
        return lab_reading_is_plausible(reading) ? std::optional<LabReading>(reading) : std::nullopt;
    }

    static std::optional<LabReading> lab_from_spectrum_samples(const SpectrumSamples &samples, uint8_t illuminant, uint8_t observer)
    {
        if (samples.reflectance.size() != 31)
            return std::nullopt;

        std::array<double, 31> spectrum {};
        std::copy(samples.reflectance.begin(), samples.reflectance.end(), spectrum.begin());
        return lab_from_reflectance_spectrum(spectrum, illuminant, observer);
    }

    static std::optional<PtsParsedMeasurement> parse_spectrum_from_pts_record(uint8_t operation, const std::vector<uint8_t> &data, size_t sci_offset, size_t sce_offset, uint8_t illuminant, uint8_t observer)
    {
        if (!pts_record_is_spectrum(data))
            return std::nullopt;

        const auto read_spectrum = [&data](size_t offset) -> std::optional<SpectrumSamples> {
            if (offset + 31 * sizeof(uint16_t) > data.size())
                return std::nullopt;

            SpectrumSamples samples;
            samples.raw_u16.reserve(31);
            samples.reflectance.reserve(31);
            bool has_signal = false;
            for (size_t i = 0; i < 31; ++i) {
                const uint16_t raw = read_le16(data, offset + i * sizeof(uint16_t));
                has_signal = has_signal || raw != 0;
                samples.raw_u16.push_back(raw);
                samples.reflectance.push_back(double(raw) * 0.0001);
            }
            return has_signal ? std::optional<SpectrumSamples>(samples) : std::nullopt;
        };

        RawSpectrumData raw;
        raw.operation = operation;
        raw.data_type = pts_record_data_type(data);
        raw.illuminant = illuminant;
        raw.observer = observer;
        raw.sci = read_spectrum(sci_offset);
        raw.sce = read_spectrum(sce_offset);
        raw.payload = data;

        std::optional<LabReading> sci_lab;
        std::optional<LabReading> sce_lab;
        if (raw.sci)
            sci_lab = lab_from_spectrum_samples(*raw.sci, illuminant, observer);
        if (raw.sce)
            sce_lab = lab_from_spectrum_samples(*raw.sce, illuminant, observer);

        if (sci_lab || sce_lab) {
            PtsParsedMeasurement parsed;
            parsed.lab = sci_lab ? *sci_lab : *sce_lab;
            parsed.sci_lab = sci_lab;
            parsed.sce_lab = sce_lab;
            parsed.raw_spectrum = raw;
            return parsed;
        }
        return std::nullopt;
    }

    static std::optional<PtsParsedMeasurement> parse_lab_from_pts_record(const std::vector<uint8_t> &data, size_t sci_offset, size_t sce_offset)
    {
        if (!pts_record_is_lab(data))
            return std::nullopt;

        // PTS color blocks store observer, illuminant, then three
        // little-endian float values. Keep both when the record has them;
        // SCI remains the primary Lab value for existing averages.
        const auto read_block = [&data](size_t offset) -> std::optional<LabReading> {
            if (offset + 14 > data.size())
                return std::nullopt;
            bool has_signal = false;
            for (size_t i = 0; i < 14; ++i)
                has_signal = has_signal || data[offset + i] != 0;
            if (!has_signal)
                return std::nullopt;
            LabReading reading { read_float_le(data, offset + 2),
                                 read_float_le(data, offset + 6),
                                 read_float_le(data, offset + 10) };
            return lab_reading_is_plausible(reading) ? std::optional<LabReading>(reading) : std::nullopt;
        };

        const std::optional<LabReading> sci = read_block(sci_offset);
        const std::optional<LabReading> sce = read_block(sce_offset);
        if (sci || sce) {
            PtsParsedMeasurement parsed;
            parsed.lab = sci ? *sci : *sce;
            parsed.sci_lab = sci;
            parsed.sce_lab = sce;
            return parsed;
        }
        return std::nullopt;
    }

    static std::optional<PtsParsedMeasurement> parse_lab_from_pts_measurement(uint8_t operation, const std::vector<uint8_t> &data, uint8_t illuminant, uint8_t observer)
    {
        if (operation == 0x23) {
            if (auto reading = parse_lab_from_pts_record(data, 25, 87)) // StandardRecord
                return reading;
            return parse_spectrum_from_pts_record(operation, data, 25, 87, illuminant, observer);
        }
        if (operation == 0x24) {
            if (auto reading = parse_lab_from_pts_record(data, 29, 91)) // TrialRecord
                return reading;
            return parse_spectrum_from_pts_record(operation, data, 29, 91, illuminant, observer);
        }
        return std::nullopt;
    }

    static std::vector<uint8_t> make_pts_data_frame(uint8_t session, uint8_t command, const std::vector<uint8_t> &payload)
    {
        std::vector<uint8_t> command_data;
        command_data.reserve(payload.size() + 1);
        command_data.push_back(command);
        command_data.insert(command_data.end(), payload.begin(), payload.end());

        std::vector<uint8_t> frame { 0x55, 0xAA, 0xA6, session };
        append_le32(frame, 0);
        append_le16(frame, uint16_t(command_data.size() + 2));
        frame.insert(frame.end(), command_data.begin(), command_data.end());
        append_le16(frame, pts_crc(command_data));
        return frame;
    }

    static bool chunk_is_mostly_text(const std::string &chunk)
    {
        if (chunk.empty())
            return false;
        size_t printable = 0;
        for (unsigned char ch : chunk)
            if (ch == '\r' || ch == '\n' || ch == '\t' || (ch >= 0x20 && ch < 0x7F))
                ++printable;
        return printable * 4 >= chunk.size() * 3;
    }

    static uint8_t pts_illuminant_code(const wxString &value)
    {
        wxString normalized = value;
        normalized.Trim(true).Trim(false).MakeUpper();
        if (normalized == "D50")
            return 1;
        if (normalized == "A")
            return 2;
        if (normalized == "C")
            return 3;
        if (normalized == "D55")
            return 4;
        if (normalized == "D75")
            return 5;
        if (normalized.StartsWith("F")) {
            long f = 0;
            if (normalized.Mid(1).ToLong(&f) && f >= 1 && f <= 12)
                return uint8_t(5 + f);
        }
        return 0; // D65
    }

    static uint8_t pts_observer_code(const wxString &value)
    {
        wxString normalized = value;
        normalized.Trim(true).Trim(false).MakeUpper();
        return normalized.StartsWith("2") || normalized.Contains("1931") ? 0 : 1;
    }

    static std::optional<LabReading> lab_from_json(const nlohmann::json &object)
    {
        if (!object.is_object())
            return std::nullopt;
        if (!object.contains("L") || !object.contains("a") || !object.contains("b"))
            return std::nullopt;
        if (!object["L"].is_number() || !object["a"].is_number() || !object["b"].is_number())
            return std::nullopt;
        return LabReading { object["L"].get<double>(), object["a"].get<double>(), object["b"].get<double>() };
    }

    static nlohmann::json lab_to_json(const LabReading &reading)
    {
        return { { "L", reading.l }, { "a", reading.a }, { "b", reading.b } };
    }

    static bool is_reflective_anchor_record(const nlohmann::json &record)
    {
        return record.contains("swatch_type") && record["swatch_type"].is_string() &&
               record["swatch_type"].get<std::string>() == "reflective_anchor";
    }

    static std::vector<std::string> measurement_conditions_for_record(const nlohmann::json &record)
    {
        if (!is_reflective_anchor_record(record))
            return { "black_backing" };
        return { "white_backing", "black_backing" };
    }

    static std::string default_measurement_condition_for_record(const nlohmann::json &record)
    {
        return is_reflective_anchor_record(record) ? std::string("white_backing") : std::string("black_backing");
    }

    static std::string canonical_measurement_condition(const std::string &condition, const std::string &fallback = "black_backing")
    {
        if (condition == "white_backing" || condition == "black_backing")
            return condition;
        return fallback;
    }

    static wxString measurement_condition_label(const std::string &condition)
    {
        if (canonical_measurement_condition(condition) == "black_backing")
            return _L("black backing");
        return _L("white backing");
    }

    static std::string measurement_condition_from_record(const nlohmann::json &record)
    {
        const std::string fallback = default_measurement_condition_for_record(record);
        if (record.contains("measurement_condition") && record["measurement_condition"].is_string())
            return canonical_measurement_condition(record["measurement_condition"].get<std::string>(), fallback);
        return fallback;
    }

    static nlohmann::json manifest_record_for_measurement_condition(const nlohmann::json &record, const std::string &condition)
    {
        nlohmann::json out = record;
        const std::string canonical_condition = canonical_measurement_condition(condition, default_measurement_condition_for_record(record));
        out["measurement_condition"] = canonical_condition;
        out["measurement_condition_label"] = wx_to_u8(measurement_condition_label(canonical_condition));
        if (record.contains("swatch_id"))
            out["physical_swatch_id"] = record["swatch_id"];
        if (record.contains("printed_reference"))
            out["physical_printed_reference"] = record["printed_reference"];
        return out;
    }

    static std::string measurement_record_key(const std::string &swatch_id, const std::string &condition)
    {
        return swatch_id + "\n" + condition;
    }

    static wxString measurement_type_text(const nlohmann::json &record)
    {
        const wxString type = json_string_value(record, "swatch_type");
        const std::string condition = measurement_condition_from_record(record);
        return type + _L(" ") + measurement_condition_label(condition);
    }

    static nlohmann::json reference_measurement_to_json(wxTextCtrl *control)
    {
        if (control == nullptr)
            return nullptr;
        wxString text = control->GetValue();
        text.Trim(true).Trim(false);
        if (text.empty())
            return nullptr;

        const std::vector<LabReading> readings = parse_readings_text(text);
        if (!readings.empty())
            return lab_to_json(readings.front());
        return wx_to_u8(text);
    }

    static wxString reference_measurement_from_json(const nlohmann::json &value)
    {
        if (const std::optional<LabReading> reading = lab_from_json(value))
            return format_reading(*reading);
        if (value.is_string())
            return u8_to_wx(value.get<std::string>());
        if (value.is_null())
            return wxString();
        return json_scalar_to_text(value);
    }

    static const char* specular_mode_name(PtsSpecularMode mode)
    {
        return mode == PtsSpecularMode::SCI ? "SCI" : "SCE";
    }

    static uint8_t pts_specular_mode_code(PtsSpecularMode mode)
    {
        return mode == PtsSpecularMode::SCI ? 0 : 1;
    }

    static wxString take_specular_mode_text(const MeasurementTake &take)
    {
        if (take.sci_lab && take.sce_lab)
            return _L("SCI+SCE");
        if (take.sci_lab)
            return _L("SCI");
        if (take.sce_lab)
            return _L("SCE");
        if (take.raw_spectrum && take.raw_spectrum->sci && take.raw_spectrum->sce)
            return _L("SCI+SCE");
        if (take.raw_spectrum && take.raw_spectrum->sci)
            return _L("SCI");
        if (take.raw_spectrum && take.raw_spectrum->sce)
            return _L("SCE");
        return wxString();
    }

    static bool parsed_has_sci(const PtsParsedMeasurement &parsed)
    {
        return parsed.sci_lab || (parsed.raw_spectrum && parsed.raw_spectrum->sci);
    }

    static bool parsed_has_sce(const PtsParsedMeasurement &parsed)
    {
        return parsed.sce_lab || (parsed.raw_spectrum && parsed.raw_spectrum->sce);
    }

    static PtsParsedMeasurement parsed_with_requested_mode(PtsParsedMeasurement parsed, PtsSpecularMode mode)
    {
        const bool has_sci = parsed.sci_lab || (parsed.raw_spectrum && parsed.raw_spectrum->sci);
        const bool has_sce = parsed.sce_lab || (parsed.raw_spectrum && parsed.raw_spectrum->sce);

        if (has_sci && !has_sce && mode == PtsSpecularMode::SCE) {
            parsed.sce_lab = parsed.sci_lab ? parsed.sci_lab : std::optional<LabReading>(parsed.lab);
            parsed.sci_lab.reset();
            if (parsed.raw_spectrum && parsed.raw_spectrum->sci) {
                parsed.raw_spectrum->sce = std::move(parsed.raw_spectrum->sci);
                parsed.raw_spectrum->sci.reset();
            }
            parsed.lab = *parsed.sce_lab;
        } else if (has_sce && !has_sci && mode == PtsSpecularMode::SCI) {
            parsed.sci_lab = parsed.sce_lab ? parsed.sce_lab : std::optional<LabReading>(parsed.lab);
            parsed.sce_lab.reset();
            if (parsed.raw_spectrum && parsed.raw_spectrum->sce) {
                parsed.raw_spectrum->sci = std::move(parsed.raw_spectrum->sce);
                parsed.raw_spectrum->sce.reset();
            }
            parsed.lab = *parsed.sci_lab;
        } else if (!has_sci && !has_sce) {
            if (mode == PtsSpecularMode::SCI)
                parsed.sci_lab = parsed.lab;
            else
                parsed.sce_lab = parsed.lab;
        }
        return parsed;
    }

    static PtsParsedMeasurement merge_pts_measurements(const std::optional<PtsParsedMeasurement> &first,
                                                       const std::optional<PtsParsedMeasurement> &second)
    {
        PtsParsedMeasurement merged = first ? *first : (second ? *second : PtsParsedMeasurement {});

        const auto merge_one = [&merged](const PtsParsedMeasurement &parsed) {
            if (!merged.sci_lab && parsed.sci_lab)
                merged.sci_lab = parsed.sci_lab;
            if (!merged.sce_lab && parsed.sce_lab)
                merged.sce_lab = parsed.sce_lab;

            if (parsed.raw_spectrum) {
                if (!merged.raw_spectrum)
                    merged.raw_spectrum = parsed.raw_spectrum;
                else {
                    if (!merged.raw_spectrum->sci && parsed.raw_spectrum->sci)
                        merged.raw_spectrum->sci = parsed.raw_spectrum->sci;
                    if (!merged.raw_spectrum->sce && parsed.raw_spectrum->sce)
                        merged.raw_spectrum->sce = parsed.raw_spectrum->sce;
                }
            }
        };

        if (first)
            merge_one(*first);
        if (second)
            merge_one(*second);

        if (merged.sci_lab)
            merged.lab = *merged.sci_lab;
        else if (merged.sce_lab)
            merged.lab = *merged.sce_lab;
        return merged;
    }

    static wxString pts_operation_name(uint8_t operation)
    {
        if (operation == 0x23)
            return _L("standard");
        if (operation == 0x24)
            return _L("sample");
        return _L("unknown");
    }

    static nlohmann::json wavelength_json_400_700_10nm()
    {
        nlohmann::json wavelengths = nlohmann::json::array();
        for (int wavelength = 400; wavelength <= 700; wavelength += 10)
            wavelengths.push_back(wavelength);
        return wavelengths;
    }

    static nlohmann::json spectrum_samples_to_json(const SpectrumSamples &samples)
    {
        nlohmann::json out;
        out["raw_u16"] = samples.raw_u16;
        out["reflectance"] = samples.reflectance;
        return out;
    }

    static nlohmann::json raw_spectrum_to_json(const RawSpectrumData &raw)
    {
        nlohmann::json out;
        out["protocol"] = "3nh_pts";
        out["operation"] = wx_to_u8(wxString::Format("0x%02X", unsigned(raw.operation)));
        out["operation_name"] = wx_to_u8(pts_operation_name(raw.operation));
        out["data_type"] = raw.data_type == 0 ? "spectrum" : wx_to_u8(wxString::Format("%u", unsigned(raw.data_type)));
        out["illuminant"] = raw.illuminant == 0 ? "D65" : wx_to_u8(wxString::Format("%u", unsigned(raw.illuminant)));
        out["observer"] = raw.observer == 0 ? "2" : "10";
        out["sample_scale"] = 0.0001;
        out["wavelength_start_nm"] = 400;
        out["wavelength_end_nm"] = 700;
        out["wavelength_step_nm"] = 10;
        out["wavelength_nm"] = wavelength_json_400_700_10nm();
        out["specular_modes"] = nlohmann::json::array();
        if (raw.sci)
            out["specular_modes"].push_back("SCI");
        if (raw.sce)
            out["specular_modes"].push_back("SCE");
        if (raw.sci)
            out["sci"] = spectrum_samples_to_json(*raw.sci);
        if (raw.sce)
            out["sce"] = spectrum_samples_to_json(*raw.sce);
        if (!raw.payload.empty())
            out["payload_hex"] = wx_to_u8(bytes_to_hex(raw.payload, raw.payload.size()));
        return out;
    }

    static std::optional<std::vector<uint16_t>> uint16_vector_from_json(const nlohmann::json &array)
    {
        if (!array.is_array())
            return std::nullopt;
        std::vector<uint16_t> values;
        values.reserve(array.size());
        for (const nlohmann::json &value : array) {
            if (!value.is_number_unsigned() && !value.is_number_integer())
                return std::nullopt;
            const int number = value.get<int>();
            if (number < 0 || number > 65535)
                return std::nullopt;
            values.push_back(uint16_t(number));
        }
        return values;
    }

    static std::optional<std::vector<double>> double_vector_from_json(const nlohmann::json &array)
    {
        if (!array.is_array())
            return std::nullopt;
        std::vector<double> values;
        values.reserve(array.size());
        for (const nlohmann::json &value : array) {
            if (!value.is_number())
                return std::nullopt;
            values.push_back(value.get<double>());
        }
        return values;
    }

    static std::optional<SpectrumSamples> spectrum_samples_from_json(const nlohmann::json &object)
    {
        if (!object.is_object() || !object.contains("raw_u16"))
            return std::nullopt;
        SpectrumSamples samples;
        if (auto raw = uint16_vector_from_json(object["raw_u16"]))
            samples.raw_u16 = std::move(*raw);
        else
            return std::nullopt;
        if (std::none_of(samples.raw_u16.begin(), samples.raw_u16.end(), [](uint16_t value) { return value != 0; }))
            return std::nullopt;

        if (object.contains("reflectance")) {
            if (auto reflectance = double_vector_from_json(object["reflectance"]))
                samples.reflectance = std::move(*reflectance);
        }
        if (samples.reflectance.empty()) {
            samples.reflectance.reserve(samples.raw_u16.size());
            for (uint16_t raw : samples.raw_u16)
                samples.reflectance.push_back(double(raw) * 0.0001);
        }
        return samples;
    }

    static std::optional<RawSpectrumData> raw_spectrum_from_json(const nlohmann::json &object)
    {
        if (!object.is_object())
            return std::nullopt;

        RawSpectrumData raw;
        if (object.contains("operation")) {
            if (object["operation"].is_string()) {
                const std::string op = object["operation"].get<std::string>();
                unsigned value = 0;
                std::stringstream ss;
                ss << std::hex << (op.rfind("0x", 0) == 0 || op.rfind("0X", 0) == 0 ? op.substr(2) : op);
                ss >> value;
                raw.operation = uint8_t(value & 0xFF);
            } else if (object["operation"].is_number_integer()) {
                raw.operation = uint8_t(object["operation"].get<int>() & 0xFF);
            }
        }
        if (object.contains("data_type") && object["data_type"].is_number_integer())
            raw.data_type = uint8_t(object["data_type"].get<int>() & 0xFF);
        if (object.contains("observer")) {
            if (object["observer"].is_string())
                raw.observer = object["observer"].get<std::string>().rfind("2", 0) == 0 ? 0 : 1;
            else if (object["observer"].is_number_integer())
                raw.observer = object["observer"].get<int>() == 2 ? 0 : 1;
        }
        if (object.contains("illuminant") && object["illuminant"].is_number_integer())
            raw.illuminant = uint8_t(object["illuminant"].get<int>() & 0xFF);
        if (object.contains("sci"))
            raw.sci = spectrum_samples_from_json(object["sci"]);
        if (object.contains("sce"))
            raw.sce = spectrum_samples_from_json(object["sce"]);
        return raw.sci || raw.sce ? std::optional<RawSpectrumData>(raw) : std::nullopt;
    }

    std::optional<size_t> swatch_for_grid_row(int grid_row) const
    {
        if (grid_row < 0 || static_cast<size_t>(grid_row) >= m_grid_rows.size())
            return std::nullopt;
        return m_grid_rows[static_cast<size_t>(grid_row)].swatch;
    }

    std::optional<size_t> take_for_grid_row(int grid_row) const
    {
        if (grid_row < 0 || static_cast<size_t>(grid_row) >= m_grid_rows.size())
            return std::nullopt;
        const GridRowRef &ref = m_grid_rows[static_cast<size_t>(grid_row)];
        return ref.kind == GridRowKind::Take ? std::optional<size_t>(ref.take) : std::nullopt;
    }

    int grid_row_for_swatch(size_t swatch) const
    {
        for (size_t row = 0; row < m_grid_rows.size(); ++row)
            if (m_grid_rows[row].kind == GridRowKind::Swatch && m_grid_rows[row].swatch == swatch)
                return static_cast<int>(row);
        return -1;
    }

    std::vector<LabReading> readings_from_grid_row(int grid_row) const
    {
        if (grid_row < 0 || grid_row >= m_grid->GetNumberRows())
            return {};
        return parse_readings_text(m_grid->GetCellValue(grid_row, ColReadings));
    }

    std::vector<LabReading> readings_for_row(int row) const
    {
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            return {};
        return readings_from_takes(m_rows[static_cast<size_t>(row)].takes);
    }

    wxString ill_obs_text() const
    {
        return m_illuminant->GetValue().Trim(true).Trim(false) + _L("/") + m_observer->GetValue().Trim(true).Trim(false);
    }

    double pass_delta_e_limit() const
    {
        return m_pass_delta_e != nullptr ? m_pass_delta_e->GetValue() : 2.0;
    }

    bool take_passes(const LabReading &delta) const
    {
        return delta_e_ab(delta) <= pass_delta_e_limit();
    }

    int target_take_count() const
    {
        return std::max(m_target_takes != nullptr ? m_target_takes->GetValue() : 3, 1);
    }

    std::optional<FailedTake> failed_take_for_row(int row) const
    {
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            return std::nullopt;

        const std::vector<MeasurementTake> &takes = m_rows[static_cast<size_t>(row)].takes;
        if (takes.size() < 3)
            return std::nullopt;

        FailedTake worst;
        worst.delta_e = pass_delta_e_limit();
        bool found = false;
        for (size_t candidate = 0; candidate < takes.size(); ++candidate) {
            LabReading avg;
            for (size_t i = 0; i < takes.size(); ++i) {
                if (i == candidate)
                    continue;
                avg.l += takes[i].lab.l;
                avg.a += takes[i].lab.a;
                avg.b += takes[i].lab.b;
            }
            const double count = double(takes.size() - 1);
            avg.l /= count;
            avg.a /= count;
            avg.b /= count;

            const double de = delta_e_ab(lab_delta(takes[candidate].lab, avg));
            if (de > worst.delta_e) {
                worst.index = candidate;
                worst.delta_e = de;
                found = true;
            }
        }

        return found ? std::optional<FailedTake>(worst) : std::nullopt;
    }

    void set_simulation_cell(int grid_row, const LabReading &reading)
    {
        const wxColour colour = lab_to_srgb_colour(reading);
        m_grid->SetCellValue(grid_row, ColSim, u8_to_wx(colour_to_hex(colour)));
        m_grid->SetCellBackgroundColour(grid_row, ColSim, colour);
        m_grid->SetCellTextColour(grid_row, ColSim, readable_text_colour(colour));
    }

    void reset_simulation_cell(int grid_row)
    {
        m_grid->SetCellValue(grid_row, ColSim, wxString());
        m_grid->SetCellBackgroundColour(grid_row, ColSim, m_grid->GetDefaultCellBackgroundColour());
        m_grid->SetCellTextColour(grid_row, ColSim, m_grid->GetDefaultCellTextColour());
    }

    void set_judgement_cell(int grid_row, bool pass)
    {
        m_grid->SetCellValue(grid_row, ColJudgement, pass ? _L("Pass") : _L("Fail"));
        m_grid->SetCellTextColour(grid_row, ColJudgement, pass ? wxColour(0, 128, 0) : wxColour(180, 0, 0));
    }

    void clear_delta_cells(int grid_row)
    {
        m_grid->SetCellValue(grid_row, ColDeltaL, wxString());
        m_grid->SetCellValue(grid_row, ColDeltaA, wxString());
        m_grid->SetCellValue(grid_row, ColDeltaB, wxString());
        m_grid->SetCellValue(grid_row, ColOffset, wxString());
        m_grid->SetCellValue(grid_row, ColDeltaE, wxString());
        m_grid->SetCellValue(grid_row, ColJudgement, wxString());
        m_grid->SetCellTextColour(grid_row, ColJudgement, m_grid->GetDefaultCellTextColour());
    }

    void populate_swatch_summary_row(int grid_row, size_t swatch)
    {
        const std::vector<LabReading> readings = readings_for_row(static_cast<int>(swatch));
        m_grid->SetCellValue(grid_row, ColReadings, format_readings(readings));
        m_grid->SetCellValue(grid_row, ColTakes, readings.empty() ? wxString() : wxString::Format("%zu", readings.size()));
        m_grid->SetCellValue(grid_row, ColHex, m_rows[swatch].rgb_hex);
        m_grid->SetCellValue(grid_row, ColNotes, m_rows[swatch].notes);
        m_grid->SetCellValue(grid_row, ColIllObs, ill_obs_text());
        clear_delta_cells(grid_row);

        if (const std::optional<LabReading> avg = average_reading(readings)) {
            m_grid->SetCellValue(grid_row, ColL, wxString::Format("%.3f", avg->l));
            m_grid->SetCellValue(grid_row, ColA, wxString::Format("%.3f", avg->a));
            m_grid->SetCellValue(grid_row, ColB, wxString::Format("%.3f", avg->b));
            set_simulation_cell(grid_row, *avg);

            bool all_pass = true;
            double max_delta_e = 0.0;
            for (const MeasurementTake &take : m_rows[swatch].takes) {
                const LabReading delta = lab_delta(take.lab, *avg);
                const double de = delta_e_ab(delta);
                max_delta_e = std::max(max_delta_e, de);
                all_pass = all_pass && take_passes(delta);
            }
            m_grid->SetCellValue(grid_row, ColDeltaE, wxString::Format("%.2f", max_delta_e));
            set_judgement_cell(grid_row, all_pass);
        } else {
            m_grid->SetCellValue(grid_row, ColL, wxString());
            m_grid->SetCellValue(grid_row, ColA, wxString());
            m_grid->SetCellValue(grid_row, ColB, wxString());
            reset_simulation_cell(grid_row);
        }

        for (int col = 0; col < ColCount; ++col)
            m_grid->SetReadOnly(grid_row, col, col != ColReadings && col != ColHex && col != ColNotes);
    }

    void populate_take_row(int grid_row, size_t swatch, size_t take_idx)
    {
        const MeasurementTake &take = m_rows[swatch].takes[take_idx];
        const std::vector<LabReading> readings = readings_for_row(static_cast<int>(swatch));
        const std::optional<LabReading> avg = average_reading(readings);
        const LabReading delta = avg ? lab_delta(take.lab, *avg) : LabReading {};
        const double de = avg ? delta_e_ab(delta) : 0.0;

        m_grid->SetRowLabelValue(grid_row, wxString::Format("%zu.%zu", swatch + 1, take_idx + 1));
        m_grid->SetCellValue(grid_row, ColId, wxString::Format(_L("  Take %zu"), take_idx + 1));
        m_grid->SetCellValue(grid_row, ColType, take.source.empty() ? _L("Take") : take.source);
        m_grid->SetCellValue(grid_row, ColReadings, format_reading(take.lab));
        m_grid->SetCellValue(grid_row, ColTakes, wxString::Format("%zu", take_idx + 1));
        m_grid->SetCellValue(grid_row, ColL, wxString::Format("%.3f", take.lab.l));
        m_grid->SetCellValue(grid_row, ColA, wxString::Format("%.3f", take.lab.a));
        m_grid->SetCellValue(grid_row, ColB, wxString::Format("%.3f", take.lab.b));
        m_grid->SetCellValue(grid_row, ColDateTime, take.timestamp);
        m_grid->SetCellValue(grid_row, ColIllObs, ill_obs_text());
        m_grid->SetCellValue(grid_row, ColDeltaL, wxString::Format("%.2f", delta.l));
        m_grid->SetCellValue(grid_row, ColDeltaA, wxString::Format("%.2f", delta.a));
        m_grid->SetCellValue(grid_row, ColDeltaB, wxString::Format("%.2f", delta.b));
        m_grid->SetCellValue(grid_row, ColOffset, avg ? color_offset_text(delta) : wxString());
        m_grid->SetCellValue(grid_row, ColDeltaE, wxString::Format("%.2f", de));
        set_simulation_cell(grid_row, take.lab);
        set_judgement_cell(grid_row, take_passes(delta));

        const wxColour row_bg = take_passes(delta) ? wxColour(235, 250, 235) : wxColour(255, 235, 235);
        for (int col = 0; col < ColCount; ++col) {
            if (col != ColSim)
                m_grid->SetCellBackgroundColour(grid_row, col, row_bg);
            m_grid->SetReadOnly(grid_row, col, true);
        }
    }

    void update_grid_row_visibility()
    {
        if (m_grid == nullptr)
            return;

        const int current = active_row();
        const wxColour active_bg(225, 241, 255);
        const wxColour default_bg = m_grid->GetDefaultCellBackgroundColour();

        m_grid->BeginBatch();
        for (size_t row = 0; row < m_grid_rows.size(); ++row) {
            const GridRowRef &ref = m_grid_rows[row];
            const int grid_row = static_cast<int>(row);

            if (ref.kind == GridRowKind::Take) {
                if (static_cast<int>(ref.swatch) == current)
                    m_grid->ShowRow(grid_row);
                else
                    m_grid->HideRow(grid_row);
                continue;
            }

            const bool is_active = static_cast<int>(ref.swatch) == current;
            for (int col = 0; col < ColCount; ++col) {
                if (col == ColSim && !m_grid->GetCellValue(grid_row, col).empty())
                    continue;
                m_grid->SetCellBackgroundColour(grid_row, col, is_active ? active_bg : default_bg);
            }
        }
        m_grid->EndBatch();
        m_grid->ForceRefresh();
    }

    void refresh_measurement_grid()
    {
        if (m_grid == nullptr)
            return;

        m_refreshing_grid = true;
        const int previous_swatch = active_row();
        if (m_grid->GetNumberRows() > 0)
            m_grid->DeleteRows(0, m_grid->GetNumberRows());
        m_grid_rows.clear();

        size_t total_rows = 0;
        for (const Row &row : m_rows)
            total_rows += 1 + row.takes.size();
        if (total_rows > 0)
            m_grid->AppendRows(static_cast<int>(total_rows));

        int grid_row = 0;
        for (size_t swatch = 0; swatch < m_rows.size(); ++swatch) {
            m_grid_rows.push_back({ GridRowKind::Swatch, swatch, 0 });
            fill_manifest_row(grid_row, m_rows[swatch].manifest_record);
            m_grid->SetRowLabelValue(grid_row, wxString::Format("%zu", swatch + 1));
            populate_swatch_summary_row(grid_row, swatch);
            ++grid_row;

            for (size_t take = 0; take < m_rows[swatch].takes.size(); ++take) {
                m_grid_rows.push_back({ GridRowKind::Take, swatch, take });
                populate_take_row(grid_row, swatch, take);
                ++grid_row;
            }
        }

        m_refreshing_grid = false;
        if (previous_swatch >= 0 && static_cast<size_t>(previous_swatch) < m_rows.size())
            select_measurement_row(previous_swatch);
        else
            update_grid_row_visibility();
        m_grid->ForceRefresh();
    }

    void recompute_row(int row)
    {
        const int grid_row = row >= 0 ? grid_row_for_swatch(static_cast<size_t>(row)) : -1;
        if (grid_row >= 0)
            populate_swatch_summary_row(grid_row, static_cast<size_t>(row));
    }

    int active_row() const
    {
        if (m_current_swatch >= 0 && static_cast<size_t>(m_current_swatch) < m_rows.size())
            return m_current_swatch;
        const int cursor = m_grid->GetGridCursorRow();
        if (const std::optional<size_t> swatch = swatch_for_grid_row(cursor))
            return static_cast<int>(*swatch);
        const int next = find_next_incomplete_row(0);
        return next >= 0 ? next : 0;
    }

    int find_next_incomplete_row(int start) const
    {
        if (m_rows.empty())
            return -1;
        const int rows = static_cast<int>(m_rows.size());
        const int target = target_take_count();
        for (int pass = 0; pass < 2; ++pass) {
            const int begin = pass == 0 ? std::max(start, 0) : 0;
            const int end = pass == 0 ? rows : std::min(start, rows);
            for (int row = begin; row < end; ++row)
                if (int(readings_for_row(row).size()) < target)
                    return row;
        }
        return -1;
    }

    void select_measurement_row(int row)
    {
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            return;
        m_current_swatch = row;
        const int grid_row = grid_row_for_swatch(static_cast<size_t>(row));
        if (grid_row >= 0) {
            m_grid->SetGridCursor(grid_row, ColReadings);
            m_grid->SelectRow(grid_row);
        }
        update_grid_row_visibility();
        update_next_sample();
    }

    void sync_row_takes_from_grid(int row)
    {
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            return;

        const int grid_row = grid_row_for_swatch(static_cast<size_t>(row));
        if (grid_row < 0)
            return;

        std::vector<LabReading> readings = readings_from_grid_row(grid_row);
        const std::vector<MeasurementTake> &takes = m_rows[static_cast<size_t>(row)].takes;
        bool matches = readings.size() == takes.size();
        if (matches) {
            for (size_t i = 0; i < readings.size(); ++i) {
                if (!lab_readings_match(readings[i], takes[i].lab)) {
                    matches = false;
                    break;
                }
            }
        }
        if (matches)
            return;

        std::vector<MeasurementTake> replacement;
        replacement.reserve(readings.size());
        for (const LabReading &reading : readings)
            replacement.push_back(make_table_take(reading));
        m_rows[static_cast<size_t>(row)].takes = std::move(replacement);
        update_primary_color_from_anchor_measurement(row);
    }

    void set_takes_for_row(int row, std::vector<MeasurementTake> takes)
    {
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            return;
        for (MeasurementTake &take : takes)
            if (take.timestamp.empty())
                take.timestamp = current_timestamp();
        m_rows[static_cast<size_t>(row)].takes = std::move(takes);
        update_primary_color_from_anchor_measurement(row);
        refresh_measurement_grid();
        select_measurement_row(row);
        update_summary();
        update_next_sample();
    }

    void append_take_to_row(int row, MeasurementTake take)
    {
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            return;

        sync_row_takes_from_grid(row);
        if (take.timestamp.empty())
            take.timestamp = current_timestamp();
        m_rows[static_cast<size_t>(row)].takes.push_back(std::move(take));
        update_primary_color_from_anchor_measurement(row);
        const std::vector<LabReading> readings = readings_from_takes(m_rows[static_cast<size_t>(row)].takes);
        refresh_measurement_grid();
        select_measurement_row(row);
        const MeasurementTake &last_take = m_rows[static_cast<size_t>(row)].takes.back();
        m_serial_status->SetLabel(wxString::Format(_L("%s take %zu: %.3f %.3f %.3f"),
                                                   last_take.source,
                                                   readings.size(),
                                                   last_take.lab.l,
                                                   last_take.lab.a,
                                                   last_take.lab.b));

        if (!m_measure_all_active && m_auto_advance->GetValue() && int(readings.size()) >= target_take_count()) {
            const int next = find_next_incomplete_row(row + 1);
            if (next >= 0)
                select_measurement_row(next);
        }

        update_summary();
        update_next_sample();

        if (m_measure_all_active && row == m_measure_all_row)
            CallAfter([this]() { continue_measure_all(); });
    }

    void append_reading_to_row(int row, const LabReading &reading, const wxString &source)
    {
        MeasurementTake take;
        take.lab = reading;
        take.source = source;
        take.timestamp = current_timestamp();
        append_take_to_row(row, std::move(take));
    }

    void append_pts_measurement_to_row(int row, const PtsParsedMeasurement &parsed)
    {
        MeasurementTake take;
        take.lab = parsed.lab;
        take.sci_lab = parsed.sci_lab;
        take.sce_lab = parsed.sce_lab;
        take.raw_spectrum = parsed.raw_spectrum;
        take.source = _L("Spectro");
        const wxString specular_mode = take_specular_mode_text(take);
        if (!specular_mode.empty())
            take.source += _L(" ") + specular_mode;
        append_take_to_row(row, std::move(take));
    }

    void handle_completed_pts_measurement(const PtsParsedMeasurement &parsed)
    {
        if (m_reference_measure_target != ReferenceMeasurementTarget::None) {
            const ReferenceMeasurementTarget target = m_reference_measure_target;
            m_reference_measure_target = ReferenceMeasurementTarget::None;
            store_reference_measurement(target, parsed.lab);
            return;
        }

        append_pts_measurement_to_row(active_row(), parsed);
    }

    void set_readings_for_row(int row, const std::vector<LabReading> &readings)
    {
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            return;
        std::vector<MeasurementTake> takes;
        takes.reserve(readings.size());
        for (const LabReading &reading : readings)
            takes.push_back(make_table_take(reading));
        set_takes_for_row(row, std::move(takes));
    }

    void on_previous_sample(wxCommandEvent &)
    {
        if (m_grid->GetNumberRows() == 0)
            return;
        select_measurement_row(std::max(active_row() - 1, 0));
    }

    void on_remove_last_take(wxCommandEvent &)
    {
        const int row = active_row();
        std::vector<LabReading> readings = readings_for_row(row);
        if (readings.empty()) {
            if (m_serial_status)
                m_serial_status->SetLabel(_L("No takes on current sample"));
            return;
        }

        sync_row_takes_from_grid(row);
        std::vector<MeasurementTake> takes = size_t(row) < m_rows.size() ? m_rows[size_t(row)].takes : std::vector<MeasurementTake>{};
        if (!takes.empty())
            takes.pop_back();
        set_takes_for_row(row, std::move(takes));
        if (m_serial_status)
            m_serial_status->SetLabel(_L("Removed last take"));
    }

    void on_clear_current_sample(wxCommandEvent &)
    {
        const int row = active_row();
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            return;
        set_takes_for_row(row, {});
        if (m_serial_status)
            m_serial_status->SetLabel(_L("Cleared current sample"));
    }

    void on_clear_current_sample_and_measure_all(wxCommandEvent &)
    {
        if (!serial_ready_for_measurement())
            return;

        const int row = active_row();
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            return;

        set_takes_for_row(row, {});
        if (m_serial_status)
            m_serial_status->SetLabel(_L("Cleared current sample"));
        start_measure_all_for_row(row);
    }

    void on_clear_previous_sample_and_measure_all(wxCommandEvent &)
    {
        if (!serial_ready_for_measurement())
            return;

        const int previous = active_row() - 1;
        if (previous < 0 || static_cast<size_t>(previous) >= m_rows.size()) {
            if (m_serial_status)
                m_serial_status->SetLabel(_L("No previous sample"));
            return;
        }

        set_takes_for_row(previous, {});
        if (m_serial_status)
            m_serial_status->SetLabel(_L("Cleared previous sample"));
        start_measure_all_for_row(previous);
    }

    void on_add_manual_take(wxCommandEvent &)
    {
        LabReading reading;
        if (!parse_double_cell(m_manual_l->GetValue(), reading.l) ||
            !parse_double_cell(m_manual_a->GetValue(), reading.a) ||
            !parse_double_cell(m_manual_b->GetValue(), reading.b)) {
            MessageDialog(this, _L("Enter L*, a*, and b* before adding a take."), _L("Color swatch measurements"), wxOK | wxICON_WARNING).ShowModal();
            return;
        }
        append_reading_to_row(active_row(), reading, _L("Manual"));
        m_manual_l->Clear();
        m_manual_a->Clear();
        m_manual_b->Clear();
        m_manual_l->SetFocus();
    }

    void on_load_manifest(wxCommandEvent &)
    {
        wxFileDialog dlg(this,
                         _L("Load calibration swatch manifest"),
                         wxEmptyString,
                         wxEmptyString,
                         _L("JSON files (*.json)|*.json|All files (*.*)|*.*"),
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK)
            load_manifest(dlg.GetPath());
    }

    void load_reference_measurements_from_json(const nlohmann::json &measurements)
    {
        if (!measurements.contains("reference_measurements") || !measurements["reference_measurements"].is_object())
            return;

        const nlohmann::json &references = measurements["reference_measurements"];
        if (m_reference_white_backing != nullptr && references.contains("white_backing"))
            m_reference_white_backing->SetValue(reference_measurement_from_json(references["white_backing"]));
        if (m_reference_black_backing != nullptr && references.contains("black_backing"))
            m_reference_black_backing->SetValue(reference_measurement_from_json(references["black_backing"]));
    }

    void on_import_measurements(wxCommandEvent &)
    {
        if (m_rows.empty()) {
            MessageDialog(this, _L("Load a swatch manifest first."), _L("Color swatch measurements"), wxOK | wxICON_WARNING).ShowModal();
            return;
        }

        wxFileDialog dlg(this,
                         _L("Import color swatch measurements"),
                         wxEmptyString,
                         wxEmptyString,
                         _L("JSON files (*.json)|*.json|All files (*.*)|*.*"),
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK)
            return;

        nlohmann::json measurements;
        if (!read_json_file(dlg.GetPath(), measurements))
            return;
        if (!measurements.contains("records") || !measurements["records"].is_array()) {
            MessageDialog(this, _L("The selected file does not contain measurement records."), _L("Color swatch measurements"), wxOK | wxICON_WARNING).ShowModal();
            return;
        }

        load_reference_measurements_from_json(measurements);

        std::map<std::string, int> row_by_id;
        for (size_t row = 0; row < m_rows.size(); ++row) {
            const nlohmann::json &manifest_record = m_rows[row].manifest_record;
            if (manifest_record.contains("swatch_id")) {
                const std::string id = wx_to_u8(json_scalar_to_text(manifest_record["swatch_id"]));
                row_by_id[measurement_record_key(id, m_rows[row].measurement_condition)] = static_cast<int>(row);

                // Backward compatibility for measurement files written before explicit backing rows.
                const bool anchor = is_reflective_anchor_record(manifest_record);
                if ((anchor && m_rows[row].measurement_condition == "white_backing") ||
                    (!anchor && m_rows[row].measurement_condition == "black_backing")) {
                    row_by_id[measurement_record_key(id, "normal")] = static_cast<int>(row);
                    row_by_id[measurement_record_key(id, "")] = static_cast<int>(row);
                }
            }
        }

        for (const nlohmann::json &record : measurements["records"]) {
            std::string id;
            if (record.contains("swatch_id") && record["swatch_id"].is_string())
                id = record["swatch_id"].get<std::string>();
            else if (record.contains("manifest") && record["manifest"].contains("swatch_id") && record["manifest"]["swatch_id"].is_string())
                id = record["manifest"]["swatch_id"].get<std::string>();
            std::string condition = "normal";
            if (record.contains("measurement_condition") && record["measurement_condition"].is_string())
                condition = record["measurement_condition"].get<std::string>();
            else if (record.contains("manifest") && record["manifest"].contains("measurement_condition") &&
                     record["manifest"]["measurement_condition"].is_string())
                condition = record["manifest"]["measurement_condition"].get<std::string>();
            const std::string key = measurement_record_key(id, condition);
            if (id.empty() || row_by_id.count(key) == 0)
                continue;

            const nlohmann::json *measured = record.contains("measured") ? &record["measured"] : &record;
            const int row = row_by_id[key];

            std::vector<MeasurementTake> takes;
            if (measured->contains("readings") && (*measured)["readings"].is_array()) {
                for (const nlohmann::json &reading_json : (*measured)["readings"]) {
                    MeasurementTake take;
                    if (auto reading = lab_from_json(reading_json))
                        take.lab = *reading;
                    else if (reading_json.is_array() && reading_json.size() >= 3 &&
                             reading_json[0].is_number() && reading_json[1].is_number() && reading_json[2].is_number())
                        take.lab = { reading_json[0].get<double>(), reading_json[1].get<double>(), reading_json[2].get<double>() };
                    else
                        continue;
                    if (reading_json.contains("lab_sci"))
                        take.sci_lab = lab_from_json(reading_json["lab_sci"]);
                    if (reading_json.contains("lab_sce"))
                        take.sce_lab = lab_from_json(reading_json["lab_sce"]);
                    take.source = reading_json.contains("source") && reading_json["source"].is_string() ?
                        u8_to_wx(reading_json["source"].get<std::string>()) :
                        _L("Import");
                    if (reading_json.contains("timestamp") && reading_json["timestamp"].is_string())
                        take.timestamp = u8_to_wx(reading_json["timestamp"].get<std::string>());
                    else if (reading_json.contains("date_time") && reading_json["date_time"].is_string())
                        take.timestamp = u8_to_wx(reading_json["date_time"].get<std::string>());
                    if (reading_json.contains("raw_spectrum"))
                        take.raw_spectrum = raw_spectrum_from_json(reading_json["raw_spectrum"]);
                    takes.push_back(std::move(take));
                }
            }
            if (takes.empty()) {
                if (measured->contains("lab"))
                    if (auto reading = lab_from_json((*measured)["lab"]))
                        takes.push_back(make_table_take(*reading));
                if (takes.empty() && measured->contains("lab_average"))
                    if (auto reading = lab_from_json((*measured)["lab_average"]))
                        takes.push_back(make_table_take(*reading));
                if (takes.empty() && measured->contains("measured_lab"))
                    if (auto reading = lab_from_json((*measured)["measured_lab"]))
                        takes.push_back(make_table_take(*reading));
            }
            if (!takes.empty()) {
                for (MeasurementTake &take : takes)
                    if (take.timestamp.empty())
                        take.timestamp = current_timestamp();
                m_rows[static_cast<size_t>(row)].takes = std::move(takes);
            }

            if (measured->contains("rgb_hex") && (*measured)["rgb_hex"].is_string())
                m_rows[static_cast<size_t>(row)].rgb_hex = u8_to_wx((*measured)["rgb_hex"].get<std::string>());
            else if (measured->contains("measured_rgb_hex") && (*measured)["measured_rgb_hex"].is_string())
                m_rows[static_cast<size_t>(row)].rgb_hex = u8_to_wx((*measured)["measured_rgb_hex"].get<std::string>());
            if (measured->contains("notes") && (*measured)["notes"].is_string())
                m_rows[static_cast<size_t>(row)].notes = u8_to_wx((*measured)["notes"].get<std::string>());
        }

        for (int row = 0; row < static_cast<int>(m_rows.size()); ++row)
            update_primary_color_from_anchor_measurement(row);

        refresh_measurement_grid();
        update_summary();
        update_next_sample();
    }

    void on_save_measurements(wxCommandEvent &)
    {
        if (m_rows.empty()) {
            MessageDialog(this, _L("Load a swatch manifest first."), _L("Color swatch measurements"), wxOK | wxICON_WARNING).ShowModal();
            return;
        }

        if (m_grid->IsCellEditControlEnabled()) {
            m_grid->SaveEditControlValue();
            m_grid->HideCellEditControl();
        }

        wxFileName manifest_name(m_manifest_path->GetValue());
        const wxString default_name = manifest_name.GetName().empty() ?
            _L("calibration_swatch_measurements.json") :
            manifest_name.GetName() + _L("_measurements.json");

        wxFileDialog dlg(this,
                         _L("Save color swatch measurements"),
                         manifest_name.GetPath(),
                         default_name,
                         _L("JSON files (*.json)|*.json|All files (*.*)|*.*"),
                         wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK)
            return;

        nlohmann::json out;
        out["schema"] = "fullspectrum.calibration_measurements.v2";
        out["source_manifest_path"] = wx_to_u8(m_manifest_path->GetValue());
        out["source_manifest_schema"] = m_manifest.value("schema", "");
        if (m_manifest.contains("primary_filaments"))
            out["primary_filaments"] = m_manifest["primary_filaments"];
        if (m_manifest.contains("primary_colors"))
            out["primary_colors"] = m_manifest["primary_colors"];
        if (m_manifest.contains("primary_td_values"))
            out["primary_td_values"] = m_manifest["primary_td_values"];
        out["instrument"] = {
            { "name", wx_to_u8(m_instrument->GetValue()) },
            { "illuminant", wx_to_u8(m_illuminant->GetValue()) },
            { "observer", wx_to_u8(m_observer->GetValue()) },
            { "measurement_geometry", wx_to_u8(m_geometry->GetValue()) },
            { "specular_modes", { "SCI", "SCE" } },
            { "primary_lab_mode", "SCI" },
            { "target_take_count", target_take_count() }
        };
        out["reference_measurements"] = {
            { "white_backing", reference_measurement_to_json(m_reference_white_backing) },
            { "black_backing", reference_measurement_to_json(m_reference_black_backing) }
        };
        out["records"] = nlohmann::json::array();

        for (int row = 0; row < static_cast<int>(m_rows.size()); ++row) {
            nlohmann::json measured;
            sync_row_takes_from_grid(row);
            const std::vector<MeasurementTake> &takes = m_rows[static_cast<size_t>(row)].takes;
            const std::vector<LabReading> readings = readings_from_takes(takes);
            const std::optional<LabReading> avg = average_reading(readings);
            if (!takes.empty()) {
                measured["readings"] = nlohmann::json::array();
                for (size_t i = 0; i < takes.size(); ++i) {
                    nlohmann::json reading_json = lab_to_json(takes[i].lab);
                    reading_json["take"] = i + 1;
                    if (!takes[i].source.empty())
                        reading_json["source"] = wx_to_u8(takes[i].source);
                    const wxString specular_mode = take_specular_mode_text(takes[i]);
                    if (!specular_mode.empty()) {
                        reading_json["specular_mode"] = wx_to_u8(specular_mode);
                        reading_json["primary_lab_mode"] = takes[i].sci_lab ? "SCI" : "SCE";
                    }
                    if (takes[i].sci_lab)
                        reading_json["lab_sci"] = lab_to_json(*takes[i].sci_lab);
                    if (takes[i].sce_lab)
                        reading_json["lab_sce"] = lab_to_json(*takes[i].sce_lab);
                    if (!takes[i].timestamp.empty()) {
                        reading_json["timestamp"] = wx_to_u8(takes[i].timestamp);
                        reading_json["date_time"] = wx_to_u8(takes[i].timestamp);
                    }
                    if (avg) {
                        const LabReading delta = lab_delta(takes[i].lab, *avg);
                        reading_json["delta_L"] = delta.l;
                        reading_json["delta_a"] = delta.a;
                        reading_json["delta_b"] = delta.b;
                        reading_json["delta_e_ab"] = delta_e_ab(delta);
                        reading_json["color_offset"] = wx_to_u8(color_offset_text(delta));
                        reading_json["judgement"] = take_passes(delta) ? "Pass" : "Fail";
                    }
                    if (takes[i].raw_spectrum)
                        reading_json["raw_spectrum"] = raw_spectrum_to_json(*takes[i].raw_spectrum);
                    measured["readings"].push_back(std::move(reading_json));
                }
                if (avg) {
                    measured["lab"] = lab_to_json(*avg);
                    measured["lab_average"] = measured["lab"];
                }
                measured["take_count"] = takes.size();
                measured["target_take_count"] = target_take_count();
                measured["pass_delta_e_ab"] = pass_delta_e_limit();
            }

            const wxString hex = wxString(m_rows[static_cast<size_t>(row)].rgb_hex).Trim(true).Trim(false);
            if (!hex.empty())
                measured["rgb_hex"] = wx_to_u8(hex);

            const wxString notes = m_rows[static_cast<size_t>(row)].notes;
            if (!notes.empty())
                measured["notes"] = wx_to_u8(notes);
            measured["complete"] = measured.contains("lab") || measured.contains("rgb_hex");

            nlohmann::json result_record;
            result_record["swatch_id"] = wx_to_u8(json_scalar_to_text(m_rows[static_cast<size_t>(row)].manifest_record["swatch_id"]));
            result_record["measurement_condition"] = m_rows[static_cast<size_t>(row)].measurement_condition;
            result_record["manifest"] = m_rows[static_cast<size_t>(row)].manifest_record;
            result_record["measured"] = measured;
            out["records"].push_back(std::move(result_record));
        }

        std::ofstream file(wx_to_u8(dlg.GetPath()), std::ios::binary);
        if (!file) {
            MessageDialog(this, _L("Could not open the selected file for writing."), _L("Color swatch measurements"), wxOK | wxICON_ERROR).ShowModal();
            return;
        }
        file << out.dump(2);
        file.close();
        MessageDialog(this, _L("Measurements saved."), _L("Color swatch measurements"), wxOK | wxICON_INFORMATION).ShowModal();
    }

    void on_clear_values(wxCommandEvent &)
    {
        if (m_reference_white_backing != nullptr)
            m_reference_white_backing->Clear();
        if (m_reference_black_backing != nullptr)
            m_reference_black_backing->Clear();
        for (Row &row : m_rows) {
            row.takes.clear();
            row.rgb_hex.clear();
            row.notes.clear();
        }
        refresh_measurement_grid();
        update_summary();
        update_next_sample();
    }

    wxTextCtrl* reference_measurement_control(ReferenceMeasurementTarget target) const
    {
        switch (target) {
        case ReferenceMeasurementTarget::WhiteBacking: return m_reference_white_backing;
        case ReferenceMeasurementTarget::BlackBacking: return m_reference_black_backing;
        case ReferenceMeasurementTarget::None: break;
        }
        return nullptr;
    }

    wxString reference_measurement_label(ReferenceMeasurementTarget target) const
    {
        switch (target) {
        case ReferenceMeasurementTarget::WhiteBacking: return _L("white backing");
        case ReferenceMeasurementTarget::BlackBacking: return _L("black backing");
        case ReferenceMeasurementTarget::None: break;
        }
        return wxString();
    }

    bool serial_ready_for_reference_measurement()
    {
        if (!serial_connected()) {
            m_serial_status->SetLabel(_L("Disconnected"));
            return false;
        }
        if (m_pts_state != PtsProtocolState::Ready) {
            m_serial_status->SetLabel(_L("Spectro is not ready"));
            return false;
        }
        return true;
    }

    void store_reference_measurement(ReferenceMeasurementTarget target, const LabReading &reading)
    {
        wxTextCtrl *control = reference_measurement_control(target);
        if (control == nullptr)
            return;
        control->SetValue(format_reading(reading));
        if (m_serial_status != nullptr)
            m_serial_status->SetLabel(_L("Measured ") + reference_measurement_label(target));
    }

    void on_measure_reference(ReferenceMeasurementTarget target)
    {
        if (!serial_ready_for_reference_measurement())
            return;

        m_reference_measure_target = target;
        if (!send_pts_measure()) {
            m_reference_measure_target = ReferenceMeasurementTarget::None;
            if (m_serial_status != nullptr)
                m_serial_status->SetLabel(_L("Could not start reference measurement"));
        }
    }

    void on_toggle_serial(wxCommandEvent &)
    {
#ifdef _WIN32
        if (serial_connected()) {
            disconnect_serial();
            return;
        }

        const wxString port_name = m_port->GetValue().Trim(true).Trim(false);
        long baud = 0;
        if (port_name.empty() || !m_baud->GetValue().ToLong(&baud) || baud <= 0) {
            MessageDialog(this, _L("Enter a COM port and baud rate."), _L("Color swatch measurements"), wxOK | wxICON_WARNING).ShowModal();
            return;
        }

        const std::wstring device_path = L"\\\\.\\" + port_name.ToStdWstring();
        m_serial_handle = CreateFileW(device_path.c_str(),
                                      GENERIC_READ | GENERIC_WRITE,
                                      0,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
        if (m_serial_handle == INVALID_HANDLE_VALUE) {
            m_serial_status->SetLabel(_L("Open failed"));
            return;
        }

        DCB dcb {};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(m_serial_handle, &dcb)) {
            disconnect_serial();
            m_serial_status->SetLabel(_L("Config failed"));
            return;
        }
        dcb.BaudRate = DWORD(baud);
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        if (!SetCommState(m_serial_handle, &dcb)) {
            disconnect_serial();
            m_serial_status->SetLabel(_L("Config failed"));
            return;
        }

        COMMTIMEOUTS timeouts {};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 0;
        timeouts.WriteTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = 200;
        SetCommTimeouts(m_serial_handle, &timeouts);
        SetupComm(m_serial_handle, 4096, 1024);
        PurgeComm(m_serial_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

        m_serial_buffer.clear();
        m_pts_rx_buffer.clear();
        m_pts_measure_buffer.clear();
        if (m_serial_log)
            m_serial_log->Clear();
        m_connect_serial->SetLabel(_L("Disconnect"));
        m_serial_status->SetLabel(_L("Handshaking"));
        m_serial_timer->Start(150);
        start_pts_session();
#else
        m_serial_status->SetLabel(_L("Serial capture is Windows-only here"));
#endif
    }

    bool serial_ready_for_measurement()
    {
        if (!serial_connected()) {
            m_serial_status->SetLabel(_L("Disconnected"));
            return false;
        }
        if (m_rows.empty()) {
            MessageDialog(this, _L("Load a swatch manifest before measuring."), _L("Color swatch measurements"), wxOK | wxICON_WARNING).ShowModal();
            return false;
        }
        if (m_pts_state != PtsProtocolState::Ready) {
            m_serial_status->SetLabel(_L("Spectro is not ready"));
            return false;
        }
        return true;
    }

    void stop_measure_all(const wxString &status)
    {
        m_measure_all_active = false;
        m_measure_all_row = -1;
        m_measure_all_attempts = 0;
        m_measure_all_max_attempts = 0;
        if (m_serial_status != nullptr && !status.empty())
            m_serial_status->SetLabel(status);
        update_serial_buttons();
    }

    int measure_all_start_row()
    {
        const int row = active_row();
        if (row >= 0 && static_cast<size_t>(row) < m_rows.size()) {
            sync_row_takes_from_grid(row);
            if (int(m_rows[static_cast<size_t>(row)].takes.size()) < target_take_count() || failed_take_for_row(row))
                return row;
            return find_next_incomplete_row(row + 1);
        }
        return find_next_incomplete_row(0);
    }

    void start_measure_all_for_row(int row)
    {
#ifdef _WIN32
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            return;

        sync_row_takes_from_grid(row);
        m_measure_all_active = true;
        m_measure_all_row = row;
        m_measure_all_attempts = 0;
        const int target = target_take_count();
        m_measure_all_max_attempts = std::max(target * 3, target + 8);
        select_measurement_row(row);
        continue_measure_all();
#endif
    }

    void continue_measure_all()
    {
#ifdef _WIN32
        if (!m_measure_all_active)
            return;
        if (m_measure_all_row < 0 || static_cast<size_t>(m_measure_all_row) >= m_rows.size()) {
            stop_measure_all(_L("Measure All stopped"));
            return;
        }
        if (!serial_connected()) {
            stop_measure_all(_L("Measure All stopped: disconnected"));
            return;
        }
        if (m_pts_state != PtsProtocolState::Ready)
            return;

        select_measurement_row(m_measure_all_row);
        sync_row_takes_from_grid(m_measure_all_row);

        const int target = target_take_count();
        std::vector<MeasurementTake> &takes = m_rows[static_cast<size_t>(m_measure_all_row)].takes;
        if (int(takes.size()) >= target) {
            if (const std::optional<FailedTake> failed = failed_take_for_row(m_measure_all_row)) {
                const size_t take_number = failed->index + 1;
                takes.erase(takes.begin() + static_cast<std::ptrdiff_t>(failed->index));
                update_primary_color_from_anchor_measurement(m_measure_all_row);
                refresh_measurement_grid();
                select_measurement_row(m_measure_all_row);
                const wxString message = wxString::Format(_L("Removed failed take %zu (dE %.2f); re-measuring"), take_number, failed->delta_e);
                if (m_serial_status != nullptr)
                    m_serial_status->SetLabel(message);
                log_serial(_L("Measure All ") + message);
            } else {
                stop_measure_all(wxString::Format(_L("Measure All complete: %d/%d takes"), int(takes.size()), target));
                return;
            }
        }

        if (m_measure_all_attempts >= m_measure_all_max_attempts) {
            stop_measure_all(wxString::Format(_L("Measure All stopped after %d attempts"), m_measure_all_attempts));
            return;
        }

        const int next_take = int(takes.size()) + 1;
        ++m_measure_all_attempts;
        if (m_serial_status != nullptr)
            m_serial_status->SetLabel(wxString::Format(_L("Measure All: take %d/%d"), std::min(next_take, target), target));
        if (!send_pts_measure())
            stop_measure_all(_L("Measure All stopped: could not start measurement"));
#endif
    }

    void on_measure_serial(wxCommandEvent &)
    {
#ifdef _WIN32
        if (!serial_ready_for_measurement())
            return;
        stop_measure_all(wxString());
        send_pts_measure();
#endif
    }

    void on_measure_all_serial(wxCommandEvent &)
    {
#ifdef _WIN32
        if (!serial_ready_for_measurement())
            return;

        const int row = measure_all_start_row();
        if (row < 0) {
            if (m_serial_status)
                m_serial_status->SetLabel(_L("All samples complete"));
            return;
        }

        start_measure_all_for_row(row);
#endif
    }

    void on_serial_timer(wxTimerEvent &)
    {
#ifdef _WIN32
        if (!serial_connected())
            return;

        DWORD errors = 0;
        COMSTAT stat {};
        if (!ClearCommError(m_serial_handle, &errors, &stat))
            return;
        if (stat.cbInQue == 0)
            return;

        char buffer[512];
        DWORD bytes_read = 0;
        const DWORD to_read = std::min<DWORD>(DWORD(sizeof(buffer)), stat.cbInQue);
        if (ReadFile(m_serial_handle, buffer, to_read, &bytes_read, nullptr) && bytes_read > 0)
            consume_serial_bytes(std::string(buffer, buffer + bytes_read));
#endif
    }

    void log_serial(const wxString &line)
    {
        if (!m_serial_log)
            return;
        if (m_serial_log->GetLastPosition() > 120000) {
            wxString tail = m_serial_log->GetValue().Right(80000);
            m_serial_log->SetValue(tail);
            m_serial_log->SetInsertionPointEnd();
        }
        m_serial_log->AppendText(line + "\n");
    }

#ifdef _WIN32
    bool write_serial_bytes(const std::vector<uint8_t> &bytes, const wxString &label)
    {
        if (!serial_connected() || bytes.empty())
            return false;

        DWORD written = 0;
        if (!WriteFile(m_serial_handle, bytes.data(), DWORD(bytes.size()), &written, nullptr) || written != bytes.size()) {
            m_serial_status->SetLabel(_L("Write failed"));
            return false;
        }

        log_serial(label + " " + bytes_to_hex(bytes));
        return true;
    }

    void start_pts_session()
    {
        m_pts_state = PtsProtocolState::Handshaking;
        m_pts_session = 0;
        m_pts_product = 0;
        m_pts_rx_buffer.clear();
        m_pts_measure_buffer.clear();
        m_pts_config_commands.clear();
        m_pts_config_index = 0;
        m_pts_pending_command = 0;
        m_pts_config_for_measurement = false;
        m_pts_dual_measurement_active = false;
        m_pts_requested_specular_mode = PtsSpecularMode::SCI;
        m_pts_current_measurement_mode = PtsSpecularMode::SCI;
        m_reference_measure_target = ReferenceMeasurementTarget::None;
        m_pts_pending_sci_measurement.reset();
        m_pts_pending_sce_measurement.reset();
        update_serial_buttons();

        write_serial_bytes({ 0x55, 0xAA, 0xAA, 0x00 }, _L("TX"));
        write_serial_bytes({ 0x55, 0xAA, 0xA1, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02 }, _L("TX"));
    }

    bool send_pts_measure_frame(PtsSpecularMode mode)
    {
        const std::vector<uint8_t> frame = make_pts_data_frame(m_pts_session, 0x23, { 0x00 });
        if (write_serial_bytes(frame, _L("TX"))) {
            m_pts_current_measurement_mode = mode;
            m_pts_state = PtsProtocolState::Measuring;
            m_pts_measure_buffer.clear();
            m_serial_status->SetLabel(_L("Measuring ") + u8_to_wx(specular_mode_name(mode)));
            update_serial_buttons();
            return true;
        }
        return false;
    }

    bool send_pts_specular_config_for_measurement(PtsSpecularMode mode)
    {
        if (!serial_connected())
            return false;

        m_pts_config_for_measurement = true;
        m_pts_requested_specular_mode = mode;
        m_pts_pending_command = 0xB4;
        m_pts_state = PtsProtocolState::Configuring;
        m_serial_status->SetLabel(_L("Configuring ") + u8_to_wx(specular_mode_name(mode)));
        update_serial_buttons();

        const std::vector<uint8_t> frame = make_pts_data_frame(m_pts_session, 0xB4, { 0x01, pts_specular_mode_code(mode) });
        if (write_serial_bytes(frame, _L("TX")))
            return true;

        m_pts_config_for_measurement = false;
        m_pts_pending_command = 0;
        m_pts_state = PtsProtocolState::Ready;
        update_serial_buttons();
        return false;
    }

    bool send_pts_measure()
    {
        if (m_pts_product != 2) {
            m_pts_dual_measurement_active = false;
            return send_pts_measure_frame(PtsSpecularMode::SCI);
        }

        m_pts_dual_measurement_active = true;
        m_pts_pending_sci_measurement.reset();
        m_pts_pending_sce_measurement.reset();
        return send_pts_specular_config_for_measurement(PtsSpecularMode::SCI);
    }

    void queue_pts_lab_config()
    {
        m_pts_config_commands.clear();
        m_pts_config_commands.push_back({ 0xB4, { 0x08, 0x00 }, _L("CIE Lab") });
        m_pts_config_commands.push_back({ 0xB4, { 0x06, pts_illuminant_code(m_illuminant->GetValue()) }, _L("Illuminant") });
        m_pts_config_commands.push_back({ 0xB4, { 0x07, pts_observer_code(m_observer->GetValue()) }, _L("Observer") });
        m_pts_config_commands.push_back({ 0xB4, { 0x01, 0x00 }, _L("SCI") });
        m_pts_config_index = 0;
        m_pts_pending_command = 0;
        send_next_pts_config_command();
    }

    void send_next_pts_config_command()
    {
        if (m_pts_config_index >= m_pts_config_commands.size()) {
            m_pts_state = PtsProtocolState::Ready;
            m_pts_pending_command = 0;
            m_pts_config_for_measurement = false;
            m_serial_status->SetLabel(_L("Ready"));
            update_serial_buttons();
            return;
        }

        const PtsCommand &command = m_pts_config_commands[m_pts_config_index++];
        m_pts_pending_command = command.command;
        m_pts_state = PtsProtocolState::Configuring;
        m_serial_status->SetLabel(_L("Configuring ") + command.label);
        update_serial_buttons();
        write_serial_bytes(make_pts_data_frame(m_pts_session, command.command, command.payload), _L("TX"));
    }
#endif

    void consume_serial_bytes(const std::string &chunk)
    {
        std::vector<uint8_t> bytes(chunk.begin(), chunk.end());
        log_serial(_L("RX ") + bytes_to_hex(bytes));
        m_pts_rx_buffer.insert(m_pts_rx_buffer.end(), bytes.begin(), bytes.end());
        parse_pts_frames();

        if (chunk_is_mostly_text(chunk))
            consume_serial_text(chunk);

        if (m_pts_rx_buffer.size() > 8192)
            m_pts_rx_buffer.erase(m_pts_rx_buffer.begin(), m_pts_rx_buffer.end() - 2048);
    }

    void parse_pts_frames()
    {
        const uint8_t pts_rx_header[] = { 0xAA, 0x55 };
        for (;;) {
            auto header = std::search(m_pts_rx_buffer.begin(), m_pts_rx_buffer.end(), std::begin(pts_rx_header), std::end(pts_rx_header));
            if (header == m_pts_rx_buffer.end()) {
                if (m_pts_rx_buffer.size() > 3)
                    m_pts_rx_buffer.erase(m_pts_rx_buffer.begin(), m_pts_rx_buffer.end() - 1);
                return;
            }
            if (header != m_pts_rx_buffer.begin())
                m_pts_rx_buffer.erase(m_pts_rx_buffer.begin(), header);
            if (m_pts_rx_buffer.size() < 4)
                return;

            const uint8_t type = m_pts_rx_buffer[2];
            size_t frame_size = 0;
            if (type == 0xA2)
                frame_size = 13;
            else if (type == 0xA4 || type == 0xA5 || type == 0xA8 || type == 0xA9)
                frame_size = 4;
            else if (type == 0xA7)
                frame_size = 8;
            else if (type == 0xA6) {
                if (m_pts_rx_buffer.size() < 10)
                    return;
                frame_size = 10 + read_le16(m_pts_rx_buffer, 8);
            } else {
                m_pts_rx_buffer.erase(m_pts_rx_buffer.begin());
                continue;
            }

            if (m_pts_rx_buffer.size() < frame_size)
                return;

            std::vector<uint8_t> frame(m_pts_rx_buffer.begin(), m_pts_rx_buffer.begin() + frame_size);
            m_pts_rx_buffer.erase(m_pts_rx_buffer.begin(), m_pts_rx_buffer.begin() + frame_size);
            handle_pts_frame(type, frame);
        }
    }

    void handle_pts_frame(uint8_t type, const std::vector<uint8_t> &frame)
    {
        if (type == 0xA2) {
            handle_pts_handshake_ack(frame);
        } else if (type == 0xA6) {
            handle_pts_data_frame(frame);
        } else if (type == 0xA7) {
            if (m_pts_state == PtsProtocolState::Configuring) {
                if (m_pts_config_for_measurement) {
                    m_pts_config_for_measurement = false;
                    m_pts_pending_command = 0;
                    if (!send_pts_measure_frame(m_pts_requested_specular_mode)) {
                        m_pts_dual_measurement_active = false;
                        m_pts_state = PtsProtocolState::Ready;
                        if (m_measure_all_active)
                            stop_measure_all(_L("Measure All stopped: could not start measurement"));
                        else {
                            m_serial_status->SetLabel(_L("Could not start spectro measurement"));
                            update_serial_buttons();
                        }
                    }
                } else {
                    send_next_pts_config_command();
                }
            } else if (m_pts_state == PtsProtocolState::Measuring) {
                m_serial_status->SetLabel(_L("Waiting for reading"));
            }
        } else if (type == 0xA8) {
#ifdef _WIN32
            write_serial_bytes({ 0x55, 0xAA, 0xA9, m_pts_session }, _L("TX"));
#endif
        }
    }

    void handle_pts_handshake_ack(const std::vector<uint8_t> &frame)
    {
        if (frame.size() < 13 || frame[3] == 0xFF) {
            m_serial_status->SetLabel(_L("Handshake failed"));
            return;
        }

        m_pts_session = frame[3];
        const uint32_t version = read_le32(frame, 7);
        m_pts_product = (version >> 8) & 0x00FFFFFF;
        log_serial(wxString::Format(_L("PTS session %u product %u"), unsigned(m_pts_session), unsigned(m_pts_product)));

#ifdef _WIN32
        write_serial_bytes({ 0x55, 0xAA, 0xA3, m_pts_session }, _L("TX"));
#endif

        if (m_pts_product == 2)
            queue_pts_lab_config();
        else {
            m_pts_state = PtsProtocolState::Ready;
            m_serial_status->SetLabel(_L("Ready, non-PTS product"));
            update_serial_buttons();
        }
    }

    void handle_pts_data_frame(const std::vector<uint8_t> &frame)
    {
        if (frame.size() < 13)
            return;

        const uint32_t raw_seq = read_le32(frame, 4);
        const uint32_t seq = raw_seq & 0x3FFFFFFF;
        const bool is_last = (raw_seq & 0x40000000) == 0;
        const uint16_t len = read_le16(frame, 8);
        if (len < 3 || 10 + size_t(len) > frame.size())
            return;

        const size_t payload_size = size_t(len) - 2;
        if (payload_size == 0)
            return;

        const uint8_t operation = frame[10];
        std::vector<uint8_t> data(frame.begin() + 11, frame.begin() + 10 + payload_size);

#ifdef _WIN32
        std::vector<uint8_t> ack { 0x55, 0xAA, 0xA7, m_pts_session };
        append_le32(ack, seq + 1);
        write_serial_bytes(ack, _L("TX"));
#endif

        if (m_pts_state == PtsProtocolState::Configuring) {
            return;
        }

        if (operation != 0x23 && operation != 0x24)
            return;

        if (seq == 0)
            m_pts_measure_buffer.clear();
        m_pts_measure_buffer.insert(m_pts_measure_buffer.end(), data.begin(), data.end());

        if (is_last)
            finish_pts_measurement(operation);
    }

    void handle_pts_dual_measurement_result(PtsParsedMeasurement parsed)
    {
        parsed = parsed_with_requested_mode(std::move(parsed), m_pts_current_measurement_mode);

        if (parsed_has_sci(parsed) && parsed_has_sce(parsed)) {
            m_pts_dual_measurement_active = false;
            m_pts_pending_sci_measurement.reset();
            m_pts_pending_sce_measurement.reset();
            handle_completed_pts_measurement(parsed);
            return;
        }

        if (m_pts_current_measurement_mode == PtsSpecularMode::SCI) {
            m_pts_pending_sci_measurement = std::move(parsed);
            if (!send_pts_specular_config_for_measurement(PtsSpecularMode::SCE)) {
                m_pts_dual_measurement_active = false;
                m_serial_status->SetLabel(_L("Could not switch spectro to SCE"));
                update_serial_buttons();
                if (m_measure_all_active)
                    stop_measure_all(_L("Measure All stopped: could not switch to SCE"));
            }
            return;
        }

        m_pts_pending_sce_measurement = std::move(parsed);
        PtsParsedMeasurement merged = merge_pts_measurements(m_pts_pending_sci_measurement, m_pts_pending_sce_measurement);
        m_pts_dual_measurement_active = false;
        m_pts_pending_sci_measurement.reset();
        m_pts_pending_sce_measurement.reset();
        handle_completed_pts_measurement(merged);
    }

    void finish_pts_measurement(uint8_t operation)
    {
        const uint8_t illuminant = pts_illuminant_code(m_illuminant->GetValue());
        const uint8_t observer = pts_observer_code(m_observer->GetValue());
        if (const std::optional<PtsParsedMeasurement> parsed = parse_lab_from_pts_measurement(operation, m_pts_measure_buffer, illuminant, observer)) {
            if (m_pts_state == PtsProtocolState::Measuring)
                m_pts_state = PtsProtocolState::Ready;
            update_serial_buttons();
            if (pts_record_is_spectrum(m_pts_measure_buffer))
                log_serial(wxString::Format(_L("PTS spectrum converted to Lab D65/%s"), observer == 0 ? _L("2") : _L("10")));
            if (m_pts_dual_measurement_active)
                handle_pts_dual_measurement_result(*parsed);
            else
                handle_completed_pts_measurement(parsed_with_requested_mode(*parsed, m_pts_current_measurement_mode));
        } else {
            if (m_pts_state == PtsProtocolState::Measuring)
                m_pts_state = PtsProtocolState::Ready;
            m_pts_dual_measurement_active = false;
            m_reference_measure_target = ReferenceMeasurementTarget::None;
            m_pts_pending_sci_measurement.reset();
            m_pts_pending_sce_measurement.reset();
            m_serial_status->SetLabel(pts_record_is_spectrum(m_pts_measure_buffer) && illuminant != 0 ?
                                          _L("Spectrum conversion currently supports D65") :
                                          _L("Could not parse spectro reading"));
            update_serial_buttons();
            if (m_measure_all_active)
                CallAfter([this]() { continue_measure_all(); });
        }
        m_pts_measure_buffer.clear();
    }

    bool read_json_file(const wxString &path, nlohmann::json &out)
    {
        std::ifstream file(wx_to_u8(path), std::ios::binary);
        if (!file) {
            MessageDialog(this, _L("Could not open the selected JSON file."), _L("Color swatch measurements"), wxOK | wxICON_ERROR).ShowModal();
            return false;
        }

        try {
            file >> out;
        } catch (const std::exception &e) {
            MessageDialog(this,
                          _L("Could not parse the selected JSON file.") + "\n" + u8_to_wx(e.what()),
                          _L("Color swatch measurements"),
                          wxOK | wxICON_ERROR).ShowModal();
            return false;
        }
        return true;
    }

    std::map<unsigned int, std::string> primary_colors_from_manifest() const
    {
        std::map<unsigned int, std::string> colors;
        if (m_manifest.contains("primary_filaments") && m_manifest["primary_filaments"].is_array()) {
            for (const nlohmann::json &filament : m_manifest["primary_filaments"]) {
                if (!filament.is_object() || !filament.contains("slot") || !filament.contains("color_hex"))
                    continue;
                const std::optional<unsigned int> slot = json_slot_value(filament["slot"]);
                if (!slot || *slot < 1 || *slot > 4 || !filament["color_hex"].is_string())
                    continue;
                const std::optional<std::string> color = normalize_hex_color(filament["color_hex"].get<std::string>());
                if (color)
                    colors.emplace(*slot, *color);
            }
        }

        if (m_manifest.contains("primary_colors") && m_manifest["primary_colors"].is_object()) {
            for (auto it = m_manifest["primary_colors"].begin(); it != m_manifest["primary_colors"].end(); ++it) {
                if (!it.value().is_string())
                    continue;
                try {
                    const unsigned long slot_value = std::stoul(it.key());
                    if (slot_value < 1 || slot_value > 4)
                        continue;
                    const std::optional<std::string> color = normalize_hex_color(it.value().get<std::string>());
                    if (color)
                        colors[static_cast<unsigned int>(slot_value)] = *color;
                } catch (...) {
                }
            }
        }

        if (!m_manifest.contains("records") || !m_manifest["records"].is_array())
            return colors;

        for (const nlohmann::json &record : m_manifest["records"]) {
            if (record.contains("filaments") && record["filaments"].is_array()) {
                for (const nlohmann::json &filament : record["filaments"]) {
                    if (!filament.is_object() || !filament.contains("slot") || !filament.contains("color_hex"))
                        continue;
                    const std::optional<unsigned int> slot = json_slot_value(filament["slot"]);
                    if (!slot || *slot < 1 || *slot > 4 || !filament["color_hex"].is_string())
                        continue;
                    const std::optional<std::string> color = normalize_hex_color(filament["color_hex"].get<std::string>());
                    if (color)
                        colors.emplace(*slot, *color);
                }
            }

            if (!record.contains("filament_slots") || !record["filament_slots"].is_array() ||
                !record.contains("colors") || !record["colors"].is_array())
                continue;

            const nlohmann::json &slots = record["filament_slots"];
            const nlohmann::json &record_colors = record["colors"];
            const size_t count = std::min(slots.size(), record_colors.size());
            for (size_t i = 0; i < count; ++i) {
                const std::optional<unsigned int> slot = json_slot_value(slots[i]);
                if (!slot || *slot < 1 || *slot > 4 || !record_colors[i].is_string())
                    continue;
                const std::optional<std::string> color = normalize_hex_color(record_colors[i].get<std::string>());
                if (color)
                    colors.emplace(*slot, *color);
            }
        }

        return colors;
    }

    std::map<unsigned int, std::optional<double>> primary_tds_from_manifest() const
    {
        std::map<unsigned int, std::optional<double>> tds;
        if (m_manifest.contains("primary_filaments") && m_manifest["primary_filaments"].is_array()) {
            for (const nlohmann::json &filament : m_manifest["primary_filaments"]) {
                if (!filament.is_object() || !filament.contains("slot"))
                    continue;
                const std::optional<unsigned int> slot = json_slot_value(filament["slot"]);
                if (!slot || *slot < 1 || *slot > 4)
                    continue;
                tds.emplace(*slot, filament.contains("td") ? json_double_value(filament["td"]) : std::nullopt);
            }
        }

        if (m_manifest.contains("primary_td_values") && m_manifest["primary_td_values"].is_object()) {
            for (auto it = m_manifest["primary_td_values"].begin(); it != m_manifest["primary_td_values"].end(); ++it) {
                try {
                    const unsigned long slot_value = std::stoul(it.key());
                    if (slot_value < 1 || slot_value > 4)
                        continue;
                    tds[static_cast<unsigned int>(slot_value)] = it.value().is_null() ? std::nullopt : json_double_value(it.value());
                } catch (...) {
                }
            }
        }

        if (!m_manifest.contains("records") || !m_manifest["records"].is_array())
            return tds;

        for (const nlohmann::json &record : m_manifest["records"]) {
            if (record.contains("filaments") && record["filaments"].is_array()) {
                for (const nlohmann::json &filament : record["filaments"]) {
                    if (!filament.is_object() || !filament.contains("slot"))
                        continue;
                    const std::optional<unsigned int> slot = json_slot_value(filament["slot"]);
                    if (!slot || *slot < 1 || *slot > 4)
                        continue;
                    tds.emplace(*slot, filament.contains("td") ? json_double_value(filament["td"]) : std::nullopt);
                }
            }

            if (!record.contains("filament_slots") || !record["filament_slots"].is_array())
                continue;

            const nlohmann::json &slots = record["filament_slots"];
            const nlohmann::json *record_tds = record.contains("td_values") && record["td_values"].is_array() ? &record["td_values"] : nullptr;
            for (size_t i = 0; i < slots.size(); ++i) {
                const std::optional<unsigned int> slot = json_slot_value(slots[i]);
                if (!slot || *slot < 1 || *slot > 4)
                    continue;
                const std::optional<double> td = record_tds != nullptr && i < record_tds->size() && !(*record_tds)[i].is_null() ?
                    json_double_value((*record_tds)[i]) :
                    std::nullopt;
                tds.emplace(*slot, td);
            }
        }

        return tds;
    }

    void update_primary_color_controls_from_manifest()
    {
        const std::map<unsigned int, std::string> colors = primary_colors_from_manifest();
        m_updating_primary_colors = true;
        for (size_t i = 0; i < m_primary_color_pickers.size(); ++i) {
            wxColourPickerCtrl *picker = m_primary_color_pickers[i];
            if (picker == nullptr)
                continue;
            const unsigned int slot = static_cast<unsigned int>(i + 1);
            const auto it = colors.find(slot);
            picker->Enable(it != colors.end());
            picker->SetColour(it != colors.end() ? colour_from_hex(it->second) : wxColour(128, 128, 128));
        }
        m_updating_primary_colors = false;

        if (m_primary_color_status != nullptr)
            m_primary_color_status->SetLabel(colors.empty() ? _L("No primary colors in manifest") : _L("Loaded"));
    }

    void update_primary_td_controls_from_manifest()
    {
        const std::map<unsigned int, std::optional<double>> tds = primary_tds_from_manifest();
        m_updating_primary_tds = true;
        for (size_t i = 0; i < m_primary_td_inputs.size(); ++i) {
            wxTextCtrl *input = m_primary_td_inputs[i];
            if (input == nullptr)
                continue;
            const unsigned int slot = static_cast<unsigned int>(i + 1);
            const auto it = tds.find(slot);
            input->Enable(it != tds.end());
            input->ChangeValue(it != tds.end() ? optional_td_to_text(it->second) : wxString());
        }
        m_updating_primary_tds = false;

        if (m_primary_td_status != nullptr)
            m_primary_td_status->SetLabel(tds.empty() ? _L("No primary filaments in manifest") : _L("Loaded"));
    }

    static bool parse_primary_td_text(const wxString &text, std::optional<double> &td)
    {
        wxString trimmed = text;
        trimmed.Trim(true).Trim(false);
        if (trimmed.empty()) {
            td = std::nullopt;
            return true;
        }

        double value = 0.0;
        if (!trimmed.ToDouble(&value) || !std::isfinite(value) || value < 0.0)
            return false;

        td = value;
        return true;
    }

    static bool apply_primary_color_to_primary_filaments(nlohmann::json &manifest, unsigned int slot, const std::string &color)
    {
        if (!manifest.contains("primary_filaments") || !manifest["primary_filaments"].is_array())
            manifest["primary_filaments"] = nlohmann::json::array();

        for (nlohmann::json &filament : manifest["primary_filaments"]) {
            if (!filament.is_object() || !filament.contains("slot"))
                continue;
            const std::optional<unsigned int> filament_slot = json_slot_value(filament["slot"]);
            if (filament_slot && *filament_slot == slot) {
                filament["color_hex"] = color;
                return true;
            }
        }

        manifest["primary_filaments"].push_back({
            { "slot", slot },
            { "name", "" },
            { "short_label", std::to_string(slot) },
            { "color_hex", color },
            { "td", nullptr }
        });
        return true;
    }

    static bool apply_primary_td_to_primary_filaments(nlohmann::json &manifest, unsigned int slot, const std::optional<double> &td)
    {
        if (!manifest.contains("primary_filaments") || !manifest["primary_filaments"].is_array())
            manifest["primary_filaments"] = nlohmann::json::array();

        for (nlohmann::json &filament : manifest["primary_filaments"]) {
            if (!filament.is_object() || !filament.contains("slot"))
                continue;
            const std::optional<unsigned int> filament_slot = json_slot_value(filament["slot"]);
            if (filament_slot && *filament_slot == slot) {
                filament["td"] = td ? nlohmann::json(*td) : nlohmann::json(nullptr);
                return true;
            }
        }

        manifest["primary_filaments"].push_back({
            { "slot", slot },
            { "name", "" },
            { "short_label", std::to_string(slot) },
            { "color_hex", "" },
            { "td", td ? nlohmann::json(*td) : nlohmann::json(nullptr) }
        });
        return true;
    }

    static bool apply_primary_color_to_record(nlohmann::json &record, unsigned int slot, const std::string &color)
    {
        bool changed = false;

        if (record.contains("filaments") && record["filaments"].is_array()) {
            for (nlohmann::json &filament : record["filaments"]) {
                if (!filament.is_object() || !filament.contains("slot"))
                    continue;
                const std::optional<unsigned int> filament_slot = json_slot_value(filament["slot"]);
                if (filament_slot && *filament_slot == slot) {
                    filament["color_hex"] = color;
                    changed = true;
                }
            }
        }

        if (record.contains("filament_slots") && record["filament_slots"].is_array()) {
            nlohmann::json &colors = record["colors"];
            if (!colors.is_array())
                colors = nlohmann::json::array();
            const nlohmann::json &slots = record["filament_slots"];
            while (colors.size() < slots.size())
                colors.push_back("");
            for (size_t i = 0; i < slots.size(); ++i) {
                const std::optional<unsigned int> record_slot = json_slot_value(slots[i]);
                if (record_slot && *record_slot == slot) {
                    colors[i] = color;
                    changed = true;
                }
            }
        }

        if (record.contains("backing") && record["backing"].is_object() && record["backing"].contains("slot")) {
            const std::optional<unsigned int> backing_slot = json_slot_value(record["backing"]["slot"]);
            if (backing_slot && *backing_slot == slot) {
                record["backing"]["color_hex"] = color;
                changed = true;
            }
        }

        return changed;
    }

    static bool apply_primary_td_to_record(nlohmann::json &record, unsigned int slot, const std::optional<double> &td)
    {
        bool changed = false;

        if (record.contains("filaments") && record["filaments"].is_array()) {
            for (nlohmann::json &filament : record["filaments"]) {
                if (!filament.is_object() || !filament.contains("slot"))
                    continue;
                const std::optional<unsigned int> filament_slot = json_slot_value(filament["slot"]);
                if (filament_slot && *filament_slot == slot) {
                    filament["td"] = td ? nlohmann::json(*td) : nlohmann::json(nullptr);
                    changed = true;
                }
            }
        }

        if (record.contains("filament_slots") && record["filament_slots"].is_array()) {
            nlohmann::json &td_values = record["td_values"];
            if (!td_values.is_array())
                td_values = nlohmann::json::array();
            const nlohmann::json &slots = record["filament_slots"];
            while (td_values.size() < slots.size())
                td_values.push_back(nullptr);
            for (size_t i = 0; i < slots.size(); ++i) {
                const std::optional<unsigned int> record_slot = json_slot_value(slots[i]);
                if (record_slot && *record_slot == slot) {
                    td_values[i] = td ? nlohmann::json(*td) : nlohmann::json(nullptr);
                    changed = true;
                }
            }
        }

        if (record.contains("backing") && record["backing"].is_object() && record["backing"].contains("slot")) {
            const std::optional<unsigned int> backing_slot = json_slot_value(record["backing"]["slot"]);
            if (backing_slot && *backing_slot == slot) {
                record["backing"]["td"] = td ? nlohmann::json(*td) : nlohmann::json(nullptr);
                changed = true;
            }
        }

        return changed;
    }

    static std::optional<unsigned int> measured_anchor_slot(const nlohmann::json &record)
    {
        if (!record.is_object() || !record.contains("swatch_type") || !record["swatch_type"].is_string())
            return std::nullopt;
        if (record["swatch_type"].get<std::string>() != "reflective_anchor")
            return std::nullopt;
        if (measurement_condition_from_record(record) != "white_backing")
            return std::nullopt;

        if (record.contains("filament_slots") && record["filament_slots"].is_array() && record["filament_slots"].size() == 1)
            return json_slot_value(record["filament_slots"][0]);

        if (record.contains("filaments") && record["filaments"].is_array() && record["filaments"].size() == 1 &&
            record["filaments"][0].is_object() && record["filaments"][0].contains("slot"))
            return json_slot_value(record["filaments"][0]["slot"]);

        return std::nullopt;
    }

    bool write_manifest_json(const wxString &path)
    {
        std::ofstream file(wx_to_u8(path), std::ios::binary);
        if (!file)
            return false;
        file << m_manifest.dump(2);
        return bool(file);
    }

    bool apply_primary_color_to_manifest(unsigned int slot, const std::string &color)
    {
        if (!m_manifest.contains("records") || !m_manifest["records"].is_array())
            return false;

        if (!m_manifest.contains("primary_colors") || !m_manifest["primary_colors"].is_object())
            m_manifest["primary_colors"] = nlohmann::json::object();
        bool changed = !m_manifest["primary_colors"].contains(std::to_string(slot)) ||
            !m_manifest["primary_colors"][std::to_string(slot)].is_string() ||
            m_manifest["primary_colors"][std::to_string(slot)].get<std::string>() != color;
        m_manifest["primary_colors"][std::to_string(slot)] = color;
        changed = apply_primary_color_to_primary_filaments(m_manifest, slot, color) || changed;

        nlohmann::json &records = m_manifest["records"];
        for (size_t row = 0; row < records.size(); ++row) {
            if (!apply_primary_color_to_record(records[row], slot, color))
                continue;
            changed = true;
            for (size_t measurement_row = 0; measurement_row < m_rows.size(); ++measurement_row) {
                if (m_rows[measurement_row].source_manifest_index != row)
                    continue;
                m_rows[measurement_row].manifest_record =
                    manifest_record_for_measurement_condition(records[row], m_rows[measurement_row].measurement_condition);
                if (m_grid != nullptr) {
                    const int grid_row = grid_row_for_swatch(measurement_row);
                    if (grid_row >= 0)
                        m_grid->SetCellValue(grid_row, ColColors, json_array_to_text(m_rows[measurement_row].manifest_record, "colors"));
                }
            }
        }

        return changed;
    }

    void set_primary_color_picker(unsigned int slot, const std::string &color)
    {
        if (slot < 1 || slot > m_primary_color_pickers.size())
            return;
        wxColourPickerCtrl *picker = m_primary_color_pickers[slot - 1];
        if (picker == nullptr)
            return;
        m_updating_primary_colors = true;
        picker->Enable(true);
        picker->SetColour(colour_from_hex(color));
        m_updating_primary_colors = false;
    }

    void update_primary_color_from_anchor_measurement(int row)
    {
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            return;

        const std::optional<unsigned int> slot = measured_anchor_slot(m_rows[static_cast<size_t>(row)].manifest_record);
        if (!slot || *slot < 1 || *slot > m_primary_color_pickers.size())
            return;

        const std::optional<LabReading> avg = average_reading(readings_from_takes(m_rows[static_cast<size_t>(row)].takes));
        if (!avg)
            return;

        const std::string color = colour_to_hex(lab_to_srgb_colour(*avg));
        m_rows[static_cast<size_t>(row)].rgb_hex = u8_to_wx(color);
        const bool changed = apply_primary_color_to_manifest(*slot, color);
        set_primary_color_picker(*slot, color);

        if (!changed)
            return;

        const bool saved = !m_manifest_path->GetValue().empty() && write_manifest_json(m_manifest_path->GetValue());
        if (m_primary_color_status != nullptr) {
            m_primary_color_status->SetLabel(saved ?
                wxString::Format(_L("Slot %u measured as %s"), *slot, u8_to_wx(color)) :
                wxString::Format(_L("Slot %u measured in memory"), *slot));
        }
    }

    void on_primary_color_changed(unsigned int slot)
    {
        if (m_updating_primary_colors || slot < 1 || slot > m_primary_color_pickers.size())
            return;
        wxColourPickerCtrl *picker = m_primary_color_pickers[slot - 1];
        if (picker == nullptr || !picker->IsEnabled())
            return;

        const std::string color = colour_to_hex(picker->GetColour());
        if (!apply_primary_color_to_manifest(slot, color))
            return;

        const bool saved = !m_manifest_path->GetValue().empty() && write_manifest_json(m_manifest_path->GetValue());
        if (m_primary_color_status != nullptr) {
            m_primary_color_status->SetLabel(saved ?
                wxString::Format(_L("Slot %u saved as %s"), slot, u8_to_wx(color)) :
                wxString::Format(_L("Slot %u updated in memory"), slot));
        }
    }

    void on_primary_td_changed(unsigned int slot)
    {
        if (m_updating_primary_tds || slot < 1 || slot > m_primary_td_inputs.size())
            return;
        wxTextCtrl *input = m_primary_td_inputs[slot - 1];
        if (input == nullptr || !input->IsEnabled())
            return;
        if (!m_manifest.contains("records") || !m_manifest["records"].is_array())
            return;

        std::optional<double> td;
        if (!parse_primary_td_text(input->GetValue(), td)) {
            if (m_primary_td_status != nullptr)
                m_primary_td_status->SetLabel(wxString::Format(_L("Slot %u TD is not a number"), slot));
            return;
        }

        if (!m_manifest.contains("primary_td_values") || !m_manifest["primary_td_values"].is_object())
            m_manifest["primary_td_values"] = nlohmann::json::object();
        m_manifest["primary_td_values"][std::to_string(slot)] = td ? nlohmann::json(*td) : nlohmann::json(nullptr);
        bool changed = apply_primary_td_to_primary_filaments(m_manifest, slot, td);

        nlohmann::json &records = m_manifest["records"];
        for (size_t row = 0; row < records.size(); ++row) {
            if (!apply_primary_td_to_record(records[row], slot, td))
                continue;
            changed = true;
            for (size_t measurement_row = 0; measurement_row < m_rows.size(); ++measurement_row) {
                if (m_rows[measurement_row].source_manifest_index != row)
                    continue;
                m_rows[measurement_row].manifest_record =
                    manifest_record_for_measurement_condition(records[row], m_rows[measurement_row].measurement_condition);
                if (m_grid != nullptr) {
                    const int grid_row = grid_row_for_swatch(measurement_row);
                    if (grid_row >= 0)
                        m_grid->SetCellValue(grid_row, ColTd, json_array_to_text(m_rows[measurement_row].manifest_record, "td_values"));
                }
            }
        }

        if (!changed)
            return;

        const bool saved = !m_manifest_path->GetValue().empty() && write_manifest_json(m_manifest_path->GetValue());
        if (m_primary_td_status != nullptr) {
            m_primary_td_status->SetLabel(saved ?
                (td ? wxString::Format(_L("Slot %u TD saved as %.3f"), slot, *td) :
                      wxString::Format(_L("Slot %u TD cleared"), slot)) :
                wxString::Format(_L("Slot %u TD updated in memory"), slot));
        }
    }

    bool load_manifest(const wxString &path)
    {
        nlohmann::json manifest;
        if (!read_json_file(path, manifest))
            return false;

        if (!manifest.contains("records") || !manifest["records"].is_array()) {
            MessageDialog(this, _L("The selected JSON file is not a calibration swatch manifest."), _L("Color swatch measurements"), wxOK | wxICON_WARNING).ShowModal();
            return false;
        }

        m_manifest = std::move(manifest);
        m_manifest_path->SetValue(path);
        m_rows.clear();
        m_grid_rows.clear();
        m_current_swatch = 0;

        const nlohmann::json &records = m_manifest["records"];
        for (size_t record_index = 0; record_index < records.size(); ++record_index) {
            const nlohmann::json &record = records[record_index];
            for (const std::string &condition : measurement_conditions_for_record(record)) {
                Row row;
                row.manifest_record = manifest_record_for_measurement_condition(record, condition);
                row.source_manifest_index = record_index;
                row.measurement_condition = condition;
                m_rows.push_back(std::move(row));
            }
        }

        update_primary_color_controls_from_manifest();
        update_primary_td_controls_from_manifest();
        refresh_measurement_grid();

        if (!records.empty())
            select_measurement_row(0);
        update_summary();
        update_next_sample();
        update_serial_buttons();
        return true;
    }

    void fill_manifest_row(int row, const nlohmann::json &record)
    {
        m_grid->SetCellValue(row, ColId, manifest_display_id(record));
        m_grid->SetCellValue(row, ColType, measurement_type_text(record));
        m_grid->SetCellValue(row, ColSlots, json_array_to_text(record, "filament_slots"));
        m_grid->SetCellValue(row, ColRatios, json_array_to_text(record, "ratios"));
        m_grid->SetCellValue(row, ColPercentages, json_array_to_text(record, "percentages"));
        m_grid->SetCellValue(row, ColColors, json_array_to_text(record, "colors"));
        m_grid->SetCellValue(row, ColTd, json_array_to_text(record, "td_values"));
        if (record.contains("backing") && record["backing"].is_object())
            m_grid->SetCellValue(row, ColBacking, json_string_value(record["backing"], "type"));
        m_grid->SetCellValue(row, ColMeasurement, measurement_condition_label(measurement_condition_from_record(record)));
        m_grid->SetCellValue(row, ColThickness, json_string_value(record, "total_thickness_mm"));
        if (record.contains("plate_index"))
            m_grid->SetCellValue(row, ColPlate, json_scalar_to_text(record["plate_index"]));

        for (int col = 0; col < ColCount; ++col)
            m_grid->SetReadOnly(row, col, col != ColReadings && col != ColHex && col != ColNotes);
    }

    bool row_has_measurement(int row) const
    {
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            return false;
        if (!wxString(m_rows[static_cast<size_t>(row)].rgb_hex).Trim(true).Trim(false).empty())
            return true;
        return !readings_for_row(row).empty();
    }

    bool row_is_complete(int row) const
    {
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            return false;
        return int(readings_for_row(row).size()) >= target_take_count();
    }

    void update_summary()
    {
        size_t measured = 0;
        size_t complete = 0;
        for (int row = 0; row < static_cast<int>(m_rows.size()); ++row) {
            if (row_has_measurement(row))
                ++measured;
            if (row_is_complete(row))
                ++complete;
        }

        if (m_rows.empty())
            m_summary->SetLabel(_L("No manifest loaded"));
        else
            m_summary->SetLabel(wxString::Format(_L("%zu measurement rows loaded. %zu have data. %zu reached %d takes."),
                                                 m_rows.size(),
                                                 measured,
                                                 complete,
                                                 target_take_count()));
    }

    void update_next_sample()
    {
        if (m_rows.empty()) {
            m_next_sample->SetLabel(_L("Next sample: load a manifest"));
            return;
        }

        int row = active_row();
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            row = 0;
        const int target = target_take_count();
        const size_t takes = readings_for_row(row).size();
        m_next_sample->SetLabel(wxString::Format(_L("Next sample: %s %s  take %zu/%d"),
                                                 manifest_display_id(m_rows[static_cast<size_t>(row)].manifest_record),
                                                 measurement_condition_label(m_rows[static_cast<size_t>(row)].measurement_condition),
                                                 std::min<size_t>(takes + 1, size_t(target)),
                                                 target));
    }

    bool serial_connected() const
    {
#ifdef _WIN32
        return m_serial_handle != INVALID_HANDLE_VALUE;
#else
        return false;
#endif
    }

    void update_serial_buttons()
    {
        const bool ready = serial_connected() && m_pts_state == PtsProtocolState::Ready && !m_rows.empty();
        if (m_measure_serial)
            m_measure_serial->Enable(ready && !m_measure_all_active);
        if (m_measure_all_serial)
            m_measure_all_serial->Enable(ready && !m_measure_all_active);
    }

    void disconnect_serial()
    {
        m_measure_all_active = false;
        m_measure_all_row = -1;
        m_measure_all_attempts = 0;
        m_measure_all_max_attempts = 0;
        if (m_serial_timer)
            m_serial_timer->Stop();
#ifdef _WIN32
        if (m_serial_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_serial_handle);
            m_serial_handle = INVALID_HANDLE_VALUE;
        }
#endif
        m_pts_state = PtsProtocolState::Disconnected;
        m_pts_session = 0;
        m_pts_product = 0;
        m_pts_rx_buffer.clear();
        m_pts_measure_buffer.clear();
        m_pts_config_commands.clear();
        m_pts_config_index = 0;
        m_pts_pending_command = 0;
        m_pts_config_for_measurement = false;
        m_pts_dual_measurement_active = false;
        m_pts_requested_specular_mode = PtsSpecularMode::SCI;
        m_pts_current_measurement_mode = PtsSpecularMode::SCI;
        m_reference_measure_target = ReferenceMeasurementTarget::None;
        m_pts_pending_sci_measurement.reset();
        m_pts_pending_sce_measurement.reset();
        if (m_connect_serial)
            m_connect_serial->SetLabel(_L("Connect"));
        if (m_serial_status)
            m_serial_status->SetLabel(_L("Disconnected"));
        update_serial_buttons();
    }

    void consume_serial_text(const std::string &chunk)
    {
        m_serial_buffer += chunk;
        for (;;) {
            const size_t pos = m_serial_buffer.find_first_of("\r\n");
            if (pos == std::string::npos)
                break;
            const std::string line = m_serial_buffer.substr(0, pos);
            m_serial_buffer.erase(0, pos + 1);
            if (const std::optional<LabReading> reading = parse_lab_reading_from_serial_text(line)) {
                if (m_reference_measure_target != ReferenceMeasurementTarget::None) {
                    const ReferenceMeasurementTarget target = m_reference_measure_target;
                    m_reference_measure_target = ReferenceMeasurementTarget::None;
                    store_reference_measurement(target, *reading);
                } else {
                    append_reading_to_row(active_row(), *reading, _L("Spectro"));
                }
            }
        }

        if (const std::optional<LabReading> reading = parse_lab_reading_from_serial_text(m_serial_buffer)) {
            if (m_reference_measure_target != ReferenceMeasurementTarget::None) {
                const ReferenceMeasurementTarget target = m_reference_measure_target;
                m_reference_measure_target = ReferenceMeasurementTarget::None;
                store_reference_measurement(target, *reading);
            } else {
                append_reading_to_row(active_row(), *reading, _L("Spectro"));
            }
            m_serial_buffer.clear();
        } else if (m_serial_buffer.size() > 2048) {
            m_serial_buffer.erase(0, m_serial_buffer.size() - 512);
        }
    }

    wxTextCtrl *m_manifest_path = nullptr;
    wxTextCtrl *m_instrument = nullptr;
    wxTextCtrl *m_illuminant = nullptr;
    wxTextCtrl *m_observer = nullptr;
    wxTextCtrl *m_geometry = nullptr;
    wxTextCtrl *m_reference_white_backing = nullptr;
    wxTextCtrl *m_reference_black_backing = nullptr;
    std::array<wxColourPickerCtrl*, 4> m_primary_color_pickers {};
    std::array<wxTextCtrl*, 4> m_primary_td_inputs {};
    wxStaticText *m_primary_color_status = nullptr;
    wxStaticText *m_primary_td_status = nullptr;
    wxStaticText *m_next_sample = nullptr;
    wxSpinCtrl *m_target_takes = nullptr;
    wxSpinCtrlDouble *m_pass_delta_e = nullptr;
    wxCheckBox *m_auto_advance = nullptr;
    wxTextCtrl *m_manual_l = nullptr;
    wxTextCtrl *m_manual_a = nullptr;
    wxTextCtrl *m_manual_b = nullptr;
    wxTextCtrl *m_port = nullptr;
    wxTextCtrl *m_baud = nullptr;
    wxButton *m_connect_serial = nullptr;
    wxButton *m_measure_serial = nullptr;
    wxButton *m_measure_all_serial = nullptr;
    wxStaticText *m_serial_status = nullptr;
    wxTextCtrl *m_serial_log = nullptr;
    wxStaticText *m_summary = nullptr;
    wxGrid *m_grid = nullptr;
    wxTimer *m_serial_timer = nullptr;
    nlohmann::json m_manifest;
    std::vector<Row> m_rows;
    std::vector<GridRowRef> m_grid_rows;
    int m_current_swatch = -1;
    bool m_refreshing_grid = false;
    bool m_updating_primary_colors = false;
    bool m_updating_primary_tds = false;
    bool m_measure_all_active = false;
    int m_measure_all_row = -1;
    int m_measure_all_attempts = 0;
    int m_measure_all_max_attempts = 0;
    ReferenceMeasurementTarget m_reference_measure_target = ReferenceMeasurementTarget::None;
    std::string m_serial_buffer;
    PtsProtocolState m_pts_state = PtsProtocolState::Disconnected;
    uint8_t m_pts_session = 0;
    uint32_t m_pts_product = 0;
    std::vector<uint8_t> m_pts_rx_buffer;
    std::vector<uint8_t> m_pts_measure_buffer;
    std::vector<PtsCommand> m_pts_config_commands;
    size_t m_pts_config_index = 0;
    uint8_t m_pts_pending_command = 0;
    bool m_pts_config_for_measurement = false;
    bool m_pts_dual_measurement_active = false;
    PtsSpecularMode m_pts_requested_specular_mode = PtsSpecularMode::SCI;
    PtsSpecularMode m_pts_current_measurement_mode = PtsSpecularMode::SCI;
    std::optional<PtsParsedMeasurement> m_pts_pending_sci_measurement;
    std::optional<PtsParsedMeasurement> m_pts_pending_sce_measurement;
#ifdef _WIN32
    HANDLE m_serial_handle = INVALID_HANDLE_VALUE;
#endif
};

} // namespace

wxString get_calibration_type_name(CalibMode cali_mode)
{
    switch (cali_mode) {
    case CalibMode::Calib_PA_Line:
        return _L("Flow Dynamics");
    case CalibMode::Calib_Flow_Rate:
        return _L("Flow Rate");
    case CalibMode::Calib_Vol_speed_Tower:
        return _L("Max Volumetric Speed");
    case CalibMode::Calib_Temp_Tower:
        return _L("Temperature");
    case CalibMode::Calib_Retraction_tower:
        return _L("Retraction");
    default:
        return "";
    }
}

MObjectPanel::MObjectPanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name)
{
    wxPanel::Create(parent, id, pos, SELECT_MACHINE_ITEM_SIZE, style, name);
    Bind(wxEVT_PAINT, &MObjectPanel::OnPaint, this);
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));


    m_printer_status_offline = ScalableBitmap(this, "printer_status_offline", 12);
    m_printer_status_busy = ScalableBitmap(this, "printer_status_busy", 12);
    m_printer_status_idle = ScalableBitmap(this, "printer_status_idle", 12);
    m_printer_status_lock = ScalableBitmap(this, "printer_status_lock", 16);
    m_printer_in_lan = ScalableBitmap(this, "printer_in_lan", 16);

    Bind(wxEVT_ENTER_WINDOW, &MObjectPanel::on_mouse_enter, this);
    Bind(wxEVT_LEAVE_WINDOW, &MObjectPanel::on_mouse_leave, this);
    Bind(wxEVT_LEFT_UP, &MObjectPanel::on_mouse_left_up, this);
}


MObjectPanel::~MObjectPanel() {}


void MObjectPanel::set_printer_state(PrinterState state)
{
    m_state = state;
    Refresh();
}

void MObjectPanel::OnPaint(wxPaintEvent & event)
{
    wxPaintDC dc(this);
    doRender(dc);
}

void MObjectPanel::render(wxDC& dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({ 0, 0 }, size, &dc, { 0, 0 });

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif
}

void MObjectPanel::doRender(wxDC& dc)
{
    auto   left = 10;
    wxSize size = GetSize();
    dc.SetPen(*wxTRANSPARENT_PEN);

    auto dwbitmap = m_printer_status_offline;
    if (m_state == PrinterState::IDLE) { dwbitmap = m_printer_status_idle; }
    if (m_state == PrinterState::BUSY) { dwbitmap = m_printer_status_busy; }
    if (m_state == PrinterState::OFFLINE) { dwbitmap = m_printer_status_offline; }
    if (m_state == PrinterState::LOCK) { dwbitmap = m_printer_status_lock; }
    if (m_state == PrinterState::IN_LAN) { dwbitmap = m_printer_in_lan; }

    // dc.DrawCircle(left, size.y / 2, 3);
    dc.DrawBitmap(dwbitmap.bmp(), wxPoint(left, (size.y - dwbitmap.GetBmpSize().y) / 2));

    left += dwbitmap.GetBmpSize().x + 8;
    dc.SetFont(Label::Body_13);
    dc.SetBackgroundMode(wxTRANSPARENT);
    dc.SetTextForeground(StateColor::darkModeColorFor(SELECT_MACHINE_GREY900));
    wxString dev_name = "";
    if (m_info) {
        dev_name = from_u8(m_info->dev_name);

        if (m_state == PrinterState::IN_LAN) {
            dev_name += _L("(LAN)");
        }
    }
    auto        sizet = dc.GetTextExtent(dev_name);
    auto        text_end = size.x - FromDIP(15);

    wxString finally_name = dev_name;
    if (sizet.x > (text_end - left)) {
        auto limit_width = text_end - left - dc.GetTextExtent("...").x - 15;
        for (auto i = 0; i < dev_name.length(); i++) {
            auto curr_width = dc.GetTextExtent(dev_name.substr(0, i));
            if (curr_width.x >= limit_width) {
                finally_name = dev_name.substr(0, i) + "...";
                break;
            }
        }
    }

    dc.DrawText(finally_name, wxPoint(left, (size.y - sizet.y) / 2));


    if (m_hover) {
        dc.SetPen(SELECT_MACHINE_BRAND);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, size.x, size.y);
    }

}

void MObjectPanel::update_machine_info(MachineObject* info, bool is_my_devices)
{
    m_info = info;
    m_is_my_devices = is_my_devices;
    Refresh();
}

void MObjectPanel::on_mouse_enter(wxMouseEvent& evt)
{
    m_hover = true;
    Refresh();
}

void MObjectPanel::on_mouse_leave(wxMouseEvent& evt)
{
    m_hover = false;
    Refresh();
}

void MObjectPanel::on_mouse_left_up(wxMouseEvent& evt)
{
    if (m_is_my_devices) {
        if (m_info && m_info->is_lan_mode_printer()) {
            if (m_info->has_access_right() && m_info->is_avaliable()) {
                Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
                if (!dev) return;
                dev->set_selected_machine(m_info->dev_id);
            }
        }
        else {
            Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
            if (!dev) return;
            dev->set_selected_machine(m_info->dev_id);
        }
        wxCommandEvent event(EVT_DISSMISS_MACHINE_LIST);
        event.SetEventObject(this->GetParent()->GetParent());
        wxPostEvent(this, event);
    }
}

SelectMObjectPopup::SelectMObjectPopup(wxWindow* parent)
    :PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS), m_dismiss(false)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__


    SetSize(SELECT_MACHINE_POPUP_SIZE);
    SetMinSize(SELECT_MACHINE_POPUP_SIZE);
    SetMaxSize(SELECT_MACHINE_POPUP_SIZE);

    Freeze();
    wxBoxSizer* m_sizer_main = new wxBoxSizer(wxVERTICAL);
    SetBackgroundColour(SELECT_MACHINE_GREY400);



    m_scrolledWindow = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, SELECT_MACHINE_LIST_SIZE, wxHSCROLL | wxVSCROLL);
    m_scrolledWindow->SetBackgroundColour(*wxWHITE);
    m_scrolledWindow->SetMinSize(SELECT_MACHINE_LIST_SIZE);
    m_scrolledWindow->SetScrollRate(0, 5);
    auto m_sizxer_scrolledWindow = new wxBoxSizer(wxVERTICAL);
    m_scrolledWindow->SetSizer(m_sizxer_scrolledWindow);
    m_scrolledWindow->Layout();
    m_sizxer_scrolledWindow->Fit(m_scrolledWindow);

    m_sizer_my_devices = new wxBoxSizer(wxVERTICAL);
    m_sizxer_scrolledWindow->Add(m_sizer_my_devices, 0, wxEXPAND, 0);


    m_sizer_main->Add(m_scrolledWindow, 0, wxALL | wxEXPAND, FromDIP(2));

    SetSizer(m_sizer_main);
    Layout();
    Thaw();

#ifdef __APPLE__
    m_scrolledWindow->Bind(wxEVT_LEFT_UP, &SelectMObjectPopup::OnLeftUp, this);
#endif // __APPLE__

    m_refresh_timer = new wxTimer();
    m_refresh_timer->SetOwner(this);
    Bind(EVT_UPDATE_USER_MLIST, &SelectMObjectPopup::update_machine_list, this);
    Bind(wxEVT_TIMER, &SelectMObjectPopup::on_timer, this);
    Bind(EVT_DISSMISS_MACHINE_LIST, &SelectMObjectPopup::on_dissmiss_win, this);
}

SelectMObjectPopup::~SelectMObjectPopup() { delete m_refresh_timer; }

void SelectMObjectPopup::Popup(wxWindow* WXUNUSED(focus))
{
    BOOST_LOG_TRIVIAL(trace) << "get_print_info: start";
    if (m_refresh_timer) {
        m_refresh_timer->Stop();
        m_refresh_timer->Start(MACHINE_LIST_REFRESH_INTERVAL);
    }

    if (wxGetApp().is_user_login()) {
        if (!get_print_info_thread) {
            get_print_info_thread = new boost::thread(Slic3r::create_thread([this, token = std::weak_ptr<int>(m_token)] {
                NetworkAgent* agent = wxGetApp().getAgent();
                unsigned int http_code;
                std::string body;
                int result = agent->get_user_print_info(&http_code, &body);

                wxGetApp().CallAfter([token, this, result, body]() {
                    if (token.expired()) {return;}
                    if (result == 0) {
                        m_print_info = body;
                    }
                    else {
                        m_print_info = "";
                    }

                    wxCommandEvent event(EVT_UPDATE_USER_MLIST);
                    event.SetEventObject(this);
                    wxPostEvent(this, event);
                });
            }));
        }
    }

    wxPostEvent(this, wxTimerEvent());
    PopupWindow::Popup();
}

void SelectMObjectPopup::OnDismiss()
{
    BOOST_LOG_TRIVIAL(trace) << "get_print_info: dismiss";
    m_dismiss = true;

    if (m_refresh_timer) {
        m_refresh_timer->Stop();
    }
    if (get_print_info_thread) {
        if (get_print_info_thread->joinable()) {
            get_print_info_thread->join();
            delete get_print_info_thread;
            get_print_info_thread = nullptr;
        }
    }

    wxCommandEvent event(EVT_FINISHED_UPDATE_MLIST);
    event.SetEventObject(this);
    wxPostEvent(this, event);
}

bool SelectMObjectPopup::ProcessLeftDown(wxMouseEvent& event) {
    return PopupWindow::ProcessLeftDown(event);
}

bool SelectMObjectPopup::Show(bool show) {
    if (show) {
        for (int i = 0; i < m_user_list_machine_panel.size(); i++) {
            m_user_list_machine_panel[i]->mPanel->update_machine_info(nullptr);
            m_user_list_machine_panel[i]->mPanel->Hide();
        }
    }
    return PopupWindow::Show(show);
}

void SelectMObjectPopup::on_timer(wxTimerEvent& event)
{
    BOOST_LOG_TRIVIAL(trace) << "SelectMObjectPopup on_timer";
    wxGetApp().reset_to_active();
    wxCommandEvent user_event(EVT_UPDATE_USER_MLIST);
    user_event.SetEventObject(this);
    wxPostEvent(this, user_event);
}

void SelectMObjectPopup::update_user_devices()
{
    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;

    if (!m_print_info.empty()) {
        dev->parse_user_print_info(m_print_info);
        m_print_info = "";
    }

    m_bind_machine_list.clear();
    m_bind_machine_list = dev->get_my_machine_list();

    //sort list
    std::vector<std::pair<std::string, MachineObject*>> user_machine_list;
    for (auto& it : m_bind_machine_list) {
        user_machine_list.push_back(it);
    }

    std::sort(user_machine_list.begin(), user_machine_list.end(), [&](auto& a, auto& b) {
        if (a.second && b.second) {
            return a.second->dev_name.compare(b.second->dev_name) < 0;
        }
        return false;
        });

    BOOST_LOG_TRIVIAL(trace) << "SelectMObjectPopup update_machine_list start";
    this->Freeze();
    m_scrolledWindow->Freeze();
    int i = 0;

    for (auto& elem : user_machine_list) {
        MachineObject* mobj = elem.second;
        MObjectPanel* op = nullptr;
        if (i < m_user_list_machine_panel.size()) {
            op = m_user_list_machine_panel[i]->mPanel;
            op->Show();
        }
        else {
            op = new MObjectPanel(m_scrolledWindow, wxID_ANY);
            MPanel* mpanel = new MPanel();
            mpanel->mIndex = wxString::Format("%d", i);
            mpanel->mPanel = op;
            m_user_list_machine_panel.push_back(mpanel);
            m_sizer_my_devices->Add(op, 0, wxEXPAND, 0);
        }
        i++;
        op->update_machine_info(mobj, true);
        //set in lan
        if (mobj->is_lan_mode_printer()) {
            if (!mobj->is_online()) {
                continue;
            }
            else {
                if (mobj->has_access_right() && mobj->is_avaliable()) {
                    op->set_printer_state(PrinterState::IN_LAN);
                    op->SetToolTip(_L("Online"));
                }
                else {
                    op->set_printer_state(PrinterState::LOCK);
                }
            }
        }
        else {
            if (!mobj->is_online()) {
                op->SetToolTip(_L("Offline"));
                op->set_printer_state(PrinterState::OFFLINE);
            }
            else {
                if (mobj->is_in_printing()) {
                    op->SetToolTip(_L("Busy"));
                    op->set_printer_state(PrinterState::BUSY);
                }
                else {
                    op->SetToolTip(_L("Online"));
                    op->set_printer_state(PrinterState::IDLE);
                }
            }
        }
    }

    for (int j = i; j < m_user_list_machine_panel.size(); j++) {
        m_user_list_machine_panel[j]->mPanel->update_machine_info(nullptr);
        m_user_list_machine_panel[j]->mPanel->Hide();
    }
    //m_sizer_my_devices->Layout();

    if (m_my_devices_count != i) {
        m_scrolledWindow->Fit();
    }
    m_scrolledWindow->Layout();
    m_scrolledWindow->Thaw();
    Layout();
    Fit();
    this->Thaw();
    m_my_devices_count = i;
}

void SelectMObjectPopup::on_dissmiss_win(wxCommandEvent& event)
{
    Dismiss();
}

void SelectMObjectPopup::update_machine_list(wxCommandEvent& event)
{
    update_user_devices();
    BOOST_LOG_TRIVIAL(trace) << "SelectMObjectPopup update_machine_list end";
}

void SelectMObjectPopup::OnLeftUp(wxMouseEvent& event)
{
    auto mouse_pos = ClientToScreen(event.GetPosition());
    auto wxscroll_win_pos = m_scrolledWindow->ClientToScreen(wxPoint(0, 0));

    if (mouse_pos.x > wxscroll_win_pos.x && mouse_pos.y > wxscroll_win_pos.y && mouse_pos.x < (wxscroll_win_pos.x + m_scrolledWindow->GetSize().x) &&
        mouse_pos.y < (wxscroll_win_pos.y + m_scrolledWindow->GetSize().y)) {

        for (MPanel* p : m_user_list_machine_panel) {
            auto p_rect = p->mPanel->ClientToScreen(wxPoint(0, 0));
            if (mouse_pos.x > p_rect.x && mouse_pos.y > p_rect.y && mouse_pos.x < (p_rect.x + p->mPanel->GetSize().x) && mouse_pos.y < (p_rect.y + p->mPanel->GetSize().y)) {
                wxMouseEvent event(wxEVT_LEFT_UP);
                auto         tag_pos = p->mPanel->ScreenToClient(mouse_pos);
                event.SetPosition(tag_pos);
                event.SetEventObject(p->mPanel);
                wxPostEvent(p->mPanel, event);
            }
        }
    }
}


CalibrationPanel::CalibrationPanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : wxPanel(parent, id, pos, size, style),
    m_mobjectlist_popup(SelectMObjectPopup(this))
{
    SetBackgroundColour(*wxWHITE);

    init_tabpanel();

    wxBoxSizer* sizer_main = new wxBoxSizer(wxVERTICAL);
    sizer_main->Add(m_tabpanel, 1, wxEXPAND, 0);

    SetSizerAndFit(sizer_main);
    Layout();

    init_timer();
    Bind(wxEVT_TIMER, &CalibrationPanel::on_timer, this);
}

void CalibrationPanel::init_tabpanel() {
    m_side_tools = new SideTools(this, wxID_ANY);
    m_side_tools->get_panel()->Connect(wxEVT_LEFT_DOWN, wxMouseEventHandler(CalibrationPanel::on_printer_clicked), NULL, this);


    wxBoxSizer* sizer_side_tools = new wxBoxSizer(wxVERTICAL);
    sizer_side_tools->Add(m_side_tools, 1, wxEXPAND, 0);

    m_tabpanel = new Tabbook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, sizer_side_tools, wxNB_LEFT | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME);
    m_side_tools->set_table_panel(m_tabpanel);
    m_tabpanel->SetBackgroundColour(*wxWHITE);

    m_cali_panels[0] = new PressureAdvanceWizard(m_tabpanel);
    m_cali_panels[1] = new FlowRateWizard(m_tabpanel);
    //m_cali_panels[2] = new MaxVolumetricSpeedWizard(m_tabpanel);

    for (int i = 0; i < (int)CALI_MODE_COUNT; i++) {
        bool selected = false;
        if (i == 0)
            selected = true;
        m_tabpanel->AddPage(m_cali_panels[i],
            get_calibration_type_name(m_cali_panels[i]->get_calibration_mode()),
            "",
            selected);
    }
    m_tabpanel->AddPage(new ColorSwatchMeasurementPage(m_tabpanel), _L("Color Swatches"), "", false);

    for (int i = 0; i < (int)CALI_MODE_COUNT; i++)
        m_tabpanel->SetPageImage(i, "");

    auto padding_size = m_tabpanel->GetBtnsListCtrl()->GetPaddingSize(0);
    m_tabpanel->GetBtnsListCtrl()->SetPaddingSize({ FromDIP(15), padding_size.y });

    m_initialized = true;
}

void CalibrationPanel::init_timer()
{
    m_refresh_timer = new wxTimer();
    m_refresh_timer->SetOwner(this);
    m_refresh_timer->Start(REFRESH_INTERVAL);
    wxPostEvent(this, wxTimerEvent());
}

void CalibrationPanel::on_timer(wxTimerEvent& event) {
    update_all();
}

void CalibrationPanel::update_print_error_info(int code, std::string msg, std::string extra) {
    // update current wizard only
    int curr_selected = m_tabpanel->GetSelection();
    if (curr_selected >= 0 && curr_selected < CALI_MODE_COUNT) {
        if (m_cali_panels[curr_selected]) {
            auto page = m_cali_panels[curr_selected]->get_curr_step()->page;
            if (page) {
                if (page->get_page_type() == CaliPageType::CALI_PAGE_PRESET) {
                    auto preset_page = static_cast<CalibrationPresetPage*>(page);
                    preset_page->update_print_error_info(code, msg, extra);
                }
                if (page->get_page_type() == CaliPageType::CALI_PAGE_COARSE_SAVE) {
                    auto corase_page = static_cast<CalibrationFlowCoarseSavePage*>(page);
                    corase_page->update_print_error_info(code, msg, extra);
                }
            }
        }
    }
}

void CalibrationPanel::update_all() {

    NetworkAgent* m_agent = wxGetApp().getAgent();
    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;
    obj = dev->get_selected_machine();

    // check valid machine
    if (obj && dev->get_my_machine(obj->dev_id) == nullptr) {
        dev->set_selected_machine("");
        if (m_agent) m_agent->set_user_selected_machine("");
        show_status((int) MONITOR_NO_PRINTER);
        return;
    }

    // update current wizard only
    int curr_selected = m_tabpanel->GetSelection();

    if (curr_selected >= 0 && curr_selected < CALI_MODE_COUNT) {
        if (m_cali_panels[curr_selected])
            m_cali_panels[curr_selected]->update(obj);
    }

    if (obj) {
        if (last_obj != obj && obj->is_info_ready()) {
            for (int i = 0; i < CALI_MODE_COUNT; i++) {
                m_cali_panels[i]->on_device_connected(obj);
            }
            last_obj = obj;
        }
    }

    if (wxGetApp().is_user_login()) {
        dev->check_pushing();
        try {
            m_agent->refresh_connection();
        }
        catch (...) {
            ;
        }
    }

    if (obj) {
        wxGetApp().reset_to_active();
        if (obj->connection_type() != last_conn_type) {
            last_conn_type = obj->connection_type();
        }
    }

    m_side_tools->update_status(obj);

    if (!obj) {
        show_status((int)MONITOR_NO_PRINTER);
        return;
    }

    if (obj->is_connecting()) {
        show_status(MONITOR_CONNECTING);
        return;
    }
    else if (!obj->is_connected()) {
        int server_status = 0;
        // only disconnected server in cloud mode
        if (obj->connection_type() != "lan") {
            if (m_agent) {
                server_status = m_agent->is_server_connected() ? 0 : (int)MONITOR_DISCONNECTED_SERVER;
            }
        }
        show_status((int)MONITOR_DISCONNECTED + server_status);
        return;
    }

    show_status(MONITOR_NORMAL);
}

void CalibrationPanel::show_status(int status)
{
    if (!m_initialized) return;
    if (last_status == status)return;
    last_status = status;

    BOOST_LOG_TRIVIAL(info) << "monitor: show_status = " << status;


    Freeze();
    // update panels
    if (m_side_tools) { m_side_tools->show_status(status); };

    if ((status & (int)MonitorStatus::MONITOR_NO_PRINTER) != 0) {
        set_default();
        m_tabpanel->Layout();
    }
    else if (((status & (int)MonitorStatus::MONITOR_NORMAL) != 0)
        || ((status & (int)MonitorStatus::MONITOR_DISCONNECTED) != 0)
        || ((status & (int)MonitorStatus::MONITOR_DISCONNECTED_SERVER) != 0)
        || ((status & (int)MonitorStatus::MONITOR_CONNECTING) != 0))
    {

        if (((status & (int)MonitorStatus::MONITOR_DISCONNECTED) != 0)
            || ((status & (int)MonitorStatus::MONITOR_DISCONNECTED_SERVER) != 0)
            || ((status & (int)MonitorStatus::MONITOR_CONNECTING) != 0))
        {
            set_default();
        }
        m_tabpanel->Layout();
    }
    Layout();
    Thaw();
}

bool CalibrationPanel::Show(bool show) {
    if (show) {
        m_refresh_timer->Stop();
        m_refresh_timer->SetOwner(this);
        m_refresh_timer->Start(REFRESH_INTERVAL);
        wxPostEvent(this, wxTimerEvent());

        DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
        if (dev) {
            //set a default machine when obj is null
            obj = dev->get_selected_machine();
            if (obj == nullptr) {
                dev->load_last_machine();
                obj = dev->get_selected_machine();
                if (obj)
                    GUI::wxGetApp().sidebar().load_ams_list(obj->dev_id, obj);
            }
            else {
                obj->reset_update_time();
            }
        }

    }
    else {
        m_refresh_timer->Stop();
    }
    return wxPanel::Show(show);
}

void CalibrationPanel::on_printer_clicked(wxMouseEvent& event)
{
    auto mouse_pos = ClientToScreen(event.GetPosition());
    wxPoint rect = m_side_tools->ClientToScreen(wxPoint(0, 0));

    if (!m_side_tools->is_in_interval()) {
        wxPoint pos = m_side_tools->ClientToScreen(wxPoint(0, 0));
        pos.y += m_side_tools->GetRect().height;
        m_mobjectlist_popup.Move(pos);

#ifdef __linux__
        m_mobjectlist_popup.SetSize(wxSize(m_side_tools->GetSize().x, -1));
        m_mobjectlist_popup.SetMaxSize(wxSize(m_side_tools->GetSize().x, -1));
        m_mobjectlist_popup.SetMinSize(wxSize(m_side_tools->GetSize().x, -1));
#endif

        m_mobjectlist_popup.Popup();
    }
}

void CalibrationPanel::set_default()
{
    obj = nullptr;
    last_conn_type = "undefined";
    wxGetApp().sidebar().load_ams_list({}, {});
}

void CalibrationPanel::msw_rescale()
{
    for (int i = 0; i < (int)CALI_MODE_COUNT; i++) {
        m_cali_panels[i]->msw_rescale();
    }
}

void CalibrationPanel::on_sys_color_changed()
{
    for (int i = 0; i < (int)CALI_MODE_COUNT; i++) {
        m_cali_panels[i]->on_sys_color_changed();
    }
}

CalibrationPanel::~CalibrationPanel() {
    m_side_tools->get_panel()->Disconnect(wxEVT_LEFT_DOWN, wxMouseEventHandler(CalibrationPanel::on_printer_clicked), NULL, this);
    if (m_refresh_timer)
        m_refresh_timer->Stop();
    delete m_refresh_timer;
}

}}
