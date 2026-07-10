#include "FullSpectrumKSPairResidual.hpp"
#include "FullSpectrumLabTDRidgeModel.h"
#include "FullSpectrumMaterialDatabaseProfile.h"
#include "filament_mixer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

namespace Slic3r {

namespace {

namespace MaterialDatabaseData = FullSpectrumMaterialDatabaseProfileData;
namespace LabTDRidgeData = FullSpectrumLabTDRidgeModelData;

using Spectrum = std::array<double, MaterialDatabaseData::SPECTRUM_SIZE>;

constexpr double EPSILON = 1e-9;
constexpr double DISPLAY_D65_10_X = 94.811;
constexpr double DISPLAY_D65_10_Y = 100.0;
constexpr double DISPLAY_D65_10_Z = 107.304;

struct Lab
{
    double L = 0.0;
    double a = 0.0;
    double b = 0.0;
};

struct CieObserverSample
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct LinearRgb
{
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

struct Oklab
{
    double L = 0.0;
    double a = 0.0;
    double b = 0.0;
};

struct MaterialKS
{
    Spectrum              ks {};
    std::optional<size_t> material_index;
    std::string           normalized_hex;
    LinearRgb             source_rgb {};
    double                weight = 0.0;
};

enum class PairResidualMode
{
    None,
    ExactProfilePairs,
    ExtrapolateAllInputPairs
};

static double clamp01(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

static std::optional<std::string> normalize_hex_color(const std::string &hex)
{
    if (hex.size() < 7 || hex[0] != '#')
        return std::nullopt;

    std::string normalized = "#";
    normalized.reserve(7);
    for (size_t i = 1; i < 7; ++i) {
        const unsigned char ch = static_cast<unsigned char>(hex[i]);
        if (!std::isxdigit(ch))
            return std::nullopt;
        normalized.push_back(static_cast<char>(std::toupper(ch)));
    }
    return normalized;
}

static std::optional<size_t> material_index_for_color(const std::string &hex)
{
    const std::optional<std::string> normalized = normalize_hex_color(hex);
    if (!normalized)
        return std::nullopt;

    for (size_t material = 0; material < MaterialDatabaseData::MATERIAL_COUNT; ++material) {
        const size_t hex_count = MaterialDatabaseData::MATERIAL_HEX_COUNT[material];
        for (size_t hex_index = 0; hex_index < hex_count; ++hex_index) {
            if (*normalized == MaterialDatabaseData::MATERIAL_HEX[material][hex_index])
                return material;
        }
    }
    return std::nullopt;
}

static const Spectrum& material_ks(size_t material_index)
{
    return MaterialDatabaseData::MATERIAL_KS[material_index];
}

static double material_td_mm(size_t material_index)
{
    return MaterialDatabaseData::MATERIAL_TD_MM[material_index];
}

static double srgb_to_linear(unsigned char value)
{
    const double srgb = double(value) / 255.0;
    return srgb <= 0.04045 ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
}

static std::optional<LinearRgb> linear_rgb_from_hex(const std::string &hex)
{
    const std::optional<std::string> normalized = normalize_hex_color(hex);
    if (!normalized)
        return std::nullopt;

    const auto decode = [](char hi, char lo) -> unsigned char {
        const auto nibble = [](char ch) -> int {
            if (ch >= '0' && ch <= '9')
                return ch - '0';
            return 10 + ch - 'A';
        };
        return static_cast<unsigned char>((nibble(hi) << 4) | nibble(lo));
    };

    return LinearRgb {
        srgb_to_linear(decode((*normalized)[1], (*normalized)[2])),
        srgb_to_linear(decode((*normalized)[3], (*normalized)[4])),
        srgb_to_linear(decode((*normalized)[5], (*normalized)[6]))
    };
}

static double linear_rgb_distance_squared(const LinearRgb &a, const LinearRgb &b)
{
    const double dr = a.r - b.r;
    const double dg = a.g - b.g;
    const double db = a.b - b.b;
    return dr * dr + dg * dg + db * db;
}

static double material_color_distance_squared(const LinearRgb &rgb, size_t material_index)
{
    double best = std::numeric_limits<double>::max();
    const size_t hex_count = MaterialDatabaseData::MATERIAL_HEX_COUNT[material_index];
    for (size_t hex_index = 0; hex_index < hex_count; ++hex_index) {
        const char *hex = MaterialDatabaseData::MATERIAL_HEX[material_index][hex_index];
        if (hex == nullptr || hex[0] == '\0')
            continue;
        const std::optional<LinearRgb> material_rgb = linear_rgb_from_hex(hex);
        if (material_rgb)
            best = std::min(best, linear_rgb_distance_squared(rgb, *material_rgb));
    }
    return best;
}

static double gaussian(double wavelength_nm, double center_nm, double sigma_nm)
{
    const double x = (wavelength_nm - center_nm) / sigma_nm;
    return std::exp(-0.5 * x * x);
}

static double ks_from_reflectance(double reflectance)
{
    const double r = std::clamp(reflectance, 0.001, 0.999);
    return ((1.0 - r) * (1.0 - r)) / (2.0 * r);
}

static Spectrum estimated_ks_from_hex(const std::string &hex)
{
    const std::optional<LinearRgb> rgb = linear_rgb_from_hex(hex);
    Spectrum ks {};
    if (!rgb) {
        ks.fill(ks_from_reflectance(0.02));
        return ks;
    }

    const double neutral = std::min({rgb->r, rgb->g, rgb->b});
    const double red     = rgb->r - neutral;
    const double green   = rgb->g - neutral;
    const double blue    = rgb->b - neutral;
    const double average = (rgb->r + rgb->g + rgb->b) / 3.0;
    const double floor   = 0.015 + 0.035 * average;

    for (size_t i = 0; i < ks.size(); ++i) {
        const double wavelength = double(MaterialDatabaseData::WAVELENGTH_NM[i]);
        const double red_basis = std::max(gaussian(wavelength, 610.0, 58.0),
                                          0.58 * gaussian(wavelength, 680.0, 48.0));
        const double green_basis = gaussian(wavelength, 540.0, 48.0);
        const double blue_basis  = gaussian(wavelength, 455.0, 42.0);
        const double reflectance = std::clamp(floor + 0.90 * (neutral + red * red_basis + green * green_basis + blue * blue_basis),
                                              0.003, 0.985);
        ks[i] = ks_from_reflectance(reflectance);
    }

    return ks;
}

static std::optional<std::vector<MaterialKS>> materials_from_colors(
    const std::vector<FullSpectrumKSPairResidualColorInput> &color_percents)
{
    std::vector<MaterialKS> materials;
    materials.reserve(color_percents.size());
    double total = 0.0;

    for (const FullSpectrumKSPairResidualColorInput &input : color_percents) {
        const int pct = input.percent;
        if (pct <= 0)
            continue;

        const std::optional<std::string> normalized_hex = normalize_hex_color(input.color_hex);
        if (!normalized_hex)
            return std::nullopt;
        const std::optional<LinearRgb> source_rgb = linear_rgb_from_hex(*normalized_hex);
        if (!source_rgb)
            return std::nullopt;

        double optical_strength = 1.0;
        if (input.use_td && input.td_mm && std::isfinite(*input.td_mm) && *input.td_mm > EPSILON) {
            // Treat TD as an optical strength term: lower TD means the filament
            // reaches visual opacity faster and should contribute more strongly.
            optical_strength = 1.0 / *input.td_mm;
        }

        const double weighted = static_cast<double>(pct) * optical_strength;
        const std::optional<size_t> material_index = material_index_for_color(input.color_hex);
        MaterialKS material;
        material.material_index = material_index;
        material.normalized_hex = *normalized_hex;
        material.source_rgb     = *source_rgb;
        material.weight         = weighted;
        if (material_index) {
            material.ks = material_ks(*material_index);
        } else {
            material.ks = estimated_ks_from_hex(input.color_hex);
        }
        materials.emplace_back(std::move(material));
        total += weighted;
    }

    if (total <= EPSILON)
        return std::nullopt;

    for (MaterialKS &material : materials)
        material.weight /= total;

    if (materials.size() < 2)
        return std::nullopt;

    return materials;
}

static double reflectance_from_ks(double ks)
{
    const double f = std::max(0.0, ks);
    return clamp01(1.0 + f - std::sqrt(f * f + 2.0 * f));
}

static bool apply_pair_residual_coefficients(const FullSpectrumMaterialDatabaseProfileData::PairResidualCoefficients &pair,
                                             double pa,
                                             double pb,
                                             Spectrum &ks)
{
    if (pa <= EPSILON || pb <= EPSILON)
        return false;

    const double d = (pa - pb) / (pa + pb);
    const double product = pa * pb;
    for (size_t wave = 0; wave < ks.size(); ++wave)
        ks[wave] += product * (pair.b0[wave] + pair.b1[wave] * d + pair.b2[wave] * d * d);
    return true;
}

template <class PairResiduals>
static bool apply_exact_profile_pair_residuals(const PairResiduals &pair_residuals,
                                               const std::array<double, MaterialDatabaseData::MATERIAL_COUNT> &composition,
                                               Spectrum &ks)
{
    bool applied = false;
    for (const auto &pair : pair_residuals) {
        const double pa = composition[pair.material_a];
        const double pb = composition[pair.material_b];
        if (!apply_pair_residual_coefficients(pair, pa, pb, ks))
            continue;
        applied = true;
    }
    return applied;
}

struct PairResidualSelection
{
    const FullSpectrumMaterialDatabaseProfileData::PairResidualCoefficients *pair = nullptr;
    bool reversed = false;
};

static std::optional<PairResidualSelection> nearest_learned_pair_residual(const MaterialKS &a, const MaterialKS &b)
{
    double best_distance = std::numeric_limits<double>::max();
    PairResidualSelection best;

    for (const auto &pair : MaterialDatabaseData::PAIR_RESIDUALS) {
        const double forward = material_color_distance_squared(a.source_rgb, pair.material_a) +
                               material_color_distance_squared(b.source_rgb, pair.material_b);
        if (forward < best_distance) {
            best_distance = forward;
            best = {&pair, false};
        }

        const double reverse = material_color_distance_squared(a.source_rgb, pair.material_b) +
                               material_color_distance_squared(b.source_rgb, pair.material_a);
        if (reverse < best_distance) {
            best_distance = reverse;
            best = {&pair, true};
        }
    }

    if (best.pair == nullptr)
        return std::nullopt;
    return best;
}

static bool apply_extrapolated_pair_residuals(const std::vector<MaterialKS> &materials, Spectrum &ks)
{
    bool applied = false;

    for (size_t i = 0; i < materials.size(); ++i) {
        const MaterialKS &a = materials[i];
        if (a.weight <= EPSILON)
            continue;

        for (size_t j = i + 1; j < materials.size(); ++j) {
            const MaterialKS &b = materials[j];
            if (b.weight <= EPSILON)
                continue;
            if (a.normalized_hex == b.normalized_hex)
                continue;
            if (a.material_index && b.material_index && *a.material_index == *b.material_index)
                continue;

            const std::optional<PairResidualSelection> selection = nearest_learned_pair_residual(a, b);
            if (!selection)
                continue;

            const double pa = selection->reversed ? b.weight : a.weight;
            const double pb = selection->reversed ? a.weight : b.weight;
            if (apply_pair_residual_coefficients(*selection->pair, pa, pb, ks))
                applied = true;
        }
    }

    return applied;
}

static Spectrum predict_reflectance_spectrum(const std::vector<MaterialKS> &materials,
                                             PairResidualMode               residual_mode = PairResidualMode::ExactProfilePairs,
                                             bool                          *applied_residuals = nullptr)
{
    Spectrum ks {};
    std::array<double, MaterialDatabaseData::MATERIAL_COUNT> material_composition {};

    for (const MaterialKS &material : materials) {
        if (material.weight <= EPSILON)
            continue;
        if (material.material_index)
            material_composition[*material.material_index] += material.weight;
        for (size_t wave = 0; wave < ks.size(); ++wave)
            ks[wave] += material.weight * material.ks[wave];
    }

    bool residuals_applied = false;
    if (residual_mode == PairResidualMode::ExactProfilePairs)
        residuals_applied = apply_exact_profile_pair_residuals(MaterialDatabaseData::PAIR_RESIDUALS, material_composition, ks);
    else if (residual_mode == PairResidualMode::ExtrapolateAllInputPairs)
        residuals_applied = apply_extrapolated_pair_residuals(materials, ks);
    if (applied_residuals)
        *applied_residuals = residuals_applied;

    Spectrum reflectance {};
    for (size_t wave = 0; wave < reflectance.size(); ++wave)
        reflectance[wave] = reflectance_from_ks(ks[wave]);

    return reflectance;
}

static const std::array<double, MaterialDatabaseData::SPECTRUM_SIZE>& cie_d65_400_700_10nm()
{
    static const std::array<double, MaterialDatabaseData::SPECTRUM_SIZE> values = {
        82.7549, 91.4860, 93.4318, 86.6823, 104.8650, 117.0080, 117.8120, 114.8610,
        115.9230, 108.8110, 109.3540, 107.8020, 104.7900, 107.6890, 104.4050, 104.0460,
        100.0000, 96.3342, 95.7880, 88.6856, 90.0062, 89.5991, 87.6987, 83.2886,
        83.6992, 80.0268, 80.2146, 82.2778, 78.2842, 69.7213, 71.6091
    };
    return values;
}

static const std::array<CieObserverSample, MaterialDatabaseData::SPECTRUM_SIZE>& cie_observer_10deg_400_700_10nm()
{
    static const std::array<CieObserverSample, MaterialDatabaseData::SPECTRUM_SIZE> values = {{
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

static Lab lab_from_xyz(double x, double y, double z, double white_x, double white_y, double white_z)
{
    const double fx = lab_pivot_xyz(x / white_x);
    const double fy = lab_pivot_xyz(y / white_y);
    const double fz = lab_pivot_xyz(z / white_z);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

static Lab lab_from_reflectance_spectrum(
    const std::array<double, MaterialDatabaseData::SPECTRUM_SIZE> &spectrum)
{
    const auto &d65 = cie_d65_400_700_10nm();
    const auto &cmf = cie_observer_10deg_400_700_10nm();

    double y_weight = 0.0;
    double xn_weight = 0.0;
    double zn_weight = 0.0;
    for (size_t i = 0; i < spectrum.size(); ++i) {
        y_weight += d65[i] * cmf[i].y;
        xn_weight += d65[i] * cmf[i].x;
        zn_weight += d65[i] * cmf[i].z;
    }

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

    return lab_from_xyz(x, y, z, white_x, white_y, white_z);
}

static std::optional<Lab> lab_from_hex_color(const std::string &hex)
{
    const std::optional<LinearRgb> rgb = linear_rgb_from_hex(hex);
    if (!rgb)
        return std::nullopt;

    const double x = 100.0 * (0.4124564 * rgb->r + 0.3575761 * rgb->g + 0.1804375 * rgb->b);
    const double y = 100.0 * (0.2126729 * rgb->r + 0.7151522 * rgb->g + 0.0721750 * rgb->b);
    const double z = 100.0 * (0.0193339 * rgb->r + 0.1191920 * rgb->g + 0.9503041 * rgb->b);

    return lab_from_xyz(x, y, z, DISPLAY_D65_10_X, DISPLAY_D65_10_Y, DISPLAY_D65_10_Z);
}

static Lab lab_from_linear_rgb(const LinearRgb &rgb)
{
    const double x = 100.0 * (0.4124564 * rgb.r + 0.3575761 * rgb.g + 0.1804375 * rgb.b);
    const double y = 100.0 * (0.2126729 * rgb.r + 0.7151522 * rgb.g + 0.0721750 * rgb.b);
    const double z = 100.0 * (0.0193339 * rgb.r + 0.1191920 * rgb.g + 0.9503041 * rgb.b);

    return lab_from_xyz(x, y, z, DISPLAY_D65_10_X, DISPLAY_D65_10_Y, DISPLAY_D65_10_Z);
}

static Oklab oklab_from_linear_rgb(const LinearRgb &rgb)
{
    const double l = 0.4122214708 * rgb.r + 0.5363325363 * rgb.g + 0.0514459929 * rgb.b;
    const double m = 0.2119034982 * rgb.r + 0.6806995451 * rgb.g + 0.1073969566 * rgb.b;
    const double s = 0.0883024619 * rgb.r + 0.2817188376 * rgb.g + 0.6299787005 * rgb.b;

    const double l_ = std::cbrt(l);
    const double m_ = std::cbrt(m);
    const double s_ = std::cbrt(s);

    return {
        0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
        1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
        0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_
    };
}

static std::optional<Oklab> oklab_from_hex_color(const std::string &hex)
{
    const std::optional<LinearRgb> rgb = linear_rgb_from_hex(hex);
    if (!rgb)
        return std::nullopt;
    return oklab_from_linear_rgb(*rgb);
}

static LinearRgb linear_rgb_from_oklab(const Oklab &oklab)
{
    const double l_ = oklab.L + 0.3963377774 * oklab.a + 0.2158037573 * oklab.b;
    const double m_ = oklab.L - 0.1055613458 * oklab.a - 0.0638541728 * oklab.b;
    const double s_ = oklab.L - 0.0894841775 * oklab.a - 1.2914855480 * oklab.b;

    const double l = l_ * l_ * l_;
    const double m = m_ * m_ * m_;
    const double s = s_ * s_ * s_;

    return {
        4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s
    };
}

static Lab lab_from_oklab(const Oklab &oklab)
{
    return lab_from_linear_rgb(linear_rgb_from_oklab(oklab));
}

static double pivot_lab_to_xyz(double value)
{
    const double cubed = value * value * value;
    return cubed > 0.008856 ? cubed : (value - 16.0 / 116.0) / 7.787;
}

static double linear_to_srgb(double value)
{
    return value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

static std::string lab_to_hex(const Lab &lab)
{
    const double fy = (lab.L + 16.0) / 116.0;
    const double fx = lab.a / 500.0 + fy;
    const double fz = fy - lab.b / 200.0;

    const double x = DISPLAY_D65_10_X * pivot_lab_to_xyz(fx) / 100.0;
    const double y = DISPLAY_D65_10_Y * pivot_lab_to_xyz(fy) / 100.0;
    const double z = DISPLAY_D65_10_Z * pivot_lab_to_xyz(fz) / 100.0;

    const double lr = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
    const double lg = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
    const double lb = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;

    const int r = std::clamp(static_cast<int>(std::round(clamp01(linear_to_srgb(lr)) * 255.0)), 0, 255);
    const int g = std::clamp(static_cast<int>(std::round(clamp01(linear_to_srgb(lg)) * 255.0)), 0, 255);
    const int b = std::clamp(static_cast<int>(std::round(clamp01(linear_to_srgb(lb)) * 255.0)), 0, 255);

    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
    return std::string(buf);
}

struct CanonicalFilamentMixerEntry
{
    std::string hex;
    int         percent = 0;
    double      td_sort = 0.0;
};

static unsigned char hex_byte(char hi, char lo)
{
    const auto nibble = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : 10 + ch - 'A'; };
    return static_cast<unsigned char>((nibble(hi) << 4) | nibble(lo));
}

static std::optional<std::string> lab_td_canonical_filament_mixer_hex(const std::vector<FullSpectrumKSPairResidualColorInput>& color_percents)
{
    std::vector<CanonicalFilamentMixerEntry> entries;
    entries.reserve(color_percents.size());
    for (const FullSpectrumKSPairResidualColorInput& input : color_percents) {
        if (input.percent <= 0)
            continue;
        const std::optional<std::string> hex = normalize_hex_color(input.color_hex);
        if (!hex)
            return std::nullopt;
        const double td_sort = input.td_mm && std::isfinite(*input.td_mm) ? *input.td_mm : 0.0;
        entries.push_back({*hex, input.percent, td_sort});
    }
    if (entries.empty())
        return std::nullopt;

    std::sort(entries.begin(), entries.end(), [](const CanonicalFilamentMixerEntry& left, const CanonicalFilamentMixerEntry& right) {
        if (left.hex != right.hex)
            return left.hex < right.hex;
        if (left.td_sort != right.td_sort)
            return left.td_sort < right.td_sort;
        return left.percent < right.percent;
    });

    unsigned char r           = hex_byte(entries.front().hex[1], entries.front().hex[2]);
    unsigned char g           = hex_byte(entries.front().hex[3], entries.front().hex[4]);
    unsigned char b           = hex_byte(entries.front().hex[5], entries.front().hex[6]);
    int           accumulated = entries.front().percent;
    for (size_t index = 1; index < entries.size(); ++index) {
        const CanonicalFilamentMixerEntry& next  = entries[index];
        const int                          total = accumulated + next.percent;
        if (total <= 0)
            continue;
        filament_mixer_lerp(r, g, b, hex_byte(next.hex[1], next.hex[2]), hex_byte(next.hex[3], next.hex[4]),
                            hex_byte(next.hex[5], next.hex[6]), static_cast<float>(next.percent) / static_cast<float>(total), &r, &g, &b);
        accumulated = total;
    }

    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
    return std::string(buf);
}

static std::optional<std::string> blend_from_colors(const std::vector<FullSpectrumKSPairResidualColorInput>& color_percents)
{
    const auto materials = materials_from_colors(color_percents);
    if (!materials)
        return std::nullopt;

    const auto spectrum = predict_reflectance_spectrum(*materials);
    return lab_to_hex(lab_from_reflectance_spectrum(spectrum));
}

static std::optional<std::string> apply_pair_residual_delta_lab(const std::string&                                       base_color_hex,
                                                                const std::vector<FullSpectrumKSPairResidualColorInput>& color_percents)
{
    const auto base_lab = lab_from_hex_color(base_color_hex);
    if (!base_lab)
        return std::nullopt;

    const auto materials = materials_from_colors(color_percents);
    if (!materials)
        return std::nullopt;

    bool residuals_applied = false;
    const auto corrected_spectrum = predict_reflectance_spectrum(*materials, PairResidualMode::ExtrapolateAllInputPairs, &residuals_applied);
    if (!residuals_applied)
        return std::nullopt;

    const auto plain_spectrum = predict_reflectance_spectrum(*materials, PairResidualMode::None);
    const Lab  plain_lab      = lab_from_reflectance_spectrum(plain_spectrum);
    const Lab  corrected_lab  = lab_from_reflectance_spectrum(corrected_spectrum);

    return lab_to_hex({
        base_lab->L + corrected_lab.L - plain_lab.L,
        base_lab->a + corrected_lab.a - plain_lab.a,
        base_lab->b + corrected_lab.b - plain_lab.b
    });
}

struct LabTDRidgeMaterial
{
    Lab    lab {};
    Oklab  oklab {};
    double td_mm = 0.0;
    double fraction = 0.0;
};

struct KnownMaterialMatch
{
    Lab    lab {};
    double td_mm = 0.0;
};

static std::optional<KnownMaterialMatch> lab_td_known_material_match(
    const std::string &normalized_hex,
    const std::optional<double> &td_mm)
{
    const LabTDRidgeData::KnownMaterialLab *same_hex = nullptr;
    size_t same_hex_count = 0;

    for (const auto &material : LabTDRidgeData::KNOWN_MATERIALS) {
        if (normalized_hex != material.hex)
            continue;

        ++same_hex_count;
        same_hex = &material;
        if (td_mm && std::isfinite(*td_mm) && *td_mm > EPSILON && std::abs(*td_mm - material.td_mm) <= 0.15) {
            return KnownMaterialMatch {
                {material.lab[0], material.lab[1], material.lab[2]},
                material.td_mm
            };
        }
    }

    if (same_hex_count == 1 && same_hex != nullptr) {
        return KnownMaterialMatch{{same_hex->lab[0], same_hex->lab[1], same_hex->lab[2]}, same_hex->td_mm};
    }

    return std::nullopt;
}

static std::optional<std::vector<LabTDRidgeMaterial>> lab_td_materials_from_colors(
    const std::vector<FullSpectrumKSPairResidualColorInput>& color_percents,
    bool&                                                    used_catalog_hex_lab,
    bool&                                                    defaulted_td,
    bool&                                                    td_disabled)
{
    std::vector<LabTDRidgeMaterial> materials;
    materials.reserve(color_percents.size());
    double total         = 0.0;
    used_catalog_hex_lab = false;
    defaulted_td         = false;
    td_disabled          = false;

    for (const FullSpectrumKSPairResidualColorInput& input : color_percents) {
        if (input.percent <= 0)
            continue;

        const std::optional<std::string> normalized_hex = normalize_hex_color(input.color_hex);
        if (!normalized_hex)
            return std::nullopt;

        const std::optional<Oklab> oklab = oklab_from_hex_color(*normalized_hex);
        if (!oklab)
            return std::nullopt;

        const std::optional<KnownMaterialMatch> known = lab_td_known_material_match(*normalized_hex, input.td_mm);
        Lab                                     lab{};
        double                                  td = 6.0;

        if (known) {
            lab = known->lab;
        } else {
            const std::optional<Lab> catalog_lab = lab_from_hex_color(*normalized_hex);
            if (!catalog_lab)
                return std::nullopt;
            lab                  = *catalog_lab;
            used_catalog_hex_lab = true;
        }

        if (!input.use_td) {
            td          = 6.0;
            td_disabled = true;
        } else if (input.td_mm && std::isfinite(*input.td_mm) && *input.td_mm > EPSILON) {
            td = *input.td_mm;
        } else if (known) {
            td = known->td_mm;
        } else {
            defaulted_td = true;
        }

        materials.push_back({lab, *oklab, td, double(input.percent)});
        total += double(input.percent);
    }

    if (materials.size() < 2 || total <= EPSILON)
        return std::nullopt;

    for (LabTDRidgeMaterial& material : materials)
        material.fraction /= total;

    return materials;
}

static double lab_td_layer_height_mm(const std::vector<FullSpectrumKSPairResidualColorInput>& color_percents)
{
    for (const FullSpectrumKSPairResidualColorInput& input : color_percents) {
        if (input.layer_height_mm && std::isfinite(*input.layer_height_mm) && *input.layer_height_mm > EPSILON)
            return *input.layer_height_mm;
    }
    return 0.08;
}

static double lab_chroma(const Lab& lab) { return std::hypot(lab.a, lab.b); }

static double lab_hue(const Lab &lab)
{
    return std::atan2(lab.b, lab.a);
}

static double lab_hue_distance(double hue_a, double hue_b)
{
    constexpr double PI = 3.1415926535897932384626433832795;
    const double delta = std::abs(hue_a - hue_b);
    return std::min(delta, 2.0 * PI - delta) / PI;
}

static double lab_td_opacity_for_layer(double td_mm, double layer_height_mm)
{
    if (td_mm <= EPSILON)
        return 1.0;
    return 1.0 - std::exp(-std::log(100.0) * layer_height_mm / td_mm);
}

static Lab lab_td_weighted_oklab_mix(const std::vector<LabTDRidgeMaterial> &materials, bool td_weighted)
{
    double total = 0.0;
    Oklab out {};
    for (const LabTDRidgeMaterial &material : materials) {
        const double strength = td_weighted ? 1.0 / std::max(material.td_mm, EPSILON) : 1.0;
        const double weight = material.fraction * strength;
        total += weight;
        out.L += weight * material.oklab.L;
        out.a += weight * material.oklab.a;
        out.b += weight * material.oklab.b;
    }
    if (total <= EPSILON)
        return {};
    out.L /= total;
    out.a /= total;
    out.b /= total;
    return lab_from_oklab(out);
}

static std::optional<Lab> lab_td_weighted_linear_rgb_mix(
    const std::vector<FullSpectrumKSPairResidualColorInput> &color_percents)
{
    double total = 0.0;
    LinearRgb out {};
    for (const FullSpectrumKSPairResidualColorInput &input : color_percents) {
        if (input.percent <= 0)
            continue;
        const std::optional<LinearRgb> rgb = linear_rgb_from_hex(input.color_hex);
        if (!rgb)
            return std::nullopt;
        const double weight = double(input.percent);
        total += weight;
        out.r += weight * rgb->r;
        out.g += weight * rgb->g;
        out.b += weight * rgb->b;
    }
    if (total <= EPSILON)
        return std::nullopt;
    out.r /= total;
    out.g /= total;
    out.b /= total;
    return lab_from_linear_rgb(out);
}

static std::optional<std::array<double, LabTDRidgeData::FEATURE_COUNT>> lab_td_ridge_features(
    const std::string&                                       base_color_hex,
    const std::vector<FullSpectrumKSPairResidualColorInput>& color_percents,
    const std::vector<LabTDRidgeMaterial>&                   materials,
    double                                                   layer_height_mm)
{
    const std::optional<Lab> base_lab = lab_from_hex_color(base_color_hex);
    if (!base_lab)
        return std::nullopt;

    const std::optional<Lab> linear_rgb_lab = lab_td_weighted_linear_rgb_mix(color_percents);
    if (!linear_rgb_lab)
        return std::nullopt;

    double max_fraction        = 0.0;
    double min_fraction        = std::numeric_limits<double>::max();
    double entropy             = 0.0;
    double fraction_square_sum = 0.0;
    for (const LabTDRidgeMaterial& material : materials) {
        max_fraction = std::max(max_fraction, material.fraction);
        min_fraction = std::min(min_fraction, material.fraction);
        fraction_square_sum += material.fraction * material.fraction;
        entropy += -material.fraction * std::log(std::max(material.fraction, EPSILON));
    }
    if (materials.size() > 1)
        entropy /= std::log(double(materials.size()));
    else
        entropy = 0.0;

    std::vector<double> hues;
    std::vector<double> chromas;
    std::vector<double> opacities;
    hues.reserve(materials.size());
    chromas.reserve(materials.size());
    opacities.reserve(materials.size());
    for (const LabTDRidgeMaterial& material : materials) {
        const double opacity = lab_td_opacity_for_layer(material.td_mm, layer_height_mm);
        hues.push_back(lab_hue(material.lab));
        chromas.push_back(lab_chroma(material.lab));
        opacities.push_back(opacity);
    }

    double weighted_lab_L      = 0.0;
    double weighted_lab_a      = 0.0;
    double weighted_lab_b      = 0.0;
    double weighted_oklab_L    = 0.0;
    double weighted_oklab_a    = 0.0;
    double weighted_oklab_b    = 0.0;
    double weighted_chroma     = 0.0;
    double weighted_hue_sin    = 0.0;
    double weighted_hue_cos    = 0.0;
    double weighted_td         = 0.0;
    double weighted_inverse_td = 0.0;
    double weighted_opacity    = 0.0;

    for (size_t i = 0; i < materials.size(); ++i) {
        const LabTDRidgeMaterial& material = materials[i];
        weighted_lab_L += material.fraction * material.lab.L;
        weighted_lab_a += material.fraction * material.lab.a;
        weighted_lab_b += material.fraction * material.lab.b;
        weighted_oklab_L += material.fraction * material.oklab.L;
        weighted_oklab_a += material.fraction * material.oklab.a;
        weighted_oklab_b += material.fraction * material.oklab.b;
        weighted_chroma += material.fraction * chromas[i];
        weighted_hue_sin += material.fraction * std::sin(hues[i]);
        weighted_hue_cos += material.fraction * std::cos(hues[i]);
        weighted_td += material.fraction * material.td_mm;
        weighted_inverse_td += material.fraction / std::max(material.td_mm, EPSILON);
        weighted_opacity += material.fraction * opacities[i];
    }

    double pair_hue_distance         = 0.0;
    double pair_td_difference        = 0.0;
    double pair_log_td_ratio_abs     = 0.0;
    double pair_chroma_difference    = 0.0;
    double pair_lightness_difference = 0.0;
    double pair_opacity_interaction  = 0.0;
    double pair_ratio_asymmetry      = 0.0;
    double pair_oklab_distance       = 0.0;
    double pair_lab_delta_e          = 0.0;

    for (size_t i = 0; i < materials.size(); ++i) {
        const LabTDRidgeMaterial& a = materials[i];
        for (size_t j = i + 1; j < materials.size(); ++j) {
            const LabTDRidgeMaterial& b           = materials[j];
            const double              pair_weight = a.fraction * b.fraction;
            if (pair_weight <= EPSILON)
                continue;

            const double td_ratio       = std::max(a.td_mm, EPSILON) / std::max(b.td_mm, EPSILON);
            const double oklab_distance = std::sqrt((a.oklab.L - b.oklab.L) * (a.oklab.L - b.oklab.L) +
                                                    (a.oklab.a - b.oklab.a) * (a.oklab.a - b.oklab.a) +
                                                    (a.oklab.b - b.oklab.b) * (a.oklab.b - b.oklab.b));
            const double lab_delta_e    = std::sqrt((a.lab.L - b.lab.L) * (a.lab.L - b.lab.L) + (a.lab.a - b.lab.a) * (a.lab.a - b.lab.a) +
                                                    (a.lab.b - b.lab.b) * (a.lab.b - b.lab.b));

            pair_hue_distance += pair_weight * lab_hue_distance(hues[i], hues[j]);
            pair_td_difference += pair_weight * std::abs(a.td_mm - b.td_mm);
            pair_log_td_ratio_abs += pair_weight * std::abs(std::log(td_ratio));
            pair_chroma_difference += pair_weight * std::abs(chromas[i] - chromas[j]);
            pair_lightness_difference += pair_weight * std::abs(a.lab.L - b.lab.L);
            pair_opacity_interaction += pair_weight * opacities[i] * opacities[j];
            pair_ratio_asymmetry += pair_weight * std::abs(a.fraction - b.fraction) / std::max(a.fraction + b.fraction, EPSILON);
            pair_oklab_distance += pair_weight * oklab_distance;
            pair_lab_delta_e += pair_weight * lab_delta_e;
        }
    }

    const Lab oklab    = lab_td_weighted_oklab_mix(materials, false);
    const Lab td_oklab = lab_td_weighted_oklab_mix(materials, true);

    std::array<double, LabTDRidgeData::FEATURE_COUNT> features{};
    size_t                                            index        = 0;
    const auto                                        push_feature = [&features, &index](double value) {
        if (index < features.size())
            features[index++] = value;
    };

    push_feature(double(materials.size()));
    push_feature(max_fraction);
    push_feature(min_fraction);
    push_feature(entropy);
    push_feature(fraction_square_sum);
    push_feature(weighted_lab_L);
    push_feature(weighted_lab_a);
    push_feature(weighted_lab_b);
    push_feature(weighted_oklab_L);
    push_feature(weighted_oklab_a);
    push_feature(weighted_oklab_b);
    push_feature(weighted_chroma);
    push_feature(weighted_hue_sin);
    push_feature(weighted_hue_cos);
    push_feature(weighted_td);
    push_feature(weighted_inverse_td);
    push_feature(weighted_opacity);
    push_feature(pair_hue_distance);
    push_feature(pair_td_difference);
    push_feature(pair_log_td_ratio_abs);
    push_feature(pair_chroma_difference);
    push_feature(pair_lightness_difference);
    push_feature(pair_opacity_interaction);
    push_feature(pair_ratio_asymmetry);
    push_feature(pair_oklab_distance);
    push_feature(pair_lab_delta_e);
    push_feature(base_lab->L);
    push_feature(base_lab->a);
    push_feature(base_lab->b);
    push_feature(oklab.L);
    push_feature(oklab.a);
    push_feature(oklab.b);
    push_feature(td_oklab.L);
    push_feature(td_oklab.a);
    push_feature(td_oklab.b);
    push_feature(linear_rgb_lab->L);
    push_feature(linear_rgb_lab->a);
    push_feature(linear_rgb_lab->b);

    if (index != features.size())
        return std::nullopt;
    return features;
}

static std::optional<FullSpectrumColorPredictionResult> apply_lab_td_ridge_delta_lab(
    const std::string& base_color_hex, const std::vector<FullSpectrumKSPairResidualColorInput>& color_percents)
{
    (void) base_color_hex;
    bool                                                 used_catalog_hex_lab = false;
    bool                                                 defaulted_td         = false;
    bool                                                 td_disabled          = false;
    const std::optional<std::vector<LabTDRidgeMaterial>> materials = lab_td_materials_from_colors(color_percents, used_catalog_hex_lab,
                                                                                                  defaulted_td, td_disabled);
    if (!materials)
        return std::nullopt;

    const std::optional<std::string> canonical_base_hex = lab_td_canonical_filament_mixer_hex(color_percents);
    if (!canonical_base_hex)
        return std::nullopt;
    const double                                                           layer_height_mm = lab_td_layer_height_mm(color_percents);
    const std::optional<std::array<double, LabTDRidgeData::FEATURE_COUNT>> features        = lab_td_ridge_features(*canonical_base_hex,
                                                                                                                   color_percents, *materials,
                                                                                                                   layer_height_mm);
    const std::optional<Lab>                                               base_lab        = lab_from_hex_color(*canonical_base_hex);
    if (!features || !base_lab)
        return std::nullopt;

    size_t outside_feature_count = 0;
    Lab    delta{LabTDRidgeData::INTERCEPT[0], LabTDRidgeData::INTERCEPT[1], LabTDRidgeData::INTERCEPT[2]};
    for (size_t feature = 0; feature < LabTDRidgeData::FEATURE_COUNT; ++feature) {
        if ((*features)[feature] < LabTDRidgeData::FEATURE_MIN[feature] - EPSILON ||
            (*features)[feature] > LabTDRidgeData::FEATURE_MAX[feature] + EPSILON)
            ++outside_feature_count;
        const double scale = std::abs(LabTDRidgeData::FEATURE_SCALE[feature]) > EPSILON ? LabTDRidgeData::FEATURE_SCALE[feature] : 1.0;
        const double standardized = ((*features)[feature] - LabTDRidgeData::FEATURE_MEAN[feature]) / scale;
        delta.L += standardized * LabTDRidgeData::COEFFICIENTS[0][feature];
        delta.a += standardized * LabTDRidgeData::COEFFICIENTS[1][feature];
        delta.b += standardized * LabTDRidgeData::COEFFICIENTS[2][feature];
    }

    double max_fraction = 0.0;
    for (const LabTDRidgeMaterial& material : *materials)
        max_fraction = std::max(max_fraction, material.fraction);
    const double minority_fraction = std::max(0.0, 1.0 - max_fraction);
    const double gate_x            = std::clamp(minority_fraction / LabTDRidgeData::MIN_FULL_CORRECTION_FRACTION, 0.0, 1.0);
    const double correction_gate   = gate_x * gate_x * (3.0 - 2.0 * gate_x);
    delta.L *= correction_gate;
    delta.a *= correction_gate;
    delta.b *= correction_gate;

    FullSpectrumColorPredictionResult result;
    result.color_hex       = lab_to_hex({base_lab->L + delta.L, base_lab->a + delta.a, base_lab->b + delta.b});
    result.prediction_path = used_catalog_hex_lab ? "CatalogHexTDRegression" : "LabTDRegression";
    result.confidence      = std::exp(-LabTDRidgeData::VALIDATION_RMSE_DELTA_E76 / 20.0);
    if (used_catalog_hex_lab)
        result.confidence *= 0.80;
    if (defaulted_td) {
        result.confidence *= 0.70;
        result.missing_data_warnings.emplace_back("missing_td_defaulted_to_6mm");
    }
    if (used_catalog_hex_lab)
        result.missing_data_warnings.emplace_back("catalog_hex_lab_used_for_unmeasured_material");
    if (td_disabled) {
        result.confidence *= 0.90;
        result.missing_data_warnings.emplace_back("td_disabled_neutralized");
    }
    if (correction_gate < 1.0 - EPSILON) {
        result.confidence *= 0.85;
        result.missing_data_warnings.emplace_back("minority_fraction_below_training_range");
    }
    if (outside_feature_count > 0) {
        result.confidence *= std::exp(-0.08 * double(outside_feature_count));
        result.missing_data_warnings.emplace_back("feature_out_of_training_range");
    }
    if (std::abs(layer_height_mm - LabTDRidgeData::EXPECTED_LAYER_HEIGHT_MM) > 0.005) {
        result.confidence *= 0.70;
        result.missing_data_warnings.emplace_back("layer_height_outside_training_domain");
    }
    result.confidence = std::clamp(result.confidence, 0.10, 0.85);
    return result;
}

} // namespace

std::optional<std::string> full_spectrum_ks_blend_color_multi(const std::vector<std::pair<std::string, int>>& color_percents)
{
    std::vector<FullSpectrumKSPairResidualColorInput> inputs;
    inputs.reserve(color_percents.size());
    for (const auto& [hex, pct] : color_percents)
        inputs.push_back({hex, pct, std::nullopt});
    return blend_from_colors(inputs);
}

std::optional<std::string> full_spectrum_ks_blend_color_multi(
    const std::vector<FullSpectrumKSPairResidualColorInput> &color_percents)
{
    return blend_from_colors(color_percents);
}

std::optional<std::string> full_spectrum_ks_apply_pair_residual_delta_lab(
    const std::string                                      &base_color_hex,
    const std::vector<FullSpectrumKSPairResidualColorInput> &color_percents)
{
    return apply_pair_residual_delta_lab(base_color_hex, color_percents);
}

std::optional<FullSpectrumColorPredictionResult> full_spectrum_lab_td_ridge_apply_delta_lab_prediction(
    const std::string                                      &base_color_hex,
    const std::vector<FullSpectrumKSPairResidualColorInput> &color_percents)
{
    return apply_lab_td_ridge_delta_lab(base_color_hex, color_percents);
}

std::optional<std::string> full_spectrum_lab_td_ridge_apply_delta_lab(
    const std::string                                      &base_color_hex,
    const std::vector<FullSpectrumKSPairResidualColorInput> &color_percents)
{
    const auto prediction = apply_lab_td_ridge_delta_lab(base_color_hex, color_percents);
    if (!prediction)
        return std::nullopt;
    return prediction->color_hex;
}

std::optional<std::string> full_spectrum_ks_blend_color(const std::string &color_a,
                                                        const std::string &color_b,
                                                        int                ratio_a,
                                                        int                ratio_b)
{
    return full_spectrum_ks_blend_color(color_a, color_b, ratio_a, ratio_b, std::nullopt, std::nullopt);
}

std::optional<std::string> full_spectrum_ks_blend_color(const std::string           &color_a,
                                                        const std::string           &color_b,
                                                        int                          ratio_a,
                                                        int                          ratio_b,
                                                        const std::optional<double> &td_a_mm,
                                                        const std::optional<double> &td_b_mm)
{
    return blend_from_colors({
        {color_a, std::max(0, ratio_a), td_a_mm},
        {color_b, std::max(0, ratio_b), td_b_mm}
    });
}

bool full_spectrum_ks_profile_matches_color(const std::string &hex)
{
    return material_index_for_color(hex).has_value();
}

std::optional<double> full_spectrum_ks_profile_td_mm_for_color(const std::string &hex)
{
    const std::optional<size_t> material_index = material_index_for_color(hex);
    if (!material_index)
        return std::nullopt;
    return material_td_mm(*material_index);
}

const char* full_spectrum_ks_profile_id()
{
    return MaterialDatabaseData::PROFILE_ID;
}

const char* full_spectrum_ks_profile_specular_mode()
{
    return MaterialDatabaseData::SPECULAR_MODE;
}

const char* full_spectrum_ks_profile_backing_condition()
{
    return MaterialDatabaseData::BACKING_CONDITION;
}

const char* full_spectrum_lab_td_ridge_model_id()
{
    return LabTDRidgeData::MODEL_ID;
}

const char* full_spectrum_lab_td_ridge_model_type()
{
    return LabTDRidgeData::MODEL_TYPE;
}

const char* full_spectrum_lab_td_ridge_target_specular_mode()
{
    return LabTDRidgeData::TARGET_SPECULAR_MODE;
}

const char* full_spectrum_lab_td_ridge_target_backing_condition()
{
    return LabTDRidgeData::TARGET_BACKING_CONDITION;
}

} // namespace Slic3r
