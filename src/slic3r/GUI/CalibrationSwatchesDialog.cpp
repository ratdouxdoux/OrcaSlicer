#include "CalibrationSwatchesDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Widgets/Label.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <wx/filename.h>
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
#include <wx/tokenzr.h>
#include <wx/utils.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <array>

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

static bool parse_positive_mm_list(const wxString &text, std::vector<double> &values, wxString *error = nullptr)
{
    values.clear();

    wxStringTokenizer tokenizer(text, ",", wxTOKEN_RET_EMPTY_ALL);
    unsigned int token_index = 0;
    while (tokenizer.HasMoreTokens()) {
        ++token_index;
        wxString token = tokenizer.GetNextToken().Trim(true).Trim(false);
        if (token.empty()) {
            if (error != nullptr)
                *error = wxString::Format(_L("TD staircase width %u is empty."), token_index);
            return false;
        }

        double value = 0.0;
        if (!token.ToDouble(&value) || !std::isfinite(value) || value <= 0.0) {
            if (error != nullptr)
                *error = wxString::Format(_L("TD staircase width \"%s\" is not a positive number."), token.c_str());
            return false;
        }

        const bool duplicate = std::any_of(values.begin(), values.end(), [value](double existing) {
            return std::abs(existing - value) < 1e-6;
        });
        if (!duplicate)
            values.emplace_back(value);
    }

    if (values.empty()) {
        if (error != nullptr)
            *error = _L("Enter at least one TD staircase width.");
        return false;
    }

    return true;
}

static std::vector<ColorCalibrationSwatches::FilamentSlot> current_filaments()
{
    std::vector<ColorCalibrationSwatches::FilamentSlot> filaments;
    const PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return filaments;

    const DynamicPrintConfig &project_config = preset_bundle->project_config;
    const ConfigOptionStrings *color_opt = project_config.option<ConfigOptionStrings>("filament_colour");
    const ConfigOptionFloats *td_opt = project_config.option<ConfigOptionFloats>("filament_transmission_distance");
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
        if (td_opt != nullptr && i < td_opt->values.size() && std::isfinite(td_opt->values[i]) && td_opt->values[i] > 0.0)
            slot.td = td_opt->values[i];
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

static bool current_bool_option(const char *key)
{
    const PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return false;

    if (const ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>(key))
        return opt->value;

    DynamicPrintConfig full_config = preset_bundle->full_config();
    if (const ConfigOptionBool *opt = full_config.option<ConfigOptionBool>(key))
        return opt->value;

    return false;
}

static std::string trim_copy(std::string value)
{
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

static std::string normalized_header(std::string value)
{
    value = trim_copy(std::move(value));
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch))
            out.push_back(char(std::tolower(ch)));
    }
    return out;
}

static int ratio_header_slot(const std::string &header)
{
    const std::string h = normalized_header(header);
    if (h == "cratio" || h == "c" || h == "cyan" || h == "f1" || h == "f1ratio" || h == "slot1")
        return 0;
    if (h == "mratio" || h == "m" || h == "magenta" || h == "f2" || h == "f2ratio" || h == "slot2")
        return 1;
    if (h == "yratio" || h == "y" || h == "yellow" || h == "f3" || h == "f3ratio" || h == "slot3")
        return 2;
    if (h == "wratio" || h == "w" || h == "k" || h == "white" || h == "grey" || h == "gray" ||
        h == "f4" || h == "f4ratio" || h == "slot4")
        return 3;
    return -1;
}

static void replace_all(std::string &value, const std::string &from, const std::string &to)
{
    if (from.empty())
        return;
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::string xml_decode(std::string value)
{
    replace_all(value, "&quot;", "\"");
    replace_all(value, "&apos;", "'");
    replace_all(value, "&lt;", "<");
    replace_all(value, "&gt;", ">");
    replace_all(value, "&amp;", "&");
    return value;
}

static std::string xml_attr_value(const std::string &tag, const std::string &name)
{
    for (const char quote : { '"', '\'' }) {
        const std::string needle = name + "=" + quote;
        const size_t start = tag.find(needle);
        if (start == std::string::npos)
            continue;

        const size_t value_start = start + needle.size();
        const size_t value_end = tag.find(quote, value_start);
        if (value_end == std::string::npos)
            return {};
        return xml_decode(tag.substr(value_start, value_end - value_start));
    }
    return {};
}

static std::string xml_tag_text(const std::string &xml, const std::string &tag_name)
{
    const std::string open = "<" + tag_name;
    const size_t tag_start = xml.find(open);
    if (tag_start == std::string::npos)
        return {};

    const size_t text_start = xml.find('>', tag_start);
    if (text_start == std::string::npos)
        return {};

    const std::string close = "</" + tag_name + ">";
    const size_t text_end = xml.find(close, text_start + 1);
    if (text_end == std::string::npos)
        return {};

    return xml_decode(xml.substr(text_start + 1, text_end - text_start - 1));
}

static std::vector<std::string> xlsx_shared_strings(const std::string &xml)
{
    std::vector<std::string> strings;
    size_t pos = 0;
    while ((pos = xml.find("<si", pos)) != std::string::npos) {
        const size_t open_end = xml.find('>', pos);
        const size_t end = open_end == std::string::npos ? std::string::npos : xml.find("</si>", open_end + 1);
        if (open_end == std::string::npos || end == std::string::npos)
            break;

        const std::string si = xml.substr(open_end + 1, end - open_end - 1);
        std::string text;
        size_t text_pos = 0;
        while ((text_pos = si.find("<t", text_pos)) != std::string::npos) {
            const size_t text_open_end = si.find('>', text_pos);
            const size_t text_end = text_open_end == std::string::npos ? std::string::npos : si.find("</t>", text_open_end + 1);
            if (text_open_end == std::string::npos || text_end == std::string::npos)
                break;
            text += xml_decode(si.substr(text_open_end + 1, text_end - text_open_end - 1));
            text_pos = text_end + 4;
        }
        strings.emplace_back(std::move(text));
        pos = end + 5;
    }
    return strings;
}

static bool zip_entry_to_string(mz_zip_archive &zip, const char *entry_name, std::string &out)
{
    const int index = mz_zip_reader_locate_file(&zip, entry_name, nullptr, 0);
    if (index < 0)
        return false;

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip, mz_uint(index), &stat))
        return false;

    out.assign(size_t(stat.m_uncomp_size), '\0');
    if (out.empty())
        return true;

    return mz_zip_reader_extract_to_mem(&zip, stat.m_file_index, out.data(), out.size(), 0) != 0;
}

static std::string first_worksheet_path(const std::string &workbook_xml, const std::string &rels_xml)
{
    std::string rel_id;
    size_t sheet_start = 0;
    while ((sheet_start = workbook_xml.find("<sheet", sheet_start)) != std::string::npos) {
        const size_t sheet_end = workbook_xml.find('>', sheet_start);
        if (sheet_end == std::string::npos)
            return "xl/worksheets/sheet1.xml";

        const std::string sheet_tag = workbook_xml.substr(sheet_start, sheet_end - sheet_start + 1);
        rel_id = xml_attr_value(sheet_tag, "r:id");
        if (!rel_id.empty())
            break;

        sheet_start = sheet_end + 1;
    }

    if (rel_id.empty())
        return "xl/worksheets/sheet1.xml";

    size_t pos = 0;
    while ((pos = rels_xml.find("<Relationship", pos)) != std::string::npos) {
        const size_t rel_end = rels_xml.find('>', pos);
        if (rel_end == std::string::npos)
            break;
        const std::string rel_tag = rels_xml.substr(pos, rel_end - pos + 1);
        if (xml_attr_value(rel_tag, "Id") == rel_id) {
            std::string target = xml_attr_value(rel_tag, "Target");
            if (target.empty())
                break;
            if (!target.empty() && target.front() == '/')
                target.erase(target.begin());
            else if (target.rfind("xl/", 0) != 0)
                target = "xl/" + target;
            return target;
        }
        pos = rel_end + 1;
    }

    return "xl/worksheets/sheet1.xml";
}

static unsigned int xlsx_column_index(const std::string &cell_ref)
{
    unsigned int index = 0;
    for (unsigned char ch : cell_ref) {
        if (!std::isalpha(ch))
            break;
        index = index * 26u + unsigned(std::toupper(ch) - 'A' + 1);
    }
    return index > 0 ? index - 1 : 0;
}

static std::string xlsx_cell_value(const std::string &cell_xml, const std::vector<std::string> &shared_strings)
{
    const size_t open_end = cell_xml.find('>');
    if (open_end == std::string::npos)
        return {};

    const std::string cell_tag = cell_xml.substr(0, open_end + 1);
    const std::string type = xml_attr_value(cell_tag, "t");
    if (type == "inlineStr")
        return trim_copy(xml_tag_text(cell_xml, "t"));

    std::string value = trim_copy(xml_tag_text(cell_xml, "v"));
    if (type == "s" && !value.empty()) {
        char *end = nullptr;
        const long index = std::strtol(value.c_str(), &end, 10);
        if (end != value.c_str() && index >= 0 && size_t(index) < shared_strings.size())
            value = shared_strings[size_t(index)];
    }
    return trim_copy(value);
}

static std::vector<std::map<unsigned int, std::string>> xlsx_sheet_rows(const std::string &sheet_xml,
                                                                        const std::vector<std::string> &shared_strings)
{
    std::vector<std::map<unsigned int, std::string>> rows;

    size_t row_pos = 0;
    while ((row_pos = sheet_xml.find("<row", row_pos)) != std::string::npos) {
        const size_t row_open_end = sheet_xml.find('>', row_pos);
        const size_t row_end = row_open_end == std::string::npos ? std::string::npos : sheet_xml.find("</row>", row_open_end + 1);
        if (row_open_end == std::string::npos || row_end == std::string::npos)
            break;

        const std::string row_xml = sheet_xml.substr(row_open_end + 1, row_end - row_open_end - 1);
        std::map<unsigned int, std::string> row;

        size_t cell_pos = 0;
        while ((cell_pos = row_xml.find("<c", cell_pos)) != std::string::npos) {
            const size_t cell_open_end = row_xml.find('>', cell_pos);
            if (cell_open_end == std::string::npos)
                break;

            const std::string cell_tag = row_xml.substr(cell_pos, cell_open_end - cell_pos + 1);
            const std::string ref = xml_attr_value(cell_tag, "r");

            size_t cell_end = cell_open_end;
            std::string cell_xml;
            if (cell_open_end > cell_pos && row_xml[cell_open_end - 1] == '/') {
                cell_xml = cell_tag;
            } else {
                cell_end = row_xml.find("</c>", cell_open_end + 1);
                if (cell_end == std::string::npos)
                    break;
                cell_xml = row_xml.substr(cell_pos, cell_end - cell_pos + 4);
            }

            if (!ref.empty()) {
                const std::string value = xlsx_cell_value(cell_xml, shared_strings);
                if (!value.empty())
                    row[xlsx_column_index(ref)] = value;
            }

            cell_pos = cell_end + 1;
        }

        rows.emplace_back(std::move(row));
        row_pos = row_end + 6;
    }

    return rows;
}

static bool parse_ratio_number(const std::string &text, double &value)
{
    std::string normalized = trim_copy(text);
    std::replace(normalized.begin(), normalized.end(), ',', '.');
    if (normalized.empty()) {
        value = 0.0;
        return true;
    }

    char *end = nullptr;
    value = std::strtod(normalized.c_str(), &end);
    if (end == normalized.c_str())
        return false;
    while (end != nullptr && *end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end)))
            return false;
        ++end;
    }
    return std::isfinite(value);
}

static std::pair<int, int> approximate_fraction(double value, int max_denominator)
{
    int best_num = 0;
    int best_den = 1;
    double best_error = std::numeric_limits<double>::max();
    for (int den = 1; den <= max_denominator; ++den) {
        int num = int(std::llround(value * double(den)));
        num = std::max(0, std::min(num, den));
        const double error = std::abs(double(num) / double(den) - value);
        if (error + 1e-12 < best_error) {
            best_error = error;
            best_num = num;
            best_den = den;
        }
    }

    const int g = std::gcd(std::max(0, best_num), std::max(1, best_den));
    return { best_num / g, best_den / g };
}

static std::vector<int> denominator_search_counts(const std::vector<double> &normalized, const std::vector<size_t> &positive_indices)
{
    std::vector<int> best(normalized.size(), 0);
    double best_error = std::numeric_limits<double>::max();
    int best_total = std::numeric_limits<int>::max();

    for (int total = int(positive_indices.size()); total <= 120; ++total) {
        std::vector<int> counts(normalized.size(), 0);
        for (size_t idx : positive_indices)
            counts[idx] = std::max(1, int(std::llround(normalized[idx] * double(total))));

        const int count_total = std::accumulate(counts.begin(), counts.end(), 0);
        if (count_total <= 0)
            continue;

        double error = 0.0;
        for (size_t idx : positive_indices)
            error += std::abs(double(counts[idx]) / double(count_total) - normalized[idx]);

        if (error + 1e-12 < best_error || (std::abs(error - best_error) < 1e-12 && count_total < best_total)) {
            best_error = error;
            best_total = count_total;
            best = std::move(counts);
        }
    }

    int g = 0;
    for (int count : best)
        if (count > 0)
            g = std::gcd(g, count);
    if (g > 1) {
        for (int &count : best)
            if (count > 0)
                count /= g;
    }

    return best;
}

static std::vector<int> ratio_counts_from_weights(const std::array<double, 4> &weights)
{
    std::vector<size_t> positive_indices;
    positive_indices.reserve(weights.size());
    double sum = 0.0;
    for (size_t i = 0; i < weights.size(); ++i) {
        if (weights[i] > 1e-9) {
            positive_indices.emplace_back(i);
            sum += weights[i];
        }
    }

    std::vector<int> counts(weights.size(), 0);
    if (positive_indices.empty())
        return counts;
    if (positive_indices.size() == 1) {
        counts[positive_indices.front()] = 1;
        return counts;
    }

    std::vector<double> normalized(weights.size(), 0.0);
    for (size_t idx : positive_indices)
        normalized[idx] = weights[idx] / sum;

    constexpr int max_denominator = 1000;
    constexpr int max_lcm = 10000;
    int lcm = 1;
    std::vector<std::pair<int, int>> fractions(weights.size(), { 0, 1 });
    bool lcm_ok = true;
    for (size_t idx : positive_indices) {
        fractions[idx] = approximate_fraction(normalized[idx], max_denominator);
        if (fractions[idx].first <= 0) {
            lcm_ok = false;
            break;
        }
        lcm = std::lcm(lcm, fractions[idx].second);
        if (lcm <= 0 || lcm > max_lcm) {
            lcm_ok = false;
            break;
        }
    }

    if (!lcm_ok)
        return denominator_search_counts(normalized, positive_indices);

    for (size_t idx : positive_indices)
        counts[idx] = fractions[idx].first * (lcm / fractions[idx].second);

    int g = 0;
    for (int count : counts)
        if (count > 0)
            g = std::gcd(g, count);
    if (g > 1) {
        for (int &count : counts)
            if (count > 0)
                count /= g;
    }
    return counts;
}

struct RatioRowSummary
{
    size_t row_count = 0;
    size_t one_filament_rows = 0;
    size_t mixed_rows = 0;
    size_t unique_recipes = 0;
    size_t unique_mixed_recipes = 0;
};

static std::string ratio_recipe_key(const std::vector<int> &row)
{
    std::ostringstream ss;
    for (size_t i = 0; i < row.size(); ++i) {
        const int ratio = std::max(0, row[i]);
        if (ratio <= 0)
            continue;
        if (ss.tellp() > 0)
            ss << '|';
        ss << (i + 1) << ':' << ratio;
    }
    return ss.str();
}

static RatioRowSummary summarize_ratio_rows(const std::vector<std::vector<int>> &rows)
{
    RatioRowSummary summary;
    summary.row_count = rows.size();

    std::set<std::string> recipes;
    std::set<std::string> mixed_recipes;
    for (const std::vector<int> &row : rows) {
        const size_t positive_count = static_cast<size_t>(std::count_if(row.begin(), row.end(), [](int value) { return value > 0; }));
        if (positive_count == 0)
            continue;

        const std::string key = ratio_recipe_key(row);
        recipes.insert(key);
        if (positive_count == 1) {
            ++summary.one_filament_rows;
        } else {
            ++summary.mixed_rows;
            mixed_recipes.insert(key);
        }
    }

    summary.unique_recipes = recipes.size();
    summary.unique_mixed_recipes = mixed_recipes.size();
    return summary;
}

static bool load_xlsx_ratio_rows(const wxString &path, std::vector<std::vector<int>> &ratio_rows, wxString *error)
{
    ratio_rows.clear();

    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, wx_to_u8(path))) {
        if (error != nullptr)
            *error = _L("Could not open the selected workbook. Only .xlsx/.xlsm files are supported.");
        return false;
    }

    struct ZipGuard
    {
        mz_zip_archive *zip = nullptr;
        ~ZipGuard() { if (zip != nullptr) close_zip_reader(zip); }
    } guard { &zip };

    std::string workbook_xml;
    std::string rels_xml;
    std::string sheet_xml;
    std::string shared_xml;
    if (!zip_entry_to_string(zip, "xl/workbook.xml", workbook_xml) ||
        !zip_entry_to_string(zip, "xl/_rels/workbook.xml.rels", rels_xml)) {
        if (error != nullptr)
            *error = _L("The selected workbook is missing workbook metadata.");
        return false;
    }

    const std::string sheet_path = first_worksheet_path(workbook_xml, rels_xml);
    if (!zip_entry_to_string(zip, sheet_path.c_str(), sheet_xml)) {
        if (error != nullptr)
            *error = _L("The selected workbook has no readable first worksheet.");
        return false;
    }

    std::vector<std::string> shared_strings;
    if (zip_entry_to_string(zip, "xl/sharedStrings.xml", shared_xml))
        shared_strings = xlsx_shared_strings(shared_xml);

    const auto rows = xlsx_sheet_rows(sheet_xml, shared_strings);
    std::array<int, 4> ratio_columns { -1, -1, -1, -1 };
    size_t header_row = rows.size();
    for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
        std::array<int, 4> candidate { -1, -1, -1, -1 };
        for (const auto &[column, value] : rows[row_index]) {
            const int slot = ratio_header_slot(value);
            if (slot >= 0)
                candidate[size_t(slot)] = int(column);
        }
        if (std::all_of(candidate.begin(), candidate.end(), [](int column) { return column >= 0; })) {
            ratio_columns = candidate;
            header_row = row_index;
            break;
        }
    }

    if (header_row == rows.size()) {
        if (error != nullptr)
            *error = _L("Expected columns C_ratio, M_ratio, Y_ratio, and W_ratio in the first worksheet.");
        return false;
    }

    for (size_t row_index = header_row + 1; row_index < rows.size(); ++row_index) {
        std::array<double, 4> weights { 0.0, 0.0, 0.0, 0.0 };
        bool has_value = false;
        for (size_t i = 0; i < ratio_columns.size(); ++i) {
            const auto cell = rows[row_index].find(unsigned(ratio_columns[i]));
            if (cell == rows[row_index].end())
                continue;

            double value = 0.0;
            if (!parse_ratio_number(cell->second, value) || value < 0.0) {
                if (error != nullptr)
                    *error = wxString::Format(_L("Invalid ratio value in spreadsheet row %llu."), static_cast<unsigned long long>(row_index + 1));
                return false;
            }

            if (value > 1e-9)
                has_value = true;
            weights[i] = value;
        }

        if (!has_value)
            continue;

        std::vector<int> counts = ratio_counts_from_weights(weights);
        if (std::any_of(counts.begin(), counts.end(), [](int count) { return count > 0; }))
            ratio_rows.emplace_back(std::move(counts));
    }

    if (ratio_rows.empty()) {
        if (error != nullptr)
            *error = _L("No ratio rows were found in the selected workbook.");
        return false;
    }

    return true;
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

    auto *ratio_file_box = new wxStaticBoxSizer(wxVERTICAL, settings_scroll, _L("Ratio spreadsheet"));
    auto *ratio_file_row = new wxBoxSizer(wxHORIZONTAL);
    m_ratio_file_open = new wxButton(settings_scroll, wxID_ANY, _L("Open ratios file..."));
    m_ratio_file_clear = new wxButton(settings_scroll, wxID_ANY, _L("Clear"));
    m_ratio_file_clear->Enable(false);
    ratio_file_row->Add(m_ratio_file_open, 0, wxRIGHT, FromDIP(8));
    ratio_file_row->Add(m_ratio_file_clear, 0);
    ratio_file_box->Add(ratio_file_row, 0, wxALL, FromDIP(8));

    m_ratio_file_status = new wxStaticText(settings_scroll,
                                           wxID_ANY,
                                           _L("No ratio file loaded. When loaded, spreadsheet rows replace the automatic mix proportions."));
    m_ratio_file_status->Wrap(FromDIP(500));
    ratio_file_box->Add(m_ratio_file_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    settings_sizer->Add(ratio_file_box, 0, wxEXPAND);

    auto *families_box = new wxStaticBoxSizer(wxVERTICAL, settings_scroll, _L("Swatches"));
    auto *families_grid = new wxFlexGridSizer(2, FromDIP(12), FromDIP(16));
    m_family_anchor     = new wxCheckBox(settings_scroll, wxID_ANY, _L("Anchor chips"));
    m_family_td_ladder  = new wxCheckBox(settings_scroll, wxID_ANY, _L("TD staircase"));
    m_family_pair_mix   = new wxCheckBox(settings_scroll, wxID_ANY, _L("Pair mixes"));
    m_family_ternary    = new wxCheckBox(settings_scroll, wxID_ANY, _L("Ternary mixes"));
    m_family_quaternary = new wxCheckBox(settings_scroll, wxID_ANY, _L("Four-color mixes"));
    m_family_anchor->SetValue(true);
    m_family_td_ladder->SetValue(false);
    m_family_pair_mix->SetValue(true);
    m_family_ternary->SetValue(false);
    m_family_quaternary->SetValue(false);
    families_grid->Add(m_family_anchor, 0, wxBOTTOM, FromDIP(4));
    families_grid->Add(m_family_td_ladder, 0, wxBOTTOM, FromDIP(4));
    families_grid->Add(m_family_pair_mix, 0, wxBOTTOM, FromDIP(4));
    families_grid->Add(m_family_ternary, 0, wxBOTTOM, FromDIP(4));
    families_grid->Add(m_family_quaternary, 0, wxBOTTOM, FromDIP(4));
    families_box->Add(families_grid, 0, wxALL, FromDIP(8));
    settings_sizer->Add(families_box, 0, wxEXPAND);

    auto *filament_td_box = new wxStaticBoxSizer(wxVERTICAL, settings_scroll, _L("Filament TD"));
    if (dialog_filaments.empty()) {
        auto *empty_note = new wxStaticText(settings_scroll, wxID_ANY, _L("No physical filaments are available."));
        filament_td_box->Add(empty_note, 0, wxALL, FromDIP(8));
    } else {
        auto *filament_td_grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
        filament_td_grid->AddGrowableCol(1);
        for (const ColorCalibrationSwatches::FilamentSlot &filament : dialog_filaments) {
            wxString label = wxString::Format(_L("Slot %u TD (mm)"), filament.slot);
            if (!filament.color_hex.empty())
                label += _L(" ") + wxString::FromUTF8(filament.color_hex.c_str());

            wxString td_value;
            if (filament.td && std::isfinite(*filament.td) && *filament.td > 0.0)
                td_value = wxString::Format("%g", *filament.td);
            auto *td_input = new wxTextCtrl(settings_scroll,
                                            wxID_ANY,
                                            td_value,
                                            wxDefaultPosition,
                                            FromDIP(wxSize(90, -1)));
            td_input->SetToolTip(_L("Transmission distance in mm. Leave empty if unknown. Anchor depths are TD + 1 mm, TD / 2 + 1 mm, and 1 mm."));
            m_filament_td_inputs.emplace_back(td_input);
            m_filament_td_slots.emplace_back(filament.slot);
            add_labeled_control(settings_scroll, filament_td_grid, label, td_input);
        }
        filament_td_box->Add(filament_td_grid, 0, wxEXPAND | wxALL, FromDIP(8));
    }
    settings_sizer->Add(filament_td_box, 0, wxEXPAND | wxTOP, FromDIP(12));

    auto *layout_box = new wxStaticBoxSizer(wxVERTICAL, settings_scroll, _L("Layout"));
    auto *layout_grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    layout_grid->AddGrowableCol(1);
    const ColorCalibrationSwatches::SwatchLayoutOptions default_layout;
    m_chip_width   = make_spin(settings_scroll, 5.0, 80.0, 20.0, 1.0);
    m_chip_depth   = make_spin(settings_scroll, 5.0, 80.0, 20.0, 1.0);
    m_spacing      = make_spin(settings_scroll, 0.0, 30.0, 4.0, 0.5);
    m_strip_spacing = make_spin(settings_scroll, 0.0, 30.0, 2.0, 0.25, 2);
    m_anchor_thick = make_spin(settings_scroll, 0.2, 20.0, 6.0, 0.2);
    m_td_ladder_widths = new wxTextCtrl(settings_scroll,
                                        wxID_ANY,
                                        _L("1, 2, 3, 4, 5, 6"),
                                        wxDefaultPosition,
                                        FromDIP(wxSize(180, -1)));
    m_td_ladder_widths->SetToolTip(_L("Comma-separated TD staircase widths in mm. These generate single-filament base swatches only."));
    m_plate_buffer = make_spin(settings_scroll, 0.0, 30.0, 8.0, 0.5, 1);
    m_prime_tower_reserve = new wxCheckBox(settings_scroll, wxID_ANY, _L("Reserve prime tower"));
    m_prime_tower_reserve->SetValue(default_layout.reserve_prime_tower);
    m_prime_tower_width = make_spin(settings_scroll, 0.0, 160.0, default_layout.prime_tower_width_mm, 1.0, 1);
    m_prime_tower_depth = make_spin(settings_scroll, 0.0, 160.0, default_layout.prime_tower_depth_mm, 1.0, 1);
    add_labeled_control(settings_scroll, layout_grid, _L("Swatch width"), m_chip_width);
    add_labeled_control(settings_scroll, layout_grid, _L("Face height"), m_chip_depth);
    add_labeled_control(settings_scroll, layout_grid, _L("Column spacing"), m_spacing);
    add_labeled_control(settings_scroll, layout_grid, _L("Strip spacing"), m_strip_spacing);
    add_labeled_control(settings_scroll, layout_grid, _L("Mix swatch depth"), m_anchor_thick);
    add_labeled_control(settings_scroll, layout_grid, _L("TD staircase widths"), m_td_ladder_widths);
    add_labeled_control(settings_scroll, layout_grid, _L("Plate buffer"), m_plate_buffer);
    layout_grid->Add(m_prime_tower_reserve, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    layout_grid->AddSpacer(0);
    add_labeled_control(settings_scroll, layout_grid, _L("Prime tower width"), m_prime_tower_width);
    add_labeled_control(settings_scroll, layout_grid, _L("Prime tower depth"), m_prime_tower_depth);
    layout_box->Add(layout_grid, 0, wxEXPAND | wxALL, FromDIP(8));
    settings_sizer->Add(layout_box, 0, wxEXPAND | wxTOP, FromDIP(12));

    auto *values_box = new wxStaticBoxSizer(wxVERTICAL, settings_scroll, _L("Mix proportions"));
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
    m_quaternary_layer_limit = new wxSpinCtrl(settings_scroll,
                                             wxID_ANY,
                                             wxEmptyString,
                                             wxDefaultPosition,
                                             FromDIP(wxSize(90, -1)),
                                             wxSP_ARROW_KEYS,
                                             1,
                                             20,
                                             3);
    m_local_z_enabled = new wxCheckBox(settings_scroll, wxID_ANY, _L("Local Z enabled"));
    m_local_z_enabled->SetValue(current_bool_option("dithering_local_z_mode"));
    m_direct_multicolor_solver = new wxCheckBox(settings_scroll, wxID_ANY, _L("Use direct multicolor Local-Z solver"));
    m_direct_multicolor_solver->SetValue(current_bool_option("dithering_local_z_direct_multicolor"));
    add_labeled_control(settings_scroll, values_grid, _L("Max 1-to-many ratio"), m_pair_layer_limit);
    add_labeled_control(settings_scroll, values_grid, _L("Four-color max lines per filament"), m_quaternary_layer_limit);
    values_grid->Add(m_local_z_enabled, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    values_grid->Add(m_direct_multicolor_solver, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
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

    auto *reference_box = new wxStaticBoxSizer(wxVERTICAL, settings_scroll, _L("Swatch reference"));
    auto *reference_grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    reference_grid->AddGrowableCol(1);
    const ColorCalibrationSwatches::SwatchReferenceOptions default_reference;
    m_plate_reference = new wxTextCtrl(settings_scroll,
                                       wxID_ANY,
                                       wxString::FromUTF8(default_reference.plate_reference.c_str()),
                                       wxDefaultPosition,
                                       FromDIP(wxSize(90, -1)));
    m_plate_reference->SetToolTip(_L("Use a unique plate letter for each printed run. Multi-plate jobs increment this letter automatically."));
    m_reference_text_size = make_spin(settings_scroll, 1.0, 16.0, default_reference.text_size_mm, 0.1, 1);
    m_reference_text_depth = make_spin(settings_scroll, 0.05, 2.0, default_reference.text_depth_mm, 0.05, 2);
    m_reference_text_stroke_width = make_spin(settings_scroll, 0.0, 2.0, default_reference.stroke_width_mm, 0.05, 2);
    add_labeled_control(settings_scroll, reference_grid, _L("Plate reference"), m_plate_reference);
    add_labeled_control(settings_scroll, reference_grid, _L("Text size"), m_reference_text_size);
    add_labeled_control(settings_scroll, reference_grid, _L("Text depth"), m_reference_text_depth);
    add_labeled_control(settings_scroll, reference_grid, _L("Text stroke width"), m_reference_text_stroke_width);
    reference_box->Add(reference_grid, 0, wxEXPAND | wxALL, FromDIP(8));
    auto *reference_note = new wxStaticText(settings_scroll,
                                           wxID_ANY,
                                           _L("Printed marks are plate letter plus manifest sample number, for example A37. Use a unique plate letter per printed run."));
    reference_note->Wrap(FromDIP(520));
    reference_box->Add(reference_note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    settings_sizer->Add(reference_box, 0, wxEXPAND | wxTOP, FromDIP(12));

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
                               static_cast<wxWindow*>(m_family_td_ladder),
                               static_cast<wxWindow*>(m_family_pair_mix),
                               static_cast<wxWindow*>(m_family_ternary),
                               static_cast<wxWindow*>(m_family_quaternary),
                               static_cast<wxWindow*>(m_chip_width),
                               static_cast<wxWindow*>(m_chip_depth),
                               static_cast<wxWindow*>(m_spacing),
                               static_cast<wxWindow*>(m_strip_spacing),
                               static_cast<wxWindow*>(m_anchor_thick),
                               static_cast<wxWindow*>(m_td_ladder_widths),
                               static_cast<wxWindow*>(m_plate_buffer),
                               static_cast<wxWindow*>(m_prime_tower_reserve),
                               static_cast<wxWindow*>(m_prime_tower_width),
                               static_cast<wxWindow*>(m_prime_tower_depth),
                               static_cast<wxWindow*>(m_pair_layer_limit),
                               static_cast<wxWindow*>(m_quaternary_layer_limit),
                               static_cast<wxWindow*>(m_local_z_enabled),
                               static_cast<wxWindow*>(m_direct_multicolor_solver),
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
                               static_cast<wxWindow*>(m_plate_reference),
                               static_cast<wxWindow*>(m_reference_text_size),
                               static_cast<wxWindow*>(m_reference_text_depth),
                               static_cast<wxWindow*>(m_reference_text_stroke_width) }) {
        control->Bind(wxEVT_CHECKBOX, bind_preview);
        control->Bind(wxEVT_CHOICE, bind_preview);
        control->Bind(wxEVT_TEXT, bind_preview);
        control->Bind(wxEVT_SPINCTRL, bind_preview);
        control->Bind(wxEVT_SPINCTRLDOUBLE, bind_preview);
    }
    for (wxTextCtrl *td_input : m_filament_td_inputs)
        if (td_input != nullptr)
            td_input->Bind(wxEVT_TEXT, bind_preview);
    m_ratio_file_open->Bind(wxEVT_BUTTON, &CalibrationSwatchesDialog::on_open_ratio_file, this);
    m_ratio_file_clear->Bind(wxEVT_BUTTON, &CalibrationSwatchesDialog::on_clear_ratio_file, this);
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

bool CalibrationSwatchesDialog::apply_filament_tds(std::vector<ColorCalibrationSwatches::FilamentSlot> &filaments, wxString *error) const
{
    for (size_t i = 0; i < m_filament_td_inputs.size(); ++i) {
        wxTextCtrl *input = m_filament_td_inputs[i];
        if (input == nullptr)
            continue;

        const unsigned int slot = i < m_filament_td_slots.size() ? m_filament_td_slots[i] : static_cast<unsigned int>(i + 1);
        wxString text = input->GetValue();
        text.Trim(true).Trim(false);

        auto it = std::find_if(filaments.begin(), filaments.end(), [slot](const ColorCalibrationSwatches::FilamentSlot &filament) {
            return filament.slot == slot;
        });
        if (it == filaments.end())
            continue;

        if (text.empty()) {
            it->td.reset();
            continue;
        }

        double td = 0.0;
        if (!text.ToDouble(&td) || !std::isfinite(td) || td < 0.0) {
            if (error != nullptr)
                *error = wxString::Format(_L("Filament slot %u TD must be a non-negative number."), slot);
            return false;
        }

        if (td > 0.0)
            it->td = td;
        else
            it->td.reset();
    }

    return true;
}

CalibrationSwatchesDialog::SwatchGeneratorConfig CalibrationSwatchesDialog::build_config() const
{
    SwatchGeneratorConfig config;
    config.filaments = current_filaments();
    apply_filament_tds(config.filaments);

    config.families.reflective_anchor = m_family_anchor->GetValue();
    config.families.td_ladder         = m_family_td_ladder->GetValue();
    config.families.pair_mix          = m_family_pair_mix->GetValue();
    config.families.pair_order        = false;
    config.families.ternary_mix       = m_family_ternary->GetValue();
    config.families.quaternary_mix    = m_family_quaternary->GetValue();
    config.families.layer_line_strip  = false;

    const Vec2d bed_size = current_bed_size_mm(m_plater);
    const double plate_buffer = std::max(0.0, m_plate_buffer->GetValue());
    const double swatch_depth = std::max(0.2, m_anchor_thick->GetValue());
    const double layer_height = std::max(0.01, current_layer_height_mm());

    config.layout.chip_width_mm  = m_chip_width->GetValue();
    config.layout.chip_depth_mm  = m_chip_depth->GetValue();
    config.layout.spacing_x_mm   = m_spacing->GetValue();
    config.layout.spacing_y_mm   = m_strip_spacing->GetValue();
    config.layout.footprint_depth_mm = 0.0;
    config.layout.margin_x_mm    = plate_buffer;
    config.layout.margin_y_mm    = plate_buffer;
    config.layout.plate_width_mm = std::max(50.0, bed_size.x());
    config.layout.plate_depth_mm = std::max(50.0, bed_size.y());
    config.layout.reserve_prime_tower = m_prime_tower_reserve->GetValue();
    config.layout.prime_tower_width_mm = m_prime_tower_width->GetValue();
    config.layout.prime_tower_depth_mm = m_prime_tower_depth->GetValue();
    config.layout.multi_plate    = true;
    config.nominal_layer_height_mm = layer_height;
    config.local_z_enabled             = m_local_z_enabled->GetValue();
    config.local_z_direct_multicolor   = config.local_z_enabled && m_direct_multicolor_solver->GetValue();
    config.explicit_ratio_rows         = m_ratio_file_rows;
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
    config.quaternary_thickness_mm = swatch_depth;
    config.pair_mix_layer_height_mm = layer_height;
    config.pair_order_layer_height_mm = layer_height;
    config.ternary_layer_height_mm = layer_height;
    config.quaternary_layer_height_mm = layer_height;
    config.layer_line_strip_layer_height_mm = layer_height;
    if (config.families.td_ladder) {
        std::vector<double> td_ladder_widths;
        if (parse_positive_mm_list(m_td_ladder_widths->GetValue(), td_ladder_widths))
            config.td_ladder_thicknesses = std::move(td_ladder_widths);
    } else {
        config.td_ladder_thicknesses = { config.anchor_thickness_mm };
    }
    config.pair_ratio_layer_limit = static_cast<unsigned int>(std::max(m_pair_layer_limit->GetValue(), 1));
    config.quaternary_ratio_layer_limit = static_cast<unsigned int>(std::max(m_quaternary_layer_limit->GetValue(), 1));

    config.swatch_reference.enabled = true;
    config.swatch_reference.plate_reference = wx_to_u8(m_plate_reference->GetValue());
    if (config.swatch_reference.plate_reference.empty())
        config.swatch_reference.plate_reference = "A";
    config.swatch_reference.text_size_mm = m_reference_text_size->GetValue();
    config.swatch_reference.text_depth_mm = m_reference_text_depth->GetValue();
    config.swatch_reference.stroke_width_mm = m_reference_text_stroke_width->GetValue();
    return config;
}

void CalibrationSwatchesDialog::on_open_ratio_file(wxCommandEvent &)
{
    wxFileDialog dlg(this,
                     _L("Open ratio spreadsheet"),
                     wxEmptyString,
                     wxEmptyString,
                     _L("Excel workbooks (*.xlsx;*.xlsm)|*.xlsx;*.xlsm|All files (*.*)|*.*"),
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;

    std::vector<std::vector<int>> rows;
    wxString error;
    if (!load_xlsx_ratio_rows(dlg.GetPath(), rows, &error)) {
        MessageDialog(this, error, _L("Calibration swatches"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }

    m_ratio_file_path = dlg.GetPath();
    m_ratio_file_rows = std::move(rows);
    update_preview();
}

void CalibrationSwatchesDialog::on_clear_ratio_file(wxCommandEvent &)
{
    m_ratio_file_path.clear();
    m_ratio_file_rows.clear();
    update_preview();
}

void CalibrationSwatchesDialog::update_preview()
{
    if (m_direct_multicolor_solver != nullptr)
        m_direct_multicolor_solver->Enable(m_local_z_enabled != nullptr && m_local_z_enabled->GetValue());
    const bool ratio_file_loaded = !m_ratio_file_rows.empty();
    for (wxWindow *control : { static_cast<wxWindow*>(m_family_anchor),
                               static_cast<wxWindow*>(m_family_td_ladder),
                               static_cast<wxWindow*>(m_family_pair_mix),
                               static_cast<wxWindow*>(m_family_ternary),
                               static_cast<wxWindow*>(m_family_quaternary),
                               static_cast<wxWindow*>(m_pair_layer_limit),
                               static_cast<wxWindow*>(m_quaternary_layer_limit) }) {
        if (control != nullptr)
            control->Enable(!ratio_file_loaded);
    }
    if (m_td_ladder_widths != nullptr)
        m_td_ladder_widths->Enable(!ratio_file_loaded && m_family_td_ladder != nullptr && m_family_td_ladder->GetValue());
    if (m_ratio_file_clear != nullptr)
        m_ratio_file_clear->Enable(ratio_file_loaded);
    if (m_ratio_file_status != nullptr) {
        if (ratio_file_loaded) {
            const wxString file_name = wxFileName(m_ratio_file_path).GetFullName();
            const RatioRowSummary ratio_summary = summarize_ratio_rows(m_ratio_file_rows);
            m_ratio_file_status->SetLabel(wxString::Format(_L("Loaded %llu ratio rows from %s: %llu unique recipes, %llu mixed material definitions, %llu one-filament rows. Automatic swatch families are ignored until the file is cleared."),
                                                           static_cast<unsigned long long>(m_ratio_file_rows.size()),
                                                           file_name.c_str(),
                                                           static_cast<unsigned long long>(ratio_summary.unique_recipes),
                                                           static_cast<unsigned long long>(ratio_summary.unique_mixed_recipes),
                                                           static_cast<unsigned long long>(ratio_summary.one_filament_rows)));
        } else {
            m_ratio_file_status->SetLabel(_L("No ratio file loaded. When loaded, spreadsheet rows replace the automatic mix proportions."));
        }
        m_ratio_file_status->Wrap(FromDIP(500));
    }

    std::vector<ColorCalibrationSwatches::FilamentSlot> filaments = current_filaments();
    wxString filament_td_error;
    if (!apply_filament_tds(filaments, &filament_td_error)) {
        m_preview->SetLabel(filament_td_error);
        return;
    }

    if (m_family_td_ladder != nullptr && m_family_td_ladder->GetValue()) {
        std::vector<double> td_ladder_widths;
        wxString td_ladder_error;
        if (!parse_positive_mm_list(m_td_ladder_widths->GetValue(), td_ladder_widths, &td_ladder_error)) {
            m_preview->SetLabel(td_ladder_error);
            return;
        }
    }

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
    if (config.spectro_jig.enabled)
        ++plate_count;

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
    if (ratio_file_loaded)
        text += wxString::Format(_L(" Ratio file: %llu row(s)."), static_cast<unsigned long long>(m_ratio_file_rows.size()));
    m_preview->SetLabel(text);
}

void CalibrationSwatchesDialog::on_generate(wxCommandEvent &)
{
    std::vector<ColorCalibrationSwatches::FilamentSlot> filaments = current_filaments();
    wxString filament_td_error;
    if (!apply_filament_tds(filaments, &filament_td_error)) {
        MessageDialog(this, filament_td_error, _L("Calibration swatches"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }

    if (m_family_td_ladder != nullptr && m_family_td_ladder->GetValue()) {
        std::vector<double> td_ladder_widths;
        wxString td_ladder_error;
        if (!parse_positive_mm_list(m_td_ladder_widths->GetValue(), td_ladder_widths, &td_ladder_error)) {
            MessageDialog(this, td_ladder_error, _L("Calibration swatches"), wxOK | wxICON_WARNING).ShowModal();
            return;
        }
    }

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
