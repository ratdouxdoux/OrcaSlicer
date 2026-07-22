#include "MixedFilamentExport.hpp"

#include "Zipper.hpp"

#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/stat.hpp>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <locale>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

namespace Slic3r {
namespace {

const MixedFilamentExportPhysicalFilament* find_physical(const std::vector<MixedFilamentExportPhysicalFilament>& physical_filaments,
                                                         unsigned int                                            id)
{
    const auto it = std::find_if(physical_filaments.begin(), physical_filaments.end(),
                                 [id](const MixedFilamentExportPhysicalFilament& physical) { return physical.id == id; });
    return it == physical_filaments.end() ? nullptr : &*it;
}

std::vector<int> normalized_positive_ratios(const std::vector<int>& ratios)
{
    if (ratios.empty() || std::any_of(ratios.begin(), ratios.end(), [](int ratio) { return ratio <= 0; }))
        return {};

    int common = 0;
    for (const int ratio : ratios)
        common = std::gcd(common, ratio);
    common = std::max(1, common);

    std::vector<int> normalized = ratios;
    for (int& ratio : normalized)
        ratio /= common;
    return normalized;
}

std::string ratio_key(const std::vector<int>& ratios)
{
    std::ostringstream out;
    for (size_t i = 0; i < ratios.size(); ++i) {
        if (i > 0)
            out << ':';
        out << ratios[i];
    }
    return out.str();
}

void append_positive_compositions(unsigned int                   remaining,
                                  size_t                         components_left,
                                  std::vector<int>&              current,
                                  std::vector<std::vector<int>>& out)
{
    if (components_left == 1) {
        current.push_back(int(remaining));
        out.push_back(current);
        current.pop_back();
        return;
    }

    const unsigned int maximum = remaining - unsigned(components_left - 1);
    for (unsigned int value = 1; value <= maximum; ++value) {
        current.push_back(int(value));
        append_positive_compositions(remaining - value, components_left - 1, current, out);
        current.pop_back();
    }
}

std::vector<std::vector<int>> canonical_pair_ratios()
{
    std::vector<std::vector<int>> out{{1, 1}};
    for (int ratio = 2; ratio <= 5; ++ratio)
        out.push_back({1, ratio});
    for (int ratio = 2; ratio <= 5; ++ratio)
        out.push_back({ratio, 1});
    return out;
}

std::vector<std::vector<int>> canonical_ternary_ratios()
{
    std::vector<std::vector<int>> out;
    for (unsigned int total = 3; total <= 5; ++total) {
        std::vector<int> current;
        append_positive_compositions(total, 3, current, out);
    }
    return out;
}

void append_bounded_ratios(size_t components_left, std::vector<int>& current, std::vector<std::vector<int>>& out)
{
    if (components_left == 0) {
        out.push_back(current);
        return;
    }
    for (int value = 1; value <= 3; ++value) {
        current.push_back(value);
        append_bounded_ratios(components_left - 1, current, out);
        current.pop_back();
    }
}

std::vector<std::vector<int>> canonical_quaternary_ratios()
{
    std::vector<std::vector<int>> generated;
    std::vector<int>              current;
    append_bounded_ratios(4, current, generated);

    std::vector<std::vector<int>> out;
    std::set<std::string>         seen;
    for (const std::vector<int>& ratios : generated) {
        std::vector<int> normalized = normalized_positive_ratios(ratios);
        if (seen.insert(ratio_key(normalized)).second)
            out.push_back(std::move(normalized));
    }
    return out;
}

std::vector<MixedFilamentColorInput> prediction_inputs(const std::vector<MixedFilamentExportPhysicalFilament>& physical_filaments,
                                                       const std::vector<MixedFilamentExportComponent>&        components)
{
    std::vector<MixedFilamentColorInput> out;
    out.reserve(components.size());
    int total_ratio = 0;
    for (const MixedFilamentExportComponent& component : components) {
        const MixedFilamentExportPhysicalFilament* physical = find_physical(physical_filaments, component.physical_filament_id);
        if (physical == nullptr || component.ratio < 0)
            return {};
        out.push_back({physical->color_hex, component.ratio, physical->td_mm});
        total_ratio += component.ratio;
    }
    return total_ratio > 0 ? out : std::vector<MixedFilamentColorInput>{};
}

void assign_predictions(MixedFilamentExportRecipe&                              recipe,
                        const std::vector<MixedFilamentExportPhysicalFilament>& physical_filaments,
                        const MixedFilamentExportPredictionOptions&             options)
{
    const std::vector<MixedFilamentColorInput> inputs = prediction_inputs(physical_filaments, recipe.components);
    if (inputs.empty())
        return;

    recipe.filament_mixer_hex = MixedFilamentManager::blend_color_multi_with_engine(inputs, MixedFilamentColorEngine::FilamentMixer,
                                                                                    options.use_td);
    if (options.include_km_ks) {
        recipe.km_ks_hex = MixedFilamentManager::blend_color_multi_with_engine(inputs, MixedFilamentColorEngine::FullSpectrumKSPairResidual,
                                                                               options.use_td);
    }
}

std::string numbered_id(const char* prefix, size_t number)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << prefix << '-' << std::setw(4) << std::setfill('0') << number;
    return out.str();
}

void append_automatic_family(std::vector<MixedFilamentExportRecipe>&                        recipes,
                             const std::vector<MixedFilamentExportPhysicalFilament>&        all_physical_filaments,
                             const std::vector<const MixedFilamentExportPhysicalFilament*>& combination,
                             const std::vector<std::vector<int>>&                           ratios,
                             const MixedFilamentExportPredictionOptions&                    prediction_options)
{
    for (const std::vector<int>& ratio : ratios) {
        MixedFilamentExportRecipe recipe;
        recipe.recipe_id = numbered_id("AUTO", recipes.size() + 1);
        recipe.name      = std::to_string(combination.size()) + "-filament mix " + ratio_key(ratio);
        recipe.source    = MixedFilamentExportRecipeSource::Automatic;
        recipe.components.reserve(combination.size());
        for (size_t i = 0; i < combination.size(); ++i)
            recipe.components.push_back({combination[i]->id, ratio[i]});
        assign_predictions(recipe, all_physical_filaments, prediction_options);
        if (!recipe.filament_mixer_hex.empty())
            recipes.push_back(std::move(recipe));
    }
}

std::string xml_escape(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 16);
    for (const unsigned char byte : text) {
        if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r')
            continue;
        switch (byte) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '\"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out.push_back(char(byte)); break;
        }
    }
    return out;
}

std::optional<std::string> normalized_argb(const std::string& hex)
{
    std::string value = hex;
    if (!value.empty() && value.front() == '#')
        value.erase(value.begin());
    if (value.size() != 6 || !std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; }))
        return std::nullopt;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return char(std::toupper(ch)); });
    return "FF" + value;
}

std::optional<std::string> quantized_fill_argb(const std::string& hex)
{
    const std::optional<std::string> argb = normalized_argb(hex);
    if (!argb)
        return std::nullopt;

    std::ostringstream out;
    out << "FF" << std::uppercase << std::hex << std::setfill('0');
    for (size_t offset : {size_t(2), size_t(4), size_t(6)}) {
        const int channel   = std::stoi(argb->substr(offset, 2), nullptr, 16);
        const int quantized = std::min(255, ((channel + 25) / 51) * 51);
        out << std::setw(2) << quantized;
    }
    return out.str();
}

std::string column_name(size_t zero_based)
{
    std::string out;
    for (size_t value = zero_based + 1; value > 0; value = (value - 1) / 26)
        out.insert(out.begin(), char('A' + ((value - 1) % 26)));
    return out;
}

enum class CellKind : uint8_t { Text, Number };

struct SheetCell
{
    CellKind    kind = CellKind::Text;
    std::string text;
    double      number = 0.0;
    size_t      style  = 2;
};

SheetCell text_cell(std::string text, size_t style = 2)
{
    SheetCell cell;
    cell.text  = std::move(text);
    cell.style = style;
    return cell;
}

SheetCell number_cell(double number, size_t style = 3)
{
    SheetCell cell;
    cell.kind   = CellKind::Number;
    cell.number = number;
    cell.style  = style;
    return cell;
}

std::string numeric_text(double value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(15) << value;
    return out.str();
}

std::string worksheet_xml(const std::vector<std::string>&            headers,
                          const std::vector<std::vector<SheetCell>>& rows,
                          const std::vector<double>&                 widths)
{
    const size_t      last_row    = rows.size() + 1;
    const std::string last_column = column_name(headers.empty() ? 0 : headers.size() - 1);

    std::ostringstream xml;
    xml.imbue(std::locale::classic());
    xml << R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        << R"(<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
        << "<dimension ref=\"A1:" << last_column << last_row << "\"/>"
        << R"(<sheetViews><sheetView workbookViewId="0"><pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/><selection pane="bottomLeft" activeCell="A2" sqref="A2"/></sheetView></sheetViews>)"
        << R"(<sheetFormatPr defaultRowHeight="15"/> )";
    if (!widths.empty()) {
        xml << "<cols>";
        for (size_t i = 0; i < widths.size(); ++i)
            xml << "<col min=\"" << (i + 1) << "\" max=\"" << (i + 1) << "\" width=\"" << widths[i] << "\" customWidth=\"1\"/>";
        xml << "</cols>";
    }

    xml << "<sheetData><row r=\"1\" ht=\"22\" customHeight=\"1\">";
    for (size_t column = 0; column < headers.size(); ++column) {
        xml << "<c r=\"" << column_name(column) << "1\" s=\"1\" t=\"inlineStr\"><is><t>" << xml_escape(headers[column]) << "</t></is></c>";
    }
    xml << "</row>";

    for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
        const size_t excel_row = row_index + 2;
        xml << "<row r=\"" << excel_row << "\">";
        for (size_t column = 0; column < rows[row_index].size(); ++column) {
            const SheetCell& cell = rows[row_index][column];
            xml << "<c r=\"" << column_name(column) << excel_row << "\" s=\"" << cell.style << "\"";
            if (cell.kind == CellKind::Text) {
                xml << " t=\"inlineStr\"><is><t xml:space=\"preserve\">" << xml_escape(cell.text) << "</t></is></c>";
            } else {
                xml << "><v>" << numeric_text(cell.number) << "</v></c>";
            }
        }
        xml << "</row>";
    }
    xml << "</sheetData><autoFilter ref=\"A1:" << last_column << last_row << "\"/></worksheet>";
    return xml.str();
}

std::map<std::string, size_t> color_styles(const std::vector<MixedFilamentExportPhysicalFilament>& physical_filaments,
                                           const std::vector<MixedFilamentExportRecipe>&           recipes)
{
    // A six-level RGB preview palette provides every valid color with a nearby
    // fill while staying below Excel's 256-fill-style workbook limit. The cell
    // text always retains the exact, unquantized hex prediction.
    std::map<std::string, size_t> styles;
    const auto                    add_color = [&styles](const std::string& hex) {
        if (const auto argb = quantized_fill_argb(hex); argb && styles.find(*argb) == styles.end())
            styles.emplace(*argb, 6 + styles.size());
    };

    for (const MixedFilamentExportPhysicalFilament& physical : physical_filaments)
        add_color(physical.color_hex);
    for (const MixedFilamentExportRecipe& recipe : recipes) {
        add_color(recipe.filament_mixer_hex);
        if (recipe.km_ks_hex)
            add_color(*recipe.km_ks_hex);
    }
    return styles;
}

size_t style_for_hex(const std::string& hex, const std::map<std::string, size_t>& styles)
{
    const auto argb = quantized_fill_argb(hex);
    if (!argb)
        return 2;
    const auto it = styles.find(*argb);
    return it == styles.end() ? 2 : it->second;
}

bool hex_fill_needs_white_text(const std::string& argb)
{
    if (argb.size() != 8)
        return false;
    const auto channel = [&argb](size_t offset) { return std::stoi(argb.substr(offset, 2), nullptr, 16); };
    const int  red     = channel(2);
    const int  green   = channel(4);
    const int  blue    = channel(6);
    return (299 * red + 587 * green + 114 * blue) / 1000 < 150;
}

std::string styles_xml(const std::map<std::string, size_t>& color_style_ids)
{
    std::ostringstream xml;
    xml << R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        << R"(<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
        << R"(<fonts count="3"><font><sz val="11"/><color rgb="FF000000"/><name val="Calibri"/><family val="2"/></font><font><b/><sz val="11"/><color rgb="FFFFFFFF"/><name val="Calibri"/><family val="2"/></font><font><sz val="11"/><color rgb="FFFFFFFF"/><name val="Calibri"/><family val="2"/></font></fonts>)"
        << "<fills count=\"" << (3 + color_style_ids.size()) << "\"><fill><patternFill patternType=\"none\"/></fill>"
        << R"(<fill><patternFill patternType="gray125"/></fill><fill><patternFill patternType="solid"><fgColor rgb="FF1F4E78"/><bgColor indexed="64"/></patternFill></fill>)";
    for (const auto& [argb, style_id] : color_style_ids) {
        (void) style_id;
        xml << "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"" << argb << "\"/><bgColor indexed=\"64\"/></patternFill></fill>";
    }
    xml << R"(</fills><borders count="2"><border><left/><right/><top/><bottom/><diagonal/></border><border><left style="thin"><color rgb="FFD9E1F2"/></left><right style="thin"><color rgb="FFD9E1F2"/></right><top style="thin"><color rgb="FFD9E1F2"/></top><bottom style="thin"><color rgb="FFD9E1F2"/></bottom><diagonal/></border></borders>)"
        << R"(<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>)"
        << "<cellXfs count=\"" << (6 + color_style_ids.size()) << "\">"
        << R"(<xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>)"
        << R"(<xf numFmtId="0" fontId="1" fillId="2" borderId="1" xfId="0" applyFill="1" applyFont="1" applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>)"
        << R"(<xf numFmtId="0" fontId="0" fillId="0" borderId="1" xfId="0" applyBorder="1" applyAlignment="1"><alignment vertical="center"/></xf>)"
        << R"(<xf numFmtId="0" fontId="0" fillId="0" borderId="1" xfId="0" applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>)"
        << R"(<xf numFmtId="4" fontId="0" fillId="0" borderId="1" xfId="0" applyNumberFormat="1" applyBorder="1"/>)"
        << R"(<xf numFmtId="10" fontId="0" fillId="0" borderId="1" xfId="0" applyNumberFormat="1" applyBorder="1"/>)";
    size_t fill_id = 3;
    for (const auto& [argb, style_id] : color_style_ids) {
        (void) style_id;
        const int font_id = hex_fill_needs_white_text(argb) ? 2 : 0;
        xml << "<xf numFmtId=\"0\" fontId=\"" << font_id << "\" fillId=\"" << fill_id++
            << R"(" borderId="1" xfId="0" applyFill="1" applyFont="1" applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>)";
    }
    xml << R"(</cellXfs><cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>)";
    return xml.str();
}

std::string recipe_ratio_summary(const MixedFilamentExportRecipe& recipe)
{
    std::vector<int> ratios;
    ratios.reserve(recipe.components.size());
    for (const MixedFilamentExportComponent& component : recipe.components)
        ratios.push_back(component.ratio);
    return ratio_key(ratios);
}

std::string recipe_filament_summary(const MixedFilamentExportRecipe& recipe)
{
    std::ostringstream out;
    for (size_t i = 0; i < recipe.components.size(); ++i) {
        if (i > 0)
            out << " / ";
        out << 'F' << recipe.components[i].physical_filament_id;
    }
    return out.str();
}

std::string recipe_percentage_summary(const MixedFilamentExportRecipe& recipe)
{
    const int          total = std::accumulate(recipe.components.begin(), recipe.components.end(), 0,
                                               [](int value, const MixedFilamentExportComponent& component) {
                                          return value + std::max(0, component.ratio);
                                      });
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(2);
    for (size_t i = 0; i < recipe.components.size(); ++i) {
        if (i > 0)
            out << " / ";
        const double percentage = total > 0 ? (100.0 * double(recipe.components[i].ratio) / double(total)) : 0.0;
        out << percentage << '%';
    }
    return out.str();
}

std::vector<std::vector<SheetCell>> mixed_rows(const std::vector<MixedFilamentExportRecipe>& recipes,
                                               const std::map<std::string, size_t>&          styles,
                                               bool                                          include_km_ks)
{
    std::vector<std::vector<SheetCell>> rows;
    rows.reserve(recipes.size());
    for (const MixedFilamentExportRecipe& recipe : recipes) {
        std::vector<SheetCell> row;
        row.push_back(text_cell(recipe.recipe_id));
        row.push_back(text_cell(recipe.name));
        row.push_back(text_cell(recipe.source == MixedFilamentExportRecipeSource::Current ? "Current" : "Automatic"));
        row.push_back(number_cell(double(recipe.components.size())));
        row.push_back(text_cell(recipe_filament_summary(recipe)));
        row.push_back(text_cell(recipe_ratio_summary(recipe)));
        row.push_back(text_cell(recipe_percentage_summary(recipe)));
        row.push_back(text_cell(recipe.filament_mixer_hex, style_for_hex(recipe.filament_mixer_hex, styles)));
        if (include_km_ks)
            row.push_back(text_cell(recipe.km_ks_hex.value_or(""), recipe.km_ks_hex ? style_for_hex(*recipe.km_ks_hex, styles) : 2));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::vector<SheetCell>> component_rows(const std::vector<MixedFilamentExportPhysicalFilament>& physical_filaments,
                                                   const std::vector<MixedFilamentExportRecipe>&           recipes,
                                                   const std::map<std::string, size_t>&                    styles)
{
    std::vector<std::vector<SheetCell>> rows;
    for (const MixedFilamentExportRecipe& recipe : recipes) {
        const int total_ratio = std::accumulate(recipe.components.begin(), recipe.components.end(), 0,
                                                [](int total, const MixedFilamentExportComponent& component) {
                                                    return total + std::max(0, component.ratio);
                                                });
        for (size_t i = 0; i < recipe.components.size(); ++i) {
            const MixedFilamentExportComponent&        component = recipe.components[i];
            const MixedFilamentExportPhysicalFilament* physical  = find_physical(physical_filaments, component.physical_filament_id);
            rows.push_back({text_cell(recipe.recipe_id), number_cell(double(i + 1)), number_cell(double(component.physical_filament_id)),
                            text_cell(physical == nullptr ? std::string() : physical->name),
                            text_cell(physical == nullptr ? std::string() : physical->color_hex,
                                      physical == nullptr ? 2 : style_for_hex(physical->color_hex, styles)),
                            number_cell(double(component.ratio)),
                            number_cell(total_ratio > 0 ? double(component.ratio) / double(total_ratio) : 0.0, 5)});
        }
    }
    return rows;
}

std::vector<std::vector<SheetCell>> physical_rows(const std::vector<MixedFilamentExportPhysicalFilament>& physical_filaments,
                                                  const std::map<std::string, size_t>&                    styles)
{
    std::vector<std::vector<SheetCell>> rows;
    rows.reserve(physical_filaments.size());
    for (const MixedFilamentExportPhysicalFilament& physical : physical_filaments) {
        std::vector<SheetCell> row{number_cell(double(physical.id)), text_cell(physical.name),
                                   text_cell(physical.color_hex, style_for_hex(physical.color_hex, styles))};
        if (physical.td_mm)
            row.push_back(number_cell(*physical.td_mm, 4));
        else
            row.push_back(text_cell(""));
        row.push_back(text_cell(physical.selected ? "Yes" : "No", 3));
        rows.push_back(std::move(row));
    }
    return rows;
}

void add_text_entry(Zipper& zipper, const std::string& name, const std::string& contents)
{
    zipper.add_entry(name, contents.data(), contents.size());
}

std::string available_sibling_path(const std::string& output_path, const char* suffix)
{
    for (size_t attempt = 0; attempt < 1000; ++attempt) {
        std::string candidate = output_path + suffix;
        if (attempt > 0)
            candidate += '-' + std::to_string(attempt);

        boost::nowide::stat_t status{};
        if (boost::nowide::stat(candidate.c_str(), &status) != 0) {
            if (errno == ENOENT)
                return candidate;
            throw std::runtime_error("Could not inspect a temporary XLSX path: " + std::string(std::strerror(errno)));
        }
    }
    throw std::runtime_error("Could not reserve a temporary XLSX path.");
}

bool is_regular_file(const boost::nowide::stat_t& status)
{
#ifdef _WIN32
    return (status.st_mode & _S_IFMT) == _S_IFREG;
#else
    return S_ISREG(status.st_mode);
#endif
}

MixedFilamentXlsxWriteResult install_completed_workbook(const std::string& temporary_path, const std::string& output_path)
{
    if (boost::nowide::rename(temporary_path.c_str(), output_path.c_str()) == 0)
        return {true, {}};

    const int             first_rename_error = errno;
    boost::nowide::stat_t output_status{};
    if (boost::nowide::stat(output_path.c_str(), &output_status) != 0)
        return {false, "Could not move the completed XLSX workbook into place: " + std::string(std::strerror(first_rename_error))};
    if (!is_regular_file(output_status))
        return {false, "The selected XLSX output path is not a regular file."};

    // Windows rename does not replace an existing file. Move the old workbook
    // aside first and restore it if installing the completed file fails.
    const std::string backup_path = available_sibling_path(output_path, ".orca-export-backup");
    if (boost::nowide::rename(output_path.c_str(), backup_path.c_str()) != 0)
        return {false, "Could not preserve the existing XLSX workbook before replacement: " + std::string(std::strerror(errno))};

    if (boost::nowide::rename(temporary_path.c_str(), output_path.c_str()) != 0) {
        const int install_error = errno;
        if (boost::nowide::rename(backup_path.c_str(), output_path.c_str()) != 0) {
            return {false, "Could not install the completed XLSX workbook or restore the previous file. The previous file remains at: " +
                               backup_path};
        }
        return {false, "Could not move the completed XLSX workbook into place; the previous file was restored: " +
                           std::string(std::strerror(install_error))};
    }

    boost::nowide::remove(backup_path.c_str());
    return {true, {}};
}

} // namespace

std::vector<MixedFilamentExportRecipe> make_current_mixed_filament_export_recipes(
    const std::vector<MixedFilamentExportPhysicalFilament>& physical_filaments,
    const std::vector<MixedFilament>&                       mixed_filaments,
    const MixedFilamentDisplayContext&                      display_context,
    const MixedFilamentExportPredictionOptions&             prediction_options)
{
    std::vector<MixedFilamentExportRecipe> recipes;
    recipes.reserve(mixed_filaments.size());
    for (const MixedFilament& mixed : mixed_filaments) {
        if (!mixed.enabled || mixed.deleted)
            continue;

        const std::vector<MixedFilamentResolvedComponent> resolved = resolve_mixed_filament_display_components(mixed, display_context);
        if (resolved.empty())
            continue;

        MixedFilamentExportRecipe recipe;
        recipe.recipe_id = numbered_id("CURRENT", recipes.size() + 1);
        recipe.name      = "Mixed filament " + std::to_string(recipes.size() + 1);
        recipe.source    = MixedFilamentExportRecipeSource::Current;
        recipe.components.reserve(resolved.size());
        for (const MixedFilamentResolvedComponent& component : resolved)
            recipe.components.push_back({component.physical_filament_id, component.ratio});
        assign_predictions(recipe, physical_filaments, prediction_options);
        if (!recipe.filament_mixer_hex.empty())
            recipes.push_back(std::move(recipe));
    }
    return recipes;
}

std::vector<MixedFilamentExportRecipe> make_automatic_mixed_filament_export_recipes(
    const std::vector<MixedFilamentExportPhysicalFilament>& physical_filaments, const MixedFilamentAutomaticExportOptions& options)
{
    std::vector<const MixedFilamentExportPhysicalFilament*> selected;
    std::set<unsigned int>                                  seen_ids;
    for (const MixedFilamentExportPhysicalFilament& physical : physical_filaments) {
        if (physical.selected && physical.id > 0 && seen_ids.insert(physical.id).second)
            selected.push_back(&physical);
    }

    std::vector<MixedFilamentExportRecipe> recipes;
    if (options.include_two_filament) {
        const std::vector<std::vector<int>> ratios = canonical_pair_ratios();
        for (size_t a = 0; a < selected.size(); ++a)
            for (size_t b = a + 1; b < selected.size(); ++b)
                append_automatic_family(recipes, physical_filaments, {selected[a], selected[b]}, ratios, options.prediction);
    }
    if (options.include_three_filament) {
        const std::vector<std::vector<int>> ratios = canonical_ternary_ratios();
        for (size_t a = 0; a < selected.size(); ++a)
            for (size_t b = a + 1; b < selected.size(); ++b)
                for (size_t c = b + 1; c < selected.size(); ++c)
                    append_automatic_family(recipes, physical_filaments, {selected[a], selected[b], selected[c]}, ratios,
                                            options.prediction);
    }
    if (options.include_four_filament) {
        const std::vector<std::vector<int>> ratios = canonical_quaternary_ratios();
        for (size_t a = 0; a < selected.size(); ++a)
            for (size_t b = a + 1; b < selected.size(); ++b)
                for (size_t c = b + 1; c < selected.size(); ++c)
                    for (size_t d = c + 1; d < selected.size(); ++d)
                        append_automatic_family(recipes, physical_filaments, {selected[a], selected[b], selected[c], selected[d]}, ratios,
                                                options.prediction);
    }
    return recipes;
}

MixedFilamentXlsxWriteResult write_mixed_filament_xlsx(const std::string&                                      output_path,
                                                       const std::vector<MixedFilamentExportPhysicalFilament>& physical_filaments,
                                                       const std::vector<MixedFilamentExportRecipe>&           recipes)
{
    if (output_path.empty())
        return {false, "The XLSX output path is empty."};

    std::string temporary_path;
    try {
        temporary_path                   = available_sibling_path(output_path, ".orca-export-tmp");
        constexpr size_t max_data_rows   = 1048575;
        size_t           component_count = 0;
        for (const MixedFilamentExportRecipe& recipe : recipes) {
            if (recipe.components.size() > max_data_rows - std::min(max_data_rows, component_count))
                return {false, "The Recipe Components sheet would exceed Excel's 1,048,576-row limit."};
            component_count += recipe.components.size();
        }
        if (recipes.size() > max_data_rows)
            return {false, "The Mixed Filaments sheet would exceed Excel's 1,048,576-row limit."};
        if (physical_filaments.size() > max_data_rows)
            return {false, "The Physical Filaments sheet would exceed Excel's 1,048,576-row limit."};

        const std::map<std::string, size_t> styles        = color_styles(physical_filaments, recipes);
        const bool                          include_km_ks = std::any_of(recipes.begin(), recipes.end(),
                                                                        [](const MixedFilamentExportRecipe& recipe) { return recipe.km_ks_hex.has_value(); });

        std::vector<std::string> mixed_headers{"Recipe ID",      "Name",   "Source",      "Component Count",
                                               "Filament Slots", "Ratios", "Percentages", "FilamentMixer Hex"};
        std::vector<double>      mixed_widths{16, 28, 14, 17, 22, 18, 28, 20};
        if (include_km_ks) {
            mixed_headers.push_back("KM/K-S Hex");
            mixed_widths.push_back(18);
        }

        const std::string sheet1 = worksheet_xml(mixed_headers, mixed_rows(recipes, styles, include_km_ks), mixed_widths);
        const std::string sheet2 = worksheet_xml({"Recipe ID", "Component Position", "Physical Filament ID", "Physical Filament",
                                                  "Base Hex", "Ratio", "Percentage"},
                                                 component_rows(physical_filaments, recipes, styles), {16, 20, 21, 28, 16, 12, 14});
        const std::string sheet3 = worksheet_xml({"Physical Filament ID", "Name", "Hex Color", "TD (mm)", "Selected for Automatic"},
                                                 physical_rows(physical_filaments, styles), {21, 30, 16, 14, 23});

        {
            Zipper zipper(temporary_path, Zipper::FAST_COMPRESSION);
            add_text_entry(
                zipper, "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/><Override PartName="/xl/worksheets/sheet2.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/><Override PartName="/xl/worksheets/sheet3.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/><Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/><Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/><Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/></Types>)");
            add_text_entry(
                zipper, "_rels/.rels",
                R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/><Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/></Relationships>)");
            add_text_entry(
                zipper, "docProps/core.xml",
                R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:creator>Snapmaker Orca</dc:creator><cp:lastModifiedBy>Snapmaker Orca</cp:lastModifiedBy><dc:title>Mixed Filament Recipes</dc:title></cp:coreProperties>)");
            add_text_entry(
                zipper, "docProps/app.xml",
                R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties" xmlns:vt="http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes"><Application>Snapmaker Orca</Application><HeadingPairs><vt:vector size="2" baseType="variant"><vt:variant><vt:lpstr>Worksheets</vt:lpstr></vt:variant><vt:variant><vt:i4>3</vt:i4></vt:variant></vt:vector></HeadingPairs><TitlesOfParts><vt:vector size="3" baseType="lpstr"><vt:lpstr>Mixed Filaments</vt:lpstr><vt:lpstr>Recipe Components</vt:lpstr><vt:lpstr>Physical Filaments</vt:lpstr></vt:vector></TitlesOfParts></Properties>)");
            add_text_entry(
                zipper, "xl/workbook.xml",
                R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><bookViews><workbookView xWindow="0" yWindow="0" windowWidth="24000" windowHeight="12000"/></bookViews><sheets><sheet name="Mixed Filaments" sheetId="1" r:id="rId1"/><sheet name="Recipe Components" sheetId="2" r:id="rId2"/><sheet name="Physical Filaments" sheetId="3" r:id="rId3"/></sheets></workbook>)");
            add_text_entry(
                zipper, "xl/_rels/workbook.xml.rels",
                R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet2.xml"/><Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet3.xml"/><Relationship Id="rId4" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>)");
            add_text_entry(zipper, "xl/styles.xml", styles_xml(styles));
            add_text_entry(zipper, "xl/worksheets/sheet1.xml", sheet1);
            add_text_entry(zipper, "xl/worksheets/sheet2.xml", sheet2);
            add_text_entry(zipper, "xl/worksheets/sheet3.xml", sheet3);
            zipper.finalize();
        }

        const MixedFilamentXlsxWriteResult install_result = install_completed_workbook(temporary_path, output_path);
        if (!install_result.success) {
            boost::nowide::remove(temporary_path.c_str());
            return install_result;
        }
        return install_result;
    } catch (const std::exception& error) {
        boost::nowide::remove(temporary_path.c_str());
        return {false, error.what()};
    } catch (...) {
        boost::nowide::remove(temporary_path.c_str());
        return {false, "Unknown error while writing the XLSX workbook."};
    }
}

} // namespace Slic3r
