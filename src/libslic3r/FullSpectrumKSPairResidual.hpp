#ifndef slic3r_FullSpectrumKSPairResidual_hpp_
#define slic3r_FullSpectrumKSPairResidual_hpp_

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

struct FullSpectrumKSPairResidualColorInput
{
    std::string           color_hex;
    int                   percent = 0;
    std::optional<double> td_mm;
    std::optional<double> layer_height_mm;
    bool                  use_td = true;
};

struct FullSpectrumColorPredictionResult
{
    std::string              color_hex;
    std::string              prediction_path;
    double                   confidence = 0.0;
    std::vector<std::string> missing_data_warnings;
};

// Spectral sidewall predictor. Colors matching the embedded 0.08 mm SCE
// black-backed profile use measured anchor spectra and learned pair residuals.
// Other valid hex colors are converted to estimated anchor spectra and mixed
// with plain KM/K-S so changing one filament color does not silently fall back
// to the legacy RGB mixer.
std::optional<std::string> full_spectrum_ks_blend_color_multi(
    const std::vector<std::pair<std::string, int>> &color_percents);

std::optional<std::string> full_spectrum_ks_blend_color_multi(
    const std::vector<FullSpectrumKSPairResidualColorInput> &color_percents);

// Applies only the learned pair-residual Delta Lab to an existing base color.
// Exact profile colors use their matching residuals. Other valid colors are
// assigned the nearest learned pair residual so FilamentMixer can be nudged
// without switching to the full spectral prediction.
std::optional<std::string> full_spectrum_ks_apply_pair_residual_delta_lab(
    const std::string                                      &base_color_hex,
    const std::vector<FullSpectrumKSPairResidualColorInput> &color_percents);

std::optional<FullSpectrumColorPredictionResult> full_spectrum_lab_td_ridge_apply_delta_lab_prediction(
    const std::string                                      &base_color_hex,
    const std::vector<FullSpectrumKSPairResidualColorInput> &color_percents);

std::optional<std::string> full_spectrum_lab_td_ridge_apply_delta_lab(
    const std::string                                      &base_color_hex,
    const std::vector<FullSpectrumKSPairResidualColorInput> &color_percents);

std::optional<std::string> full_spectrum_ks_blend_color(const std::string &color_a,
                                                        const std::string &color_b,
                                                        int                ratio_a,
                                                        int                ratio_b);

std::optional<std::string> full_spectrum_ks_blend_color(const std::string           &color_a,
                                                        const std::string           &color_b,
                                                        int                          ratio_a,
                                                        int                          ratio_b,
                                                        const std::optional<double> &td_a_mm,
                                                        const std::optional<double> &td_b_mm);

bool full_spectrum_ks_profile_matches_color(const std::string &hex);

std::optional<double> full_spectrum_ks_profile_td_mm_for_color(const std::string &hex);

const char* full_spectrum_ks_profile_id();
size_t full_spectrum_ks_profile_material_count();
size_t full_spectrum_ks_profile_pair_count();
const char* full_spectrum_ks_profile_specular_mode();
const char* full_spectrum_ks_profile_backing_condition();
const char* full_spectrum_lab_td_ridge_model_id();
const char* full_spectrum_lab_td_ridge_model_type();
const char* full_spectrum_lab_td_ridge_target_specular_mode();
const char* full_spectrum_lab_td_ridge_target_backing_condition();

} // namespace Slic3r

#endif // slic3r_FullSpectrumKSPairResidual_hpp_
