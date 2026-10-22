#include "AIPrintAssistant.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include "Preset.hpp"

namespace Slic3r::AIPrintAssistant {
namespace {
using json = nlohmann::json;

bool public_setting(const std::string& key, const ConfigOptionDef& def)
{
    // Connection credentials and preset bookkeeping are not slicing settings.
    if (def.readonly || (def.label.empty() && def.full_label.empty()) || def.printer_technology == ptSLA || key == "printer_technology")
        return false;
    for (const char* token : {"printhost", "print_host", "password", "apikey", "api_key", "access_token", "serial_port", "inherits",
                              "compatible_printers", "compatible_prints", "settings_id", "preset_id"})
        if (key.find(token) != std::string::npos)
            return false;
    return true;
}

const ConfigOptionDef& definition(const std::string& key)
{
    const auto* def = print_config_def.get(key);
    if (!def || !public_setting(key, *def))
        throw std::runtime_error("This is not an editable print setting: " + key);
    return *def;
}

json enum_value(const ConfigOptionDef& def, int value)
{
    if (def.enum_keys_map)
        for (const auto& item : *def.enum_keys_map)
            if (item.second == value)
                return item.first;
    return value;
}

double number(const json& value, const ConfigOptionDef& def)
{
    if (!value.is_number())
        throw std::runtime_error("Expected a JSON number for " + def.opt_key);
    const auto result = value.get<double>();
    if (!std::isfinite(result) || result < def.min || result > def.max)
        throw std::runtime_error("Value outside the setting's allowed range: " + def.opt_key);
    return result;
}

int integer(const json& value, const ConfigOptionDef& def)
{
    if (!value.is_number_integer())
        throw std::runtime_error("Expected an integer for " + def.opt_key);
    const double result = number(value, def);
    if (result < std::numeric_limits<int>::min() || result > std::numeric_limits<int>::max())
        throw std::runtime_error("Integer outside the supported range: " + def.opt_key);
    return static_cast<int>(result);
}

int enumeration(const json& value, const ConfigOptionDef& def)
{
    if (!def.enum_keys_map)
        return integer(value, def);
    if (value.is_string()) {
        const auto it = def.enum_keys_map->find(value.get<std::string>());
        if (it != def.enum_keys_map->end() && (def.enum_values.empty() || def.has_enum_value(it->first)))
            return it->second;
    }
    throw std::runtime_error("Unknown enum value for " + def.opt_key);
}

bool boolean(const json& value)
{
    if (!value.is_boolean())
        throw std::runtime_error("Boolean settings require true or false.");
    return value.get<bool>();
}

std::string string_value(const json& value)
{
    if (!value.is_string())
        throw std::runtime_error("Text settings require a JSON string.");
    const auto result = value.get<std::string>();
    if (result.size() > 32768 || result.find('\0') != std::string::npos)
        throw std::runtime_error("Setting text is too long or contains a null character.");
    return result;
}

FloatOrPercent float_or_percent(const json& value, const ConfigOptionDef& def)
{
    if (value.is_number())
        return {number(value, def), false};
    if (!value.is_string())
        throw std::runtime_error("Use a number for an absolute value or a string such as 25% for a percentage.");
    auto text = string_value(value);
    if (text.size() < 2 || text.back() != '%')
        throw std::runtime_error("Expected a percentage ending in %.");
    text.pop_back();
    // Strict JSON numeric parsing rejects suffixes and locale-dependent input.
    return {number(parse_json(text), def), true};
}

json percent_value(double value, bool percent) { return percent ? json(json(value).dump() + "%") : json(value); }

void require_array(const json& value)
{
    if (!value.is_array() || value.size() > 4096)
        throw std::runtime_error("Expected a setting array of at most 4096 elements.");
}

template<class T, class Convert> std::vector<T> vector_value(const json& value, bool nullable, T nil, Convert convert)
{
    require_array(value);
    std::vector<T> result;
    for (const auto& item : value) {
        if (item.is_null() && nullable)
            result.push_back(nil);
        else
            result.push_back(convert(item));
    }
    return result;
}
} // namespace

const char* scope_name(Scope scope)
{
    switch (scope) {
    case Scope::Object: return "object";
    case Scope::Process: return "process";
    case Scope::Filament: return "filament";
    case Scope::Printer: return "printer";
    }
    throw std::runtime_error("Unknown settings scope.");
}

bool setting_in_scope(const std::string& key, Scope scope)
{
    const auto* def = print_config_def.get(key);
    if (!def || !public_setting(key, *def))
        return false;
    if (scope == Scope::Object)
        return PrintObjectConfig::defaults().has(key) || PrintRegionConfig::defaults().has(key);
    const auto& keys = scope == Scope::Process  ? Preset::print_options() :
                       scope == Scope::Filament ? Preset::filament_options() :
                                                  Preset::printer_options();
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

json setting_value(const ConfigBase& config, const std::string& key)
{
    const auto& def    = definition(key);
    const auto* option = config.option(key);
    if (!option)
        throw std::runtime_error("Missing setting: " + key);
    json value;
    switch (option->type()) {
    case coFloat:
    case coPercent: value = option->getFloat(); break;
    case coInt: value = option->getInt(); break;
    case coEnum: value = enum_value(def, option->getInt()); break;
    case coBool: value = option->getBool(); break;
    case coString: value = dynamic_cast<const ConfigOptionString&>(*option).value; break;
    case coFloatOrPercent: {
        const auto& v = dynamic_cast<const ConfigOptionFloatOrPercent&>(*option);
        value         = percent_value(v.value, v.percent);
        break;
    }
    case coFloats:
    case coPercents: value = dynamic_cast<const ConfigOptionVector<double>&>(*option).values; break;
    case coInts: value = dynamic_cast<const ConfigOptionVector<int>&>(*option).values; break;
    case coStrings: value = dynamic_cast<const ConfigOptionVector<std::string>&>(*option).values; break;
    case coEnums:
        value = json::array();
        for (const auto v : dynamic_cast<const ConfigOptionVector<int>&>(*option).values)
            value.push_back(enum_value(def, v));
        break;
    case coBools:
        value = json::array();
        for (const auto v : dynamic_cast<const ConfigOptionVector<unsigned char>&>(*option).values)
            value.push_back(bool(v));
        break;
    case coFloatsOrPercents:
        value = json::array();
        for (const auto& v : dynamic_cast<const ConfigOptionVector<FloatOrPercent>&>(*option).values)
            value.push_back(percent_value(v.value, v.percent));
        break;
    case coPoint: {
        const auto& p = dynamic_cast<const ConfigOptionPoint&>(*option).value;
        value         = {p.x(), p.y()};
        break;
    }
    case coPoint3: {
        const auto& p = dynamic_cast<const ConfigOptionPoint3&>(*option).value;
        value         = {p.x(), p.y(), p.z()};
        break;
    }
    case coPoints:
        value = json::array();
        for (const auto& p : dynamic_cast<const ConfigOptionPoints&>(*option).values)
            value.push_back({p.x(), p.y()});
        break;
    default: throw std::runtime_error("Unsupported configuration type: " + key);
    }
    if (option->nullable() && !option->is_scalar()) {
        const auto& vector = dynamic_cast<const ConfigOptionVectorBase&>(*option);
        for (size_t i = 0; i < vector.size(); ++i)
            if (vector.is_nil(i))
                value[i] = nullptr;
    }
    return value;
}

void assign_setting(DynamicPrintConfig& config, const std::string& key, const json& value)
{
    const auto&        def = definition(key);
    DynamicPrintConfig holder;
    auto*              option = holder.option(key, true);
    if (!option)
        throw std::runtime_error("Could not create setting: " + key);
    auto num       = [&](const json& v) { return number(v, def); };
    auto int_value = [&](const json& v) { return integer(v, def); };
    auto enum_item = [&](const json& v) { return enumeration(v, def); };
    auto fop       = [&](const json& v) { return float_or_percent(v, def); };
    auto point     = [&](const json& v) {
        if (!v.is_array() || v.size() != 2)
            throw std::runtime_error("Points require [x, y].");
        return Vec2d(num(v[0]), num(v[1]));
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    switch (def.type) {
    case coFloat:
    case coPercent: dynamic_cast<ConfigOptionFloat&>(*option).value = num(value); break;
    case coInt: option->setInt(int_value(value)); break;
    case coEnum: option->setInt(enum_item(value)); break;
    case coBool: dynamic_cast<ConfigOptionBool&>(*option).value = boolean(value); break;
    case coString: dynamic_cast<ConfigOptionString&>(*option).value = string_value(value); break;
    case coFloatOrPercent: {
        const auto v    = fop(value);
        auto&      dest = dynamic_cast<ConfigOptionFloatOrPercent&>(*option);
        dest.value      = v.value;
        dest.percent    = v.percent;
        break;
    }
    case coFloats:
    case coPercents: dynamic_cast<ConfigOptionVector<double>&>(*option).values = vector_value<double>(value, def.nullable, nan, num); break;
    case coInts:
        dynamic_cast<ConfigOptionVector<int>&>(*option).values = vector_value<int>(value, def.nullable,
                                                                                   ConfigOptionIntsNullable::nil_value(), int_value);
        break;
    case coEnums:
        dynamic_cast<ConfigOptionVector<int>&>(*option).values = vector_value<int>(value, def.nullable,
                                                                                   ConfigOptionIntsNullable::nil_value(), enum_item);
        break;
    case coBools:
        dynamic_cast<ConfigOptionVector<unsigned char>&>(*option).values =
            vector_value<unsigned char>(value, def.nullable, ConfigOptionBoolsNullable::nil_value(), boolean);
        break;
    case coStrings:
        dynamic_cast<ConfigOptionVector<std::string>&>(*option).values = vector_value<std::string>(value, false, {}, string_value);
        break;
    case coFloatsOrPercents:
        dynamic_cast<ConfigOptionVector<FloatOrPercent>&>(*option).values = vector_value<FloatOrPercent>(value, def.nullable, {nan, false},
                                                                                                         fop);
        break;
    case coPoint: dynamic_cast<ConfigOptionPoint&>(*option).value = point(value); break;
    case coPoints: dynamic_cast<ConfigOptionPoints&>(*option).values = vector_value<Vec2d>(value, false, {}, point); break;
    case coPoint3:
        if (!value.is_array() || value.size() != 3)
            throw std::runtime_error("3D points require [x, y, z].");
        dynamic_cast<ConfigOptionPoint3&>(*option).value = Vec3d(num(value[0]), num(value[1]), num(value[2]));
        break;
    default: throw std::runtime_error("Unsupported setting type: " + key);
    }
    config.set_key_value(key, option->clone());
}
} // namespace Slic3r::AIPrintAssistant
