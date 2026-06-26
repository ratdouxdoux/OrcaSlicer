#include "ColorCalibrationSwatches.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

namespace Slic3r {
namespace ColorCalibrationSwatches {

namespace {

static Backing none_backing()
{
    return {};
}

static const std::vector<Backing>& backings_or_none(const std::vector<Backing> &backings)
{
    static const std::vector<Backing> one_none { none_backing() };
    return backings.empty() ? one_none : backings;
}

static std::string uppercase(std::string s)
{
    for (char &ch : s)
        ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

static std::string sanitize_token(const std::string &token)
{
    std::string out;
    out.reserve(token.size());
    for (char ch : token) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch))
            out.push_back(char(std::toupper(uch)));
        else if (ch == '.' || ch == ',')
            out.push_back('p');
        else if (ch == '-' || ch == '+')
            out.push_back(ch);
    }
    return out;
}

static std::string join_strings(const std::vector<std::string> &items, const std::string &separator)
{
    std::ostringstream ss;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0)
            ss << separator;
        ss << items[i];
    }
    return ss.str();
}

static std::string join_ints(const std::vector<int> &items, const std::string &separator)
{
    std::vector<std::string> tokens;
    tokens.reserve(items.size());
    for (int item : items)
        tokens.emplace_back(std::to_string(item));
    return join_strings(tokens, separator);
}

static std::string top_token(unsigned int slot)
{
    return std::to_string(slot) + "TOP";
}

static bool is_ratio_mix(SwatchType type)
{
    return type == SwatchType::PairMix ||
           type == SwatchType::PairOrder ||
           type == SwatchType::TernaryMix ||
           type == SwatchType::QuaternaryMix;
}

static bool is_multicomponent_cycle_mix(SwatchType type)
{
    return type == SwatchType::TernaryMix || type == SwatchType::QuaternaryMix;
}

static int ratio_sum(const std::vector<int> &ratios)
{
    return std::accumulate(ratios.begin(), ratios.end(), 0);
}

static std::string ratio_key(const std::vector<int> &ratios)
{
    return join_ints(ratios, "_");
}

static std::string format_percent_token(double value)
{
    const double rounded = std::round(value * 10.0) / 10.0;
    const double nearest_integer = std::round(rounded);
    if (std::abs(rounded - nearest_integer) < 1e-6)
        return std::to_string(int(nearest_integer));

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << rounded;
    return ss.str();
}

static std::vector<double> ratio_percentages(const std::vector<int> &ratios)
{
    std::vector<double> percentages;
    percentages.reserve(ratios.size());

    const int total = ratio_sum(ratios);
    if (total <= 0)
        return percentages;

    for (const int ratio : ratios)
        percentages.emplace_back(std::round(1000.0 * double(std::max(0, ratio)) / double(total)) / 10.0);
    return percentages;
}

static std::vector<int> integer_percentages(const std::vector<int> &ratios)
{
    std::vector<int> percentages(ratios.size(), 0);
    const int total = ratio_sum(ratios);
    if (total <= 0)
        return {};

    struct Remainder
    {
        size_t index;
        double fraction;
    };

    std::vector<Remainder> remainders;
    remainders.reserve(ratios.size());

    int assigned = 0;
    for (size_t i = 0; i < ratios.size(); ++i) {
        const double exact = 100.0 * double(std::max(0, ratios[i])) / double(total);
        const int whole = int(std::floor(exact));
        percentages[i] = whole;
        assigned += whole;
        remainders.push_back({ i, exact - double(whole) });
    }

    std::sort(remainders.begin(), remainders.end(), [](const Remainder &lhs, const Remainder &rhs) {
        if (std::abs(lhs.fraction - rhs.fraction) > 1e-9)
            return lhs.fraction > rhs.fraction;
        return lhs.index > rhs.index;
    });

    int remaining = 100 - assigned;
    for (size_t i = 0; remaining > 0 && !remainders.empty(); ++i, --remaining)
        ++percentages[remainders[i % remainders.size()].index];

    return percentages;
}

static std::string ratio_percentages_token(const std::vector<int> &ratios, const std::string &separator)
{
    const std::vector<double> percentages = ratio_percentages(ratios);
    std::vector<std::string> tokens;
    tokens.reserve(percentages.size());
    for (double percentage : percentages)
        tokens.emplace_back(format_percent_token(percentage));
    return tokens.empty() ? std::string() : join_strings(tokens, separator) + "%";
}

static std::vector<int> normalized_layer_counts(const std::vector<int> &ratios)
{
    if (ratios.empty())
        return {};

    std::vector<int> counts;
    counts.reserve(ratios.size());
    int g = 0;
    for (const int ratio : ratios) {
        const int count = std::max(0, ratio);
        counts.emplace_back(count);
        if (count > 0)
            g = std::gcd(g, count);
    }

    if (g > 1) {
        for (int &count : counts)
            count /= g;
    }

    return counts;
}

static std::vector<std::vector<int>> deduplicated_layer_ratios(const std::vector<std::vector<int>> &ratios)
{
    std::vector<std::vector<int>> out;
    out.reserve(ratios.size());

    std::set<std::string> seen_effective_cycles;
    for (const std::vector<int> &ratio : ratios) {
        const std::vector<int> normalized_counts = normalized_layer_counts(ratio);
        const std::string key = normalized_counts.empty() ? ratio_key(ratio) : ratio_key(normalized_counts);
        if (seen_effective_cycles.insert(key).second)
            out.emplace_back(normalized_counts.empty() ? ratio : normalized_counts);
    }
    return out;
}

static std::vector<std::vector<int>> deduplicated_pair_layer_ratios(const std::vector<std::vector<int>> &ratios)
{
    return deduplicated_layer_ratios(ratios);
}

struct LayerCycleSummary
{
    std::vector<int> counts;
    std::vector<unsigned int> sequence;
};

static LayerCycleSummary layer_cycle_summary(const SwatchSpec &spec)
{
    LayerCycleSummary summary;
    if (spec.filaments.empty() || spec.ratios.size() != spec.filaments.size())
        return summary;

    std::vector<size_t> positive_indices;
    std::vector<int> counts;
    positive_indices.reserve(spec.ratios.size());
    counts.reserve(spec.ratios.size());
    for (size_t i = 0; i < spec.ratios.size(); ++i) {
        const int count = std::max(0, spec.ratios[i]);
        if (count <= 0)
            continue;
        positive_indices.emplace_back(i);
        counts.emplace_back(count);
    }
    if (positive_indices.empty())
        return summary;

    int g = 0;
    for (const int count : counts)
        g = std::gcd(g, count);
    if (g > 1) {
        for (int &count : counts)
            count = std::max(1, count / g);
    }

    int cycle = std::accumulate(counts.begin(), counts.end(), 0);
    constexpr int max_cycle = 48;
    if (cycle > max_cycle) {
        const double scale = double(max_cycle) / double(cycle);
        for (int &count : counts)
            count = std::max(1, int(std::round(double(count) * scale)));
        cycle = std::accumulate(counts.begin(), counts.end(), 0);
        while (cycle > max_cycle) {
            auto it = std::max_element(counts.begin(), counts.end());
            if (it == counts.end() || *it <= 1)
                break;
            --(*it);
            --cycle;
        }
    }
    if (cycle <= 0)
        return summary;

    summary.counts.assign(spec.ratios.size(), 0);
    for (size_t i = 0; i < positive_indices.size(); ++i)
        summary.counts[positive_indices[i]] = counts[i];

    summary.sequence.reserve(size_t(cycle));
    std::vector<int> emitted(counts.size(), 0);
    for (int pos = 0; pos < cycle; ++pos) {
        size_t best_idx = 0;
        double best_score = -1e9;
        for (size_t i = 0; i < counts.size(); ++i) {
            const double target = double((pos + 1) * counts[i]) / double(cycle);
            const double score = target - double(emitted[i]);
            if (score > best_score) {
                best_score = score;
                best_idx = i;
            }
        }
        ++emitted[best_idx];
        summary.sequence.emplace_back(spec.filaments[positive_indices[best_idx]].slot);
    }

    return summary;
}

static std::vector<int> effective_layer_counts(const SwatchSpec &spec)
{
    if (spec.type == SwatchType::PairMix || spec.type == SwatchType::PairOrder)
        return spec.ratios;
    if (is_multicomponent_cycle_mix(spec.type))
        return layer_cycle_summary(spec).counts;
    return {};
}

static std::string slot_sequence_token(const std::vector<unsigned int> &sequence, const std::string &separator)
{
    if (sequence.empty())
        return {};

    const bool needs_separator = std::any_of(sequence.begin(), sequence.end(), [](unsigned int slot) { return slot > 9; });
    if (needs_separator) {
        std::vector<std::string> tokens;
        tokens.reserve(sequence.size());
        for (unsigned int slot : sequence)
            tokens.emplace_back(std::to_string(slot));
        return join_strings(tokens, separator);
    }

    std::string token;
    token.reserve(sequence.size());
    for (unsigned int slot : sequence)
        token += std::to_string(slot);
    return token;
}

static std::string compact_sequence_token(const std::vector<unsigned int> &sequence, const std::string &separator)
{
    if (sequence.empty())
        return {};

    for (size_t period = 1; period <= sequence.size(); ++period) {
        if (sequence.size() % period != 0)
            continue;
        bool repeats = true;
        for (size_t i = period; i < sequence.size(); ++i) {
            if (sequence[i] != sequence[i % period]) {
                repeats = false;
                break;
            }
        }
        if (!repeats)
            continue;

        const std::vector<unsigned int> prefix(sequence.begin(), sequence.begin() + period);
        const std::string prefix_token = slot_sequence_token(prefix, separator);
        const size_t repeat_count = sequence.size() / period;
        return repeat_count > 1 ? prefix_token + "x" + std::to_string(repeat_count) : prefix_token;
    }

    return slot_sequence_token(sequence, separator);
}

static std::vector<unsigned int> repeated_layer_sequence(const SwatchSpec &spec, const std::vector<int> &counts)
{
    std::vector<unsigned int> sequence;
    if (counts.empty() || spec.filaments.empty())
        return sequence;

    for (size_t i = 0; i < counts.size() && i < spec.filaments.size(); ++i) {
        const int count = std::max(0, counts[i]);
        for (int j = 0; j < count; ++j)
            sequence.emplace_back(spec.filaments[i].slot);
    }
    return sequence;
}

static std::vector<unsigned int> filament_slots(const SwatchSpec &spec)
{
    std::vector<unsigned int> slots;
    slots.reserve(spec.filaments.size());
    for (const FilamentSlot &filament : spec.filaments)
        slots.emplace_back(filament.slot);
    return slots;
}

static nlohmann::json filament_to_json(const FilamentSlot &filament)
{
    nlohmann::json j = {
        { "slot", filament.slot },
        { "name", filament.name },
        { "short_label", filament.short_label },
        { "color_hex", filament.color_hex }
    };
    j["td"] = filament.td ? nlohmann::json(*filament.td) : nlohmann::json(nullptr);
    return j;
}

static nlohmann::json backing_to_json(const Backing &backing)
{
    nlohmann::json j = {
        { "type", backing_label(backing) },
        { "slot", backing.slot },
        { "label", backing.label },
        { "color_hex", backing.color_hex }
    };
    j["td"] = backing.td ? nlohmann::json(*backing.td) : nlohmann::json(nullptr);
    return j;
}

static nlohmann::json primary_filaments_to_json(const std::vector<FilamentSlot> &filaments)
{
    nlohmann::json values = nlohmann::json::array();
    for (const FilamentSlot &filament : filaments)
        values.push_back(filament_to_json(filament));
    return values;
}

static nlohmann::json primary_colors_to_json(const std::vector<FilamentSlot> &filaments)
{
    nlohmann::json values = nlohmann::json::object();
    for (const FilamentSlot &filament : filaments)
        values[std::to_string(filament.slot)] = filament.color_hex;
    return values;
}

static nlohmann::json primary_tds_to_json(const std::vector<FilamentSlot> &filaments)
{
    nlohmann::json values = nlohmann::json::object();
    for (const FilamentSlot &filament : filaments)
        values[std::to_string(filament.slot)] = filament.td ? nlohmann::json(*filament.td) : nlohmann::json(nullptr);
    return values;
}

static std::string csv_escape(std::string value)
{
    size_t pos = 0;
    while ((pos = value.find('\n', pos)) != std::string::npos) {
        value.replace(pos, 1, "\\n");
        pos += 2;
    }

    const bool needs_quotes = value.find_first_of(",\"\r") != std::string::npos;
    if (!needs_quotes)
        return value;

    std::string escaped = "\"";
    for (char ch : value) {
        if (ch == '"')
            escaped += "\"\"";
        else
            escaped += ch;
    }
    escaped += '"';
    return escaped;
}

static std::string join_uints_for_csv(const std::vector<unsigned int> &values)
{
    std::vector<std::string> tokens;
    tokens.reserve(values.size());
    for (unsigned int value : values)
        tokens.emplace_back(std::to_string(value));
    return join_strings(tokens, "|");
}

static std::string join_doubles_for_csv(const std::vector<std::optional<double>> &values)
{
    std::vector<std::string> tokens;
    tokens.reserve(values.size());
    for (const std::optional<double> &value : values)
        tokens.emplace_back(value ? format_decimal_token(*value) : std::string());
    return join_strings(tokens, "|");
}

static std::string join_doubles_for_csv(const std::vector<double> &values)
{
    std::vector<std::string> tokens;
    tokens.reserve(values.size());
    for (const double value : values)
        tokens.emplace_back(format_percent_token(value));
    return join_strings(tokens, "|");
}

static std::vector<std::optional<double>> filament_tds(const SwatchSpec &spec)
{
    std::vector<std::optional<double>> values;
    values.reserve(spec.filaments.size());
    for (const FilamentSlot &filament : spec.filaments)
        values.emplace_back(filament.td);
    return values;
}

static nlohmann::json filament_tds_json(const SwatchSpec &spec)
{
    nlohmann::json values = nlohmann::json::array();
    for (const FilamentSlot &filament : spec.filaments)
        values.push_back(filament.td ? nlohmann::json(*filament.td) : nlohmann::json(nullptr));
    return values;
}

static std::vector<std::string> filament_names(const SwatchSpec &spec)
{
    std::vector<std::string> values;
    values.reserve(spec.filaments.size());
    for (const FilamentSlot &filament : spec.filaments)
        values.emplace_back(filament.name);
    return values;
}

static std::vector<std::string> filament_labels(const SwatchSpec &spec)
{
    std::vector<std::string> values;
    values.reserve(spec.filaments.size());
    for (const FilamentSlot &filament : spec.filaments)
        values.emplace_back(filament.short_label);
    return values;
}

static std::vector<std::string> filament_colors(const SwatchSpec &spec)
{
    std::vector<std::string> values;
    values.reserve(spec.filaments.size());
    for (const FilamentSlot &filament : spec.filaments)
        values.emplace_back(filament.color_hex);
    return values;
}

static std::vector<std::vector<int>> generated_pair_layer_ratios(unsigned int layer_limit)
{
    const unsigned int safe_limit = std::max(1u, layer_limit);
    std::vector<std::vector<int>> ratios;
    ratios.reserve(size_t(safe_limit) * 2u - 1u);
    ratios.push_back({ 1, 1 });
    for (unsigned int n = 2; n <= safe_limit; ++n)
        ratios.push_back({ 1, int(n) });
    for (unsigned int n = 2; n <= safe_limit; ++n)
        ratios.push_back({ int(n), 1 });
    return deduplicated_pair_layer_ratios(ratios);
}

static void append_positive_compositions(unsigned int                   remaining,
                                         size_t                         components_left,
                                         std::vector<int>              &current,
                                         std::vector<std::vector<int>> &ratios)
{
    if (components_left == 1) {
        current.emplace_back(int(remaining));
        ratios.emplace_back(current);
        current.pop_back();
        return;
    }

    for (unsigned int value = 1; value <= remaining - static_cast<unsigned int>(components_left - 1); ++value) {
        current.emplace_back(int(value));
        append_positive_compositions(remaining - value, components_left - 1, current, ratios);
        current.pop_back();
    }
}

static std::vector<std::vector<int>> generated_nary_layer_ratios(size_t components, unsigned int layer_limit)
{
    if (components == 0)
        return {};

    const unsigned int safe_limit = std::max<unsigned int>(static_cast<unsigned int>(components), layer_limit);
    std::vector<std::vector<int>> ratios;
    for (unsigned int total = static_cast<unsigned int>(components); total <= safe_limit; ++total) {
        std::vector<int> current;
        current.reserve(components);
        append_positive_compositions(total, components, current, ratios);
    }
    return deduplicated_layer_ratios(ratios);
}

static void append_component_bounded_ratios(size_t                         components_left,
                                            unsigned int                   component_limit,
                                            std::vector<int>              &current,
                                            std::vector<std::vector<int>> &ratios)
{
    if (components_left == 0) {
        ratios.emplace_back(current);
        return;
    }

    for (unsigned int value = 1; value <= component_limit; ++value) {
        current.emplace_back(int(value));
        append_component_bounded_ratios(components_left - 1, component_limit, current, ratios);
        current.pop_back();
    }
}

static std::vector<std::vector<int>> generated_ternary_layer_ratios(unsigned int layer_limit)
{
    return generated_nary_layer_ratios(3, layer_limit);
}

static std::vector<std::vector<int>> generated_quaternary_layer_ratios(unsigned int component_limit)
{
    const unsigned int safe_limit = std::max(1u, component_limit);
    std::vector<std::vector<int>> ratios;
    std::vector<int> current;
    current.reserve(4);
    append_component_bounded_ratios(4, safe_limit, current, ratios);
    return deduplicated_layer_ratios(ratios);
}

static SwatchRecord make_record(SwatchSpec spec,
                                const IdFormatOptions &id_options)
{
    SwatchRecord record;
    record.spec       = std::move(spec);
    record.swatch_id  = make_swatch_id(record.spec, id_options);
    record.object_name = "CS_" + record.swatch_id;

    record.volume_names.emplace_back("chip_" + record.swatch_id);
    if (has_backing(record.spec.backing))
        record.volume_names.emplace_back("backing_" + record.swatch_id);
    return record;
}

static void append_record(SwatchPlan                 &plan,
                          SwatchSpec                  spec,
                          const SwatchGeneratorConfig &config)
{
    SwatchRecord record = make_record(std::move(spec), config.id_format);
    plan.records.emplace_back(std::move(record));
}

static std::vector<double> anchor_thicknesses_for(const FilamentSlot &filament, const SwatchGeneratorConfig &config)
{
    std::vector<double> thicknesses;
    thicknesses.reserve(3);

    const auto add_unique = [&thicknesses](double value) {
        value = std::max(0.2, value);
        const bool duplicate = std::any_of(thicknesses.begin(), thicknesses.end(), [value](double existing) {
            return std::abs(existing - value) < 1e-6;
        });
        if (!duplicate)
            thicknesses.emplace_back(value);
    };

    if (filament.td && std::isfinite(*filament.td) && *filament.td >= 0.0) {
        add_unique(*filament.td + 1.0);
        add_unique(*filament.td * 0.5 + 1.0);
    } else {
        const double fallback = std::max(0.2, config.anchor_thickness_mm);
        add_unique(fallback);
        add_unique(fallback * 0.5);
    }
    add_unique(1.0);
    return thicknesses;
}

struct LayoutRect
{
    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;
};

static bool rects_overlap(const LayoutRect &a, const LayoutRect &b)
{
    return a.min_x < b.max_x && a.max_x > b.min_x && a.min_y < b.max_y && a.max_y > b.min_y;
}

static bool rect_fits_plate(const LayoutRect &rect, const SwatchLayoutOptions &layout)
{
    return rect.min_x >= layout.margin_x_mm - 1e-6 &&
           rect.min_y >= layout.margin_y_mm - 1e-6 &&
           rect.max_x <= layout.plate_width_mm - layout.margin_x_mm + 1e-6 &&
           rect.max_y <= layout.plate_depth_mm - layout.margin_y_mm + 1e-6;
}

static double record_bed_footprint_depth(const SwatchRecord &record, const SwatchGeneratorConfig &config)
{
    if (config.layout.footprint_depth_mm > 0.0)
        return config.layout.footprint_depth_mm;

    const double material_depth = std::max(0.2, record.spec.total_thickness_mm);
    const double backing_depth = has_backing(record.spec.backing) ? std::max(0.2, std::min(0.4, material_depth * 0.25)) : 0.0;
    return material_depth + backing_depth;
}

static double layout_bed_footprint_depth(const SwatchPlan &plan, const SwatchGeneratorConfig &config)
{
    if (config.layout.footprint_depth_mm > 0.0)
        return config.layout.footprint_depth_mm;

    double depth = 0.0;
    for (const SwatchRecord &record : plan.records)
        depth = std::max(depth, record_bed_footprint_depth(record, config));

    return depth > 0.0 ? depth : std::max(0.2, config.layout.chip_depth_mm);
}

static bool prime_tower_reserved_rect(const SwatchGeneratorConfig &config, LayoutRect &rect)
{
    const SwatchLayoutOptions &layout = config.layout;
    if (!layout.reserve_prime_tower || layout.prime_tower_width_mm <= 0.0 || layout.prime_tower_depth_mm <= 0.0)
        return false;

    const double min_x = layout.margin_x_mm;
    const double max_x = std::min(layout.plate_width_mm - layout.margin_x_mm, min_x + layout.prime_tower_width_mm);
    const double max_y = layout.plate_depth_mm - layout.margin_y_mm;
    const double min_y = std::max(layout.margin_y_mm, max_y - layout.prime_tower_depth_mm);
    if (min_x >= max_x || min_y >= max_y)
        return false;

    rect = { min_x, min_y, max_x, max_y };
    return true;
}

static bool plate_label_reserved_rect(const SwatchGeneratorConfig &config, LayoutRect &rect)
{
    if (!config.plate_label.enabled || config.plate_label.reserved_height_mm <= 0.0 || config.plate_label.reserved_width_mm <= 0.0)
        return false;

    const double max_x = std::max(config.layout.margin_x_mm, config.layout.plate_width_mm - config.plate_label.margin_x_mm);
    const double min_x = std::max(config.layout.margin_x_mm, max_x - config.plate_label.reserved_width_mm);
    const double min_y = config.layout.margin_y_mm;
    const double max_y = std::min(config.layout.plate_depth_mm - config.layout.margin_y_mm,
                                  min_y + config.plate_label.reserved_height_mm);
    if (min_x >= max_x || min_y >= max_y)
        return false;

    rect = { min_x, min_y, max_x, max_y };
    return true;
}

static std::vector<LayoutRect> base_layout_blockers_for_plate(const SwatchGeneratorConfig &config)
{
    std::vector<LayoutRect> blockers;

    LayoutRect rect;
    if (plate_label_reserved_rect(config, rect))
        blockers.emplace_back(rect);
    if (prime_tower_reserved_rect(config, rect))
        blockers.emplace_back(rect);

    return blockers;
}

static std::vector<LayoutRect> layout_blockers_for_plate(const SwatchGeneratorConfig &config, unsigned int plate_index)
{
    (void)plate_index;
    return base_layout_blockers_for_plate(config);
}

static void assign_layout(SwatchPlan &plan, const SwatchGeneratorConfig &config)
{
    const SwatchLayoutOptions &layout = config.layout;
    const double footprint_depth = layout_bed_footprint_depth(plan, config);
    const double step_x = layout.chip_width_mm + layout.spacing_x_mm;
    const double step_y = footprint_depth + layout.spacing_y_mm;
    const double usable_w = layout.plate_width_mm - 2.0 * layout.margin_x_mm;
    const double usable_h = layout.plate_depth_mm - 2.0 * layout.margin_y_mm;

    const unsigned int columns = step_x > 0.0 && usable_w >= layout.chip_width_mm ?
        std::max(1u, static_cast<unsigned int>(std::floor((usable_w + layout.spacing_x_mm) / step_x))) : 1u;
    const unsigned int rows = step_y > 0.0 && usable_h >= footprint_depth ?
        std::max(1u, static_cast<unsigned int>(std::floor((usable_h + layout.spacing_y_mm) / step_y))) : 1u;
    const unsigned int cells_per_plate = std::max(1u, columns * rows);

    unsigned int plate_index = 0;
    unsigned int cell_index = 0;
    unsigned int overflow_index = 0;

    auto assign_position = [&layout, step_x, step_y, columns, footprint_depth](SwatchRecord &record,
                                                                               unsigned int plate,
                                                                               unsigned int cell) {
        const unsigned int row = cell / columns;
        const unsigned int column = cell % columns;
        record.position.plate_index = plate;
        record.position.row         = row;
        record.position.column      = column;
        record.position.x_mm        = layout.margin_x_mm + layout.chip_width_mm * 0.5 + column * step_x;
        record.position.y_mm        = layout.margin_y_mm + footprint_depth * 0.5 + row * step_y;
    };

    for (SwatchRecord &record : plan.records) {
        bool placed = false;
        const unsigned int max_plate_search = layout.multi_plate ?
            static_cast<unsigned int>(std::max<size_t>(plan.records.size() + 16, 32)) :
            1u;
        while (!placed) {
            if (plate_index > max_plate_search) {
                assign_position(record, plate_index, 0);
                record.warnings.emplace_back("layout has no free cells after reserved areas");
                plan.warnings.emplace_back("layout has no free cells after reserved areas");
                placed = true;
                break;
            }

            if (cell_index >= cells_per_plate) {
                if (!layout.multi_plate) {
                    assign_position(record, 0, cells_per_plate + overflow_index++);
                    record.warnings.emplace_back("layout exceeds plate bounds and multi-plate mode is disabled");
                    plan.warnings.emplace_back("layout exceeds plate bounds and multi-plate mode is disabled");
                    placed = true;
                    break;
                }

                ++plate_index;
                cell_index = 0;
            }

            const unsigned int candidate_cell = cell_index++;
            const unsigned int row = candidate_cell / columns;
            const unsigned int column = candidate_cell % columns;
            const double x = layout.margin_x_mm + layout.chip_width_mm * 0.5 + column * step_x;
            const double y = layout.margin_y_mm + footprint_depth * 0.5 + row * step_y;
            const LayoutRect candidate {
                x - layout.chip_width_mm * 0.5,
                y - footprint_depth * 0.5,
                x + layout.chip_width_mm * 0.5,
                y + footprint_depth * 0.5
            };

            const bool fits = candidate.max_x <= layout.plate_width_mm + 1e-6 &&
                              candidate.max_y <= layout.plate_depth_mm + 1e-6;
            if (!fits)
                continue;

            const std::vector<LayoutRect> blockers = layout_blockers_for_plate(config, plate_index);
            const bool blocked = std::any_of(blockers.begin(), blockers.end(), [&candidate](const LayoutRect &blocker) {
                return rects_overlap(candidate, blocker);
            });
            if (blocked)
                continue;

            assign_position(record, plate_index, candidate_cell);
            placed = true;
        }
    }
}

static bool slot_exists(unsigned int slot, const std::vector<FilamentSlot> &filaments)
{
    return std::any_of(filaments.begin(), filaments.end(), [slot](const FilamentSlot &filament) { return filament.slot == slot; });
}

static double max_record_thickness(const SwatchPlan &plan, double fallback)
{
    double max_thickness = fallback;
    for (const SwatchRecord &record : plan.records)
        max_thickness = std::max(max_thickness, record.spec.total_thickness_mm);
    return max_thickness;
}

} // namespace

std::string swatch_type_key(SwatchType type)
{
    switch (type) {
    case SwatchType::ReflectiveAnchor: return "reflective_anchor";
    case SwatchType::TDLadder:         return "td_ladder";
    case SwatchType::PairMix:          return "pair_mix";
    case SwatchType::PairOrder:        return "pair_order";
    case SwatchType::TernaryMix:       return "ternary_mix";
    case SwatchType::QuaternaryMix:    return "four_color_mix";
    case SwatchType::LayerLineStrip:   return "layer_line_strip";
    }
    return "unknown";
}

std::string swatch_type_prefix(SwatchType type)
{
    switch (type) {
    case SwatchType::ReflectiveAnchor: return "A";
    case SwatchType::TDLadder:         return "TD";
    case SwatchType::PairMix:          return "P";
    case SwatchType::PairOrder:        return "O";
    case SwatchType::TernaryMix:       return "T";
    case SwatchType::QuaternaryMix:    return "F";
    case SwatchType::LayerLineStrip:   return "L";
    }
    return "X";
}

std::string backing_label(const Backing &backing)
{
    switch (backing.type) {
    case BackingType::None:   return "";
    case BackingType::Black:  return "BLACK";
    case BackingType::White:  return "WHITE";
    case BackingType::Custom: {
        const std::string token = sanitize_token(backing.label);
        return token.empty() ? "BACKING" : token;
    }
    }
    return "";
}

bool has_backing(const Backing &backing)
{
    return backing.type != BackingType::None;
}

std::string format_decimal_token(double value)
{
    if (std::abs(value) < 0.0005)
        value = 0.0;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(3) << value;
    std::string out = ss.str();
    while (!out.empty() && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    if (out == "-0")
        out = "0";
    for (char &ch : out) {
        if (ch == '.')
            ch = 'p';
        else if (ch == '-')
            ch = 'm';
    }
    return out.empty() ? std::string("0") : out;
}

std::string make_swatch_id(const SwatchSpec &spec, const IdFormatOptions &options)
{
    std::vector<std::string> tokens;
    tokens.reserve(12);

    if (options.include_swatch_type_prefix)
        tokens.emplace_back(swatch_type_prefix(spec.type));

    for (const FilamentSlot &filament : spec.filaments)
        tokens.emplace_back(std::to_string(filament.slot));

    switch (spec.type) {
    case SwatchType::ReflectiveAnchor:
        break;
    case SwatchType::TDLadder:
        if (!options.include_swatch_type_prefix)
            tokens.emplace_back("TD");
        tokens.emplace_back(format_decimal_token(spec.total_thickness_mm));
        break;
    case SwatchType::PairMix:
    case SwatchType::PairOrder:
        for (int ratio : spec.ratios)
            tokens.emplace_back(std::to_string(ratio));
        break;
    case SwatchType::TernaryMix:
    case SwatchType::QuaternaryMix: {
        const std::vector<int> percentages = integer_percentages(spec.ratios);
        if (!percentages.empty()) {
            for (int percentage : percentages)
                tokens.emplace_back(std::to_string(percentage));
        } else {
            for (int ratio : spec.ratios)
                tokens.emplace_back(std::to_string(ratio));
        }
        break;
    }
    case SwatchType::LayerLineStrip:
        break;
    }

    if ((spec.type == SwatchType::PairOrder || spec.type == SwatchType::LayerLineStrip) &&
        options.include_top_material && spec.top_material_slot != 0)
        tokens.emplace_back(top_token(spec.top_material_slot));

    if (options.include_backing && has_backing(spec.backing))
        tokens.emplace_back(backing_label(spec.backing));

    const bool include_thickness = options.include_thickness || spec.type == SwatchType::ReflectiveAnchor;
    if (include_thickness && spec.type != SwatchType::TDLadder && spec.total_thickness_mm > 0.0)
        tokens.emplace_back(format_decimal_token(spec.total_thickness_mm) + "MM");

    return join_strings(tokens, options.separator);
}

static std::string spreadsheet_reference(unsigned int index)
{
    std::string out;
    do {
        const unsigned int digit = index % 26;
        out.push_back(char('A' + digit));
        index = index / 26;
        if (index == 0)
            break;
        --index;
    } while (true);

    std::reverse(out.begin(), out.end());
    return out;
}

static std::string plate_reference_for_index(const std::string &base_reference, unsigned int plate_index)
{
    char base = 'A';
    for (char ch : base_reference) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalpha(uch)) {
            base = char(std::toupper(uch));
            break;
        }
    }

    return spreadsheet_reference(unsigned(base - 'A') + plate_index);
}

static void assign_printed_references(SwatchPlan &plan, const SwatchGeneratorConfig &config)
{
    if (!config.swatch_reference.enabled)
        return;

    for (size_t i = 0; i < plan.records.size(); ++i) {
        SwatchRecord &record = plan.records[i];
        record.sample_number = static_cast<unsigned int>(i + 1);
        record.plate_reference = plate_reference_for_index(config.swatch_reference.plate_reference, record.position.plate_index);
        record.printed_reference = record.plate_reference + std::to_string(record.sample_number);
        record.volume_names.emplace_back("reference_" + record.printed_reference);
    }
}

SwatchPlan generate_swatch_plan(const SwatchGeneratorConfig &config)
{
    SwatchPlan plan;
    plan.primary_filaments       = config.filaments;
    plan.title                   = config.plate_label.title;
    plan.swatch_reference_start  = plate_reference_for_index(config.swatch_reference.plate_reference, 0);
    plan.nominal_layer_height_mm = config.nominal_layer_height_mm;
    plan.swatch_depth_mm         = config.anchor_thickness_mm;
    plan.local_z_enabled         = config.local_z_enabled;
    plan.local_z_direct_multicolor = config.local_z_direct_multicolor;

    if (config.families.reflective_anchor) {
        for (const FilamentSlot &filament : config.filaments) {
            for (double thickness : anchor_thicknesses_for(filament, config)) {
                for (const Backing &backing : backings_or_none(config.anchor_backings)) {
                    SwatchSpec spec;
                    spec.type               = SwatchType::ReflectiveAnchor;
                    spec.filaments          = { filament };
                    spec.backing            = backing;
                    spec.total_thickness_mm = thickness;
                    spec.layer_height_mm    = config.nominal_layer_height_mm;
                    spec.stack_order        = { filament.slot };
                    append_record(plan, std::move(spec), config);
                }
            }
        }
    }

    if (config.families.td_ladder) {
        const std::vector<double> td_ladder_thicknesses =
            config.td_ladder_thicknesses.empty() ? std::vector<double> { config.anchor_thickness_mm } : config.td_ladder_thicknesses;
        for (const FilamentSlot &filament : config.filaments) {
            for (double thickness : td_ladder_thicknesses) {
                for (const Backing &backing : backings_or_none(config.td_ladder_backings)) {
                    SwatchSpec spec;
                    spec.type               = SwatchType::TDLadder;
                    spec.filaments          = { filament };
                    spec.backing            = backing;
                    spec.total_thickness_mm = thickness;
                    spec.layer_height_mm    = config.nominal_layer_height_mm;
                    spec.stack_order        = { filament.slot };
                    append_record(plan, std::move(spec), config);
                }
            }
        }
    }

    if (config.families.pair_mix) {
        const std::vector<std::vector<int>> pair_mix_ratios =
            config.pair_mix_ratios.empty() ?
                generated_pair_layer_ratios(config.pair_ratio_layer_limit) :
                deduplicated_pair_layer_ratios(config.pair_mix_ratios);
        for (size_t i = 0; i < config.filaments.size(); ++i) {
            for (size_t j = i + 1; j < config.filaments.size(); ++j) {
                for (const std::vector<int> &ratio : pair_mix_ratios) {
                    for (const Backing &backing : backings_or_none(config.pair_mix_backings)) {
                        SwatchSpec spec;
                        spec.type               = SwatchType::PairMix;
                        spec.filaments          = { config.filaments[i], config.filaments[j] };
                        spec.ratios             = ratio;
                        spec.backing            = backing;
                        spec.total_thickness_mm = config.pair_mix_thickness_mm;
                        spec.layer_height_mm    = config.pair_mix_layer_height_mm;
                        spec.stack_order        = { config.filaments[i].slot, config.filaments[j].slot };
                        append_record(plan, std::move(spec), config);
                    }
                }
            }
        }
    }

    if (config.families.pair_order) {
        const std::vector<std::vector<int>> pair_order_ratios =
            config.pair_order_ratios.empty() ?
                generated_pair_layer_ratios(config.pair_ratio_layer_limit) :
                deduplicated_pair_layer_ratios(config.pair_order_ratios);
        for (size_t i = 0; i < config.filaments.size(); ++i) {
            for (size_t j = i + 1; j < config.filaments.size(); ++j) {
                const unsigned int a = config.filaments[i].slot;
                const unsigned int b = config.filaments[j].slot;
                for (const std::vector<int> &ratio : pair_order_ratios) {
                    for (unsigned int top : { a, b }) {
                        for (const Backing &backing : backings_or_none(config.pair_order_backings)) {
                            SwatchSpec spec;
                            spec.type               = SwatchType::PairOrder;
                            spec.filaments          = { config.filaments[i], config.filaments[j] };
                            spec.ratios             = ratio;
                            spec.backing            = backing;
                            spec.total_thickness_mm = config.pair_order_thickness_mm;
                            spec.layer_height_mm    = config.pair_order_layer_height_mm;
                            spec.top_material_slot  = top;
                            spec.top_layer_count    = config.pair_order_top_layer_count;
                            spec.stack_order        = top == a ? std::vector<unsigned int>{ b, a } : std::vector<unsigned int>{ a, b };
                            append_record(plan, std::move(spec), config);
                        }
                    }
                }
            }
        }
    }

    if (config.families.ternary_mix) {
        const std::vector<std::vector<int>> ternary_ratios =
            config.ternary_ratios.empty() ?
                generated_ternary_layer_ratios(config.pair_ratio_layer_limit) :
                deduplicated_layer_ratios(config.ternary_ratios);
        for (size_t i = 0; i < config.filaments.size(); ++i) {
            for (size_t j = i + 1; j < config.filaments.size(); ++j) {
                for (size_t k = j + 1; k < config.filaments.size(); ++k) {
                    for (const std::vector<int> &ratio : ternary_ratios) {
                        for (const Backing &backing : backings_or_none(config.ternary_backings)) {
                            SwatchSpec spec;
                            spec.type               = SwatchType::TernaryMix;
                            spec.filaments          = { config.filaments[i], config.filaments[j], config.filaments[k] };
                            spec.ratios             = ratio;
                            spec.backing            = backing;
                            spec.total_thickness_mm = config.ternary_thickness_mm;
                            spec.layer_height_mm    = config.ternary_layer_height_mm;
                            spec.stack_order        = { config.filaments[i].slot, config.filaments[j].slot, config.filaments[k].slot };
                            append_record(plan, std::move(spec), config);
                        }
                    }
                }
            }
        }
    }

    if (config.families.quaternary_mix) {
        const std::vector<std::vector<int>> quaternary_ratios =
            config.quaternary_ratios.empty() ?
                generated_quaternary_layer_ratios(config.quaternary_ratio_layer_limit) :
                deduplicated_layer_ratios(config.quaternary_ratios);
        for (size_t i = 0; i < config.filaments.size(); ++i) {
            for (size_t j = i + 1; j < config.filaments.size(); ++j) {
                for (size_t k = j + 1; k < config.filaments.size(); ++k) {
                    for (size_t l = k + 1; l < config.filaments.size(); ++l) {
                        for (const std::vector<int> &ratio : quaternary_ratios) {
                            for (const Backing &backing : backings_or_none(config.quaternary_backings)) {
                                SwatchSpec spec;
                                spec.type               = SwatchType::QuaternaryMix;
                                spec.filaments          = { config.filaments[i], config.filaments[j], config.filaments[k], config.filaments[l] };
                                spec.ratios             = ratio;
                                spec.backing            = backing;
                                spec.total_thickness_mm = config.quaternary_thickness_mm;
                                spec.layer_height_mm    = config.quaternary_layer_height_mm;
                                spec.stack_order        = { config.filaments[i].slot,
                                                            config.filaments[j].slot,
                                                            config.filaments[k].slot,
                                                            config.filaments[l].slot };
                                append_record(plan, std::move(spec), config);
                            }
                        }
                    }
                }
            }
        }
    }

    if (config.families.layer_line_strip) {
        for (size_t i = 0; i < config.filaments.size(); ++i) {
            for (size_t j = i + 1; j < config.filaments.size(); ++j) {
                const unsigned int a = config.filaments[i].slot;
                const unsigned int b = config.filaments[j].slot;
                for (unsigned int top : { a, b }) {
                    for (const Backing &backing : backings_or_none(config.layer_line_strip_backings)) {
                        SwatchSpec spec;
                        spec.type               = SwatchType::LayerLineStrip;
                        spec.filaments          = { config.filaments[i], config.filaments[j] };
                        spec.backing            = backing;
                        spec.total_thickness_mm = config.layer_line_strip_thickness_mm;
                        spec.layer_height_mm    = config.layer_line_strip_layer_height_mm;
                        spec.top_material_slot  = top;
                        spec.stack_order        = top == a ? std::vector<unsigned int>{ b, a } : std::vector<unsigned int>{ a, b };
                        append_record(plan, std::move(spec), config);
                    }
                }
            }
        }
    }

    plan.swatch_depth_mm = max_record_thickness(plan, config.anchor_thickness_mm);
    assign_layout(plan, config);
    assign_printed_references(plan, config);
    return plan;
}

std::vector<ValidationIssue> validate_swatch_plan(const SwatchPlan &plan, const SwatchGeneratorConfig &config)
{
    std::vector<ValidationIssue> issues;
    std::set<std::string> ids;

    for (const SwatchRecord &record : plan.records) {
        if (record.swatch_id.empty()) {
            issues.push_back({ ValidationSeverity::Error, record.swatch_id, "swatch id is empty" });
        } else if (!ids.insert(record.swatch_id).second) {
            issues.push_back({ ValidationSeverity::Error, record.swatch_id, "duplicate swatch id" });
        }

        if (config.swatch_reference.enabled && record.printed_reference.empty())
            issues.push_back({ ValidationSeverity::Error, record.swatch_id, "printed reference is empty" });

        for (const FilamentSlot &filament : record.spec.filaments) {
            if (!config.filaments.empty() && !slot_exists(filament.slot, config.filaments))
                issues.push_back({ ValidationSeverity::Error, record.swatch_id, "filament slot is not selected" });
        }

        if (record.spec.type == SwatchType::PairMix || record.spec.type == SwatchType::PairOrder) {
            if (record.spec.ratios.size() != 2)
                issues.push_back({ ValidationSeverity::Error, record.swatch_id, "pair swatch requires two layer counts" });
            else if (record.spec.ratios[0] <= 0 || record.spec.ratios[1] <= 0)
                issues.push_back({ ValidationSeverity::Error, record.swatch_id, "pair swatch layer counts must be positive" });
        } else if (record.spec.type == SwatchType::TernaryMix) {
            if (record.spec.ratios.size() != 3)
                issues.push_back({ ValidationSeverity::Error, record.swatch_id, "ternary swatch requires three ratios" });
            else if (std::any_of(record.spec.ratios.begin(), record.spec.ratios.end(), [](int ratio) { return ratio <= 0; }))
                issues.push_back({ ValidationSeverity::Error, record.swatch_id, "ternary swatch ratios must be positive" });
        } else if (record.spec.type == SwatchType::QuaternaryMix) {
            if (record.spec.ratios.size() != 4)
                issues.push_back({ ValidationSeverity::Error, record.swatch_id, "four-color swatch requires four ratios" });
            else if (std::any_of(record.spec.ratios.begin(), record.spec.ratios.end(), [](int ratio) { return ratio <= 0; }))
                issues.push_back({ ValidationSeverity::Error, record.swatch_id, "four-color swatch ratios must be positive" });
        }

        const double footprint_depth = record_bed_footprint_depth(record, config);
        const bool fits = record.position.x_mm + config.layout.chip_width_mm * 0.5 <= config.layout.plate_width_mm + 1e-6 &&
                          record.position.y_mm + footprint_depth * 0.5 <= config.layout.plate_depth_mm + 1e-6;
        if (!fits && !config.layout.multi_plate)
            issues.push_back({ ValidationSeverity::Error, record.swatch_id, "layout exceeds plate bounds" });

        for (const std::string &warning : record.warnings)
            issues.push_back({ ValidationSeverity::Warning, record.swatch_id, warning });
    }

    return issues;
}

nlohmann::json swatch_record_to_json(const SwatchRecord &record)
{
    const SwatchSpec &spec = record.spec;
    const LayerCycleSummary multi_cycle = is_multicomponent_cycle_mix(spec.type) ? layer_cycle_summary(spec) : LayerCycleSummary {};
    const std::vector<int> layer_counts = is_multicomponent_cycle_mix(spec.type) ? multi_cycle.counts : effective_layer_counts(spec);
    const std::vector<unsigned int> layer_sequence =
        is_multicomponent_cycle_mix(spec.type) ? multi_cycle.sequence : repeated_layer_sequence(spec, layer_counts);

    nlohmann::json filaments_json = nlohmann::json::array();
    for (const FilamentSlot &filament : spec.filaments)
        filaments_json.push_back(filament_to_json(filament));

    nlohmann::json j = {
        { "swatch_id", record.swatch_id },
        { "printed_reference", record.printed_reference },
        { "plate_reference", record.plate_reference },
        { "sample_number", record.sample_number },
        { "swatch_type", swatch_type_key(spec.type) },
        { "plate_index", record.position.plate_index },
        { "row", record.position.row },
        { "column", record.position.column },
        { "x_mm", record.position.x_mm },
        { "y_mm", record.position.y_mm },
        { "object_name", record.object_name },
        { "volume_names", record.volume_names },
        { "filaments", filaments_json },
        { "filament_slots", filament_slots(spec) },
        { "filament_names", filament_names(spec) },
        { "short_labels", filament_labels(spec) },
        { "colors", filament_colors(spec) },
        { "td_values", filament_tds_json(spec) },
        { "ratios", spec.ratios },
        { "percentages", ratio_percentages(spec.ratios) },
        { "effective_layer_counts", layer_counts },
        { "effective_layer_sequence", layer_sequence },
        { "effective_layer_cycle", compact_sequence_token(layer_sequence, "_") },
        { "backing", backing_to_json(spec.backing) },
        { "total_thickness_mm", spec.total_thickness_mm },
        { "layer_height_mm", spec.layer_height_mm },
        { "stack_order", spec.stack_order },
        { "top_material_slot", spec.top_material_slot },
        { "top_layer_count", spec.top_layer_count },
        { "measurement_side", spec.measurement_side },
        { "warnings", record.warnings }
    };

    return j;
}

nlohmann::json manifest_json(const SwatchPlan &plan,
                             const std::string &manifest_name,
                             const std::string &csv_name)
{
    nlohmann::json records = nlohmann::json::array();
    for (const SwatchRecord &record : plan.records)
        records.push_back(swatch_record_to_json(record));

    return {
        { "schema", "fullspectrum.calibration_swatches.v1" },
        { "manifest_name", manifest_name },
        { "csv_name", csv_name },
        { "title", plan.title },
        { "swatch_reference_start", plan.swatch_reference_start },
        { "count", plan.records.size() },
        { "primary_filaments", primary_filaments_to_json(plan.primary_filaments) },
        { "primary_colors", primary_colors_to_json(plan.primary_filaments) },
        { "primary_td_values", primary_tds_to_json(plan.primary_filaments) },
        { "layer_height_mm", plan.nominal_layer_height_mm },
        { "swatch_depth_mm", plan.swatch_depth_mm },
        { "local_z_enabled", plan.local_z_enabled },
        { "local_z_direct_multicolor", plan.local_z_direct_multicolor },
        { "actual_layer_time_s", nullptr },
        { "actual_layer_time_by_plate", nlohmann::json::array() },
        { "records", records },
        { "warnings", plan.warnings }
    };
}

std::string manifest_json_string(const SwatchPlan &plan,
                                 const std::string &manifest_name,
                                 const std::string &csv_name)
{
    return manifest_json(plan, manifest_name, csv_name).dump(2);
}

std::string manifest_csv_string(const SwatchPlan &plan)
{
    const std::vector<std::string> headers {
        "swatch_id", "printed_reference", "plate_reference", "sample_number", "swatch_type", "plate_index", "row", "column", "x_mm", "y_mm",
        "object_name", "volume_names", "filament_slots", "filament_names", "short_labels",
        "colors", "td_values", "ratios", "percentages", "effective_layer_counts",
        "effective_layer_sequence", "effective_layer_cycle", "backing", "total_thickness_mm",
        "layer_height_mm", "actual_layer_time_avg_s", "actual_layer_time_min_s",
        "actual_layer_time_max_s", "actual_layer_time_layer_count", "actual_layer_time_skipped_initial_layers",
        "stack_order", "top_material_slot",
        "measurement_side", "warnings"
    };

    std::ostringstream ss;
    ss << join_strings(headers, ",") << '\n';
    for (const SwatchRecord &record : plan.records) {
        const SwatchSpec &spec = record.spec;
        const LayerCycleSummary multi_cycle =
            is_multicomponent_cycle_mix(spec.type) ? layer_cycle_summary(spec) : LayerCycleSummary {};
        const std::vector<int> layer_counts = is_multicomponent_cycle_mix(spec.type) ? multi_cycle.counts : effective_layer_counts(spec);
        const std::vector<unsigned int> layer_sequence =
            is_multicomponent_cycle_mix(spec.type) ? multi_cycle.sequence : repeated_layer_sequence(spec, layer_counts);
        std::vector<std::string> row {
            record.swatch_id,
            record.printed_reference,
            record.plate_reference,
            record.sample_number == 0 ? std::string() : std::to_string(record.sample_number),
            swatch_type_key(spec.type),
            std::to_string(record.position.plate_index),
            std::to_string(record.position.row),
            std::to_string(record.position.column),
            format_decimal_token(record.position.x_mm),
            format_decimal_token(record.position.y_mm),
            record.object_name,
            join_strings(record.volume_names, "|"),
            join_uints_for_csv(filament_slots(spec)),
            join_strings(filament_names(spec), "|"),
            join_strings(filament_labels(spec), "|"),
            join_strings(filament_colors(spec), "|"),
            join_doubles_for_csv(filament_tds(spec)),
            ratio_key(spec.ratios),
            join_doubles_for_csv(ratio_percentages(spec.ratios)),
            ratio_key(layer_counts),
            join_uints_for_csv(layer_sequence),
            compact_sequence_token(layer_sequence, "_"),
            backing_label(spec.backing),
            format_decimal_token(spec.total_thickness_mm),
            format_decimal_token(spec.layer_height_mm),
            {},
            {},
            {},
            {},
            {},
            join_uints_for_csv(spec.stack_order),
            spec.top_material_slot == 0 ? std::string() : std::to_string(spec.top_material_slot),
            spec.measurement_side,
            join_strings(record.warnings, "|")
        };

        for (size_t i = 0; i < row.size(); ++i) {
            if (i != 0)
                ss << ',';
            ss << csv_escape(row[i]);
        }
        ss << '\n';
    }

    return ss.str();
}

} // namespace ColorCalibrationSwatches
} // namespace Slic3r
