# KM/K-S Learned-Pair Engine

## Purpose

The KM/K-S learned-pair engine predicts preview colors for mixed filaments. It is selected in the UI as `KM/K-S learned pair` and is implemented by `src/libslic3r/FullSpectrumKSPairResidual.*`.

The engine is spectral rather than RGB-first. It predicts a reflectance spectrum from source filament colors, optional TD values, and an embedded measured profile, then converts that spectrum to Lab and finally to display hex. The learned part is a set of pair residuals: measured corrections that adjust plain Kubelka-Munk K/S mixing for filament pairs that have calibration data.

This engine currently affects mixed-filament preview/display colors. It does not change toolpath generation, physical extruder resolution, layer cadence, or material ratios.

## Where It Lives

- `src/libslic3r/FullSpectrumKSPairResidual.hpp`: public prediction API.
- `src/libslic3r/FullSpectrumKSPairResidual.cpp`: spectral mixing, TD weighting, residual application, and color conversion.
- `src/libslic3r/FullSpectrumMaterialDatabaseProfile.h`: active generated material database profile.
- `src/libslic3r/MixedFilament.hpp`: `MixedFilamentColorEngine::FullSpectrumKSPairResidual`.
- `src/libslic3r/MixedFilament.cpp`: engine selection, TD toggle, calibrated prediction fallback.
- `src/slic3r/GUI/Plater.cpp`: sidebar selector and `Use TD` checkbox.
- `src/slic3r/GUI/MixedFilamentDialog.cpp`: dialog selector and `Use TD` checkbox.
- `tests/libslic3r/test_mixed_filament.cpp`: behavior checks for known profile colors, TD weighting, unknown colors, and engine switching.

## Runtime Selection

The mixed-filament color engine is global runtime state:

- `MixedFilamentColorEngine::FilamentMixer`
- `MixedFilamentColorEngine::FilamentMixerPairResidualDeltaLab`
- `MixedFilamentColorEngine::FullSpectrumKSPairResidual`
- `MixedFilamentColorEngine::FilamentMixerLabTDRidge`

The app config key `mixed_filament_color_engine` stores the selected engine as:

- `filament_mixer`
- `filament_mixer_delta_lab`
- `ks_pair_residual`
- `lab_td_ridge`

The app config key `mixed_filament_use_td_prediction` stores whether TD weighting is enabled. The default app config keeps `filament_mixer` as the default engine and `mixed_filament_use_td_prediction` enabled for when the KM/K-S engine is selected.

Both the sidebar and mixed-filament dialog expose:

- a color engine choice with all four engines
- a `Use TD` checkbox enabled for the three calibrated/corrected engines

Changing either option updates the mixed-filament display context and rebuilds preview colors.

## Input Contract

The low-level calibrated API accepts `FullSpectrumKSPairResidualColorInput` records:

```cpp
struct FullSpectrumKSPairResidualColorInput
{
    std::string           color_hex;
    int                   percent = 0;
    std::optional<double> td_mm;
    std::optional<double> layer_height_mm;
    bool                  use_td = true;
};
```

The mixed-filament manager wraps ordinary preview inputs into that structure when the calibrated engine is selected. It passes TD values only when `MixedFilamentManager::use_td_for_color_prediction()` is true.

Important input behavior:

- colors must be valid `#RRGGBB` hex strings
- zero and negative percentages are ignored
- invalid hex makes the calibrated API return `std::nullopt`
- fewer than two positive input materials makes the calibrated API return `std::nullopt`
- `MixedFilamentManager` falls back to FilamentMixer behavior when the calibrated API returns `std::nullopt`

That fallback keeps pure colors, zero-ratio cases, and invalid input from breaking existing preview behavior.

## Profile Data

The active profile header is generated from SwatchDataExplorer material database profile JSON. The current generated header declares:

- `PROFILE_ID`
- `SPECULAR_MODE`
- `BACKING_CONDITION`
- `WAVELENGTH_NM`
- `MATERIAL_ID`
- `MATERIAL_TD_MM`
- `MATERIAL_HEX`
- `MATERIAL_KS`
- `PAIR_RESIDUALS`

The current scope recorded in the header is:

```text
0.08 mm sidewall, SCE spectra, black_backing
```

The wavelength grid is 400 nm through 700 nm in 10 nm steps, so each spectrum has 31 samples.

The active database is a combined four-profile export containing 16 measured materials and 24 within-profile learned pairs:

- Panchroma Grey-key CMYG
- Panchroma Snapseed CMYW
- Panchroma CMY + Trans Black
- Jayo Red + Flash Blue + Pan Yellow + Snap White

Each material can list one or more accepted display hex values. For example, the same measured material can match both the source color used in older data and the color currently stored in the slicer profile. `material_index_for_color()` normalizes the input hex and searches all accepted hex aliases for each material.

Measured per-lot hex values are the authoritative identities. Shared nominal CMY aliases remain assigned to the original Grey-key profile for backward compatibility; newer lots use their unique measured hex values. In particular, Snapseed `#E4E5E1` and Jayo `#E3E4E0` are separate materials even though the Jayo source filename contains the nominal token `E4E5E1`. Do not assign one ambiguous nominal hex to multiple physical materials without adding an explicit material/profile identifier to the runtime input.

Do not edit the generated profile header by hand. Regenerate it from the accepted measurement/profile pipeline so spectra, material aliases, TD values, and pair residuals stay consistent.

## Prediction Pipeline

At a high level:

1. Normalize and validate each input hex color.
2. Convert each positive input into a `MaterialKS`.
3. Look up known profile materials by hex.
4. Use embedded measured K/S spectra for known materials.
5. Estimate a K/S spectrum from hex for unknown but valid colors.
6. Compute normalized optical weights from percentages and optional TD values.
7. Add the weighted source K/S spectra.
8. Apply learned pair residuals for known material pairs.
9. Convert final K/S to reflectance.
10. Convert reflectance to Lab using D65 and CIE 10 degree observer tables.
11. Convert Lab to display hex.

The result is an sRGB display color for preview.

## K/S Basics

Kubelka-Munk mixing represents an opaque material by a wavelength-dependent K/S value:

```text
K/S = (1 - R)^2 / (2R)
```

where `R` is reflectance at a wavelength. The implementation clamps reflectance before conversion to avoid infinities.

After mixing in K/S space, the engine converts back to reflectance with:

```text
R = 1 + F - sqrt(F^2 + 2F)
```

where `F` is the mixed K/S value for that wavelength.

The important design point is that source colors are not blended as RGB. The engine mixes spectra-like K/S curves and only converts to display color at the end.

## Known Materials

If an input hex matches `MATERIAL_HEX`, the engine uses the measured K/S spectrum from `MATERIAL_KS`.

Known materials also contribute to the material-composition vector used by pair residuals. This vector is indexed by material index, not by input row. If the same known material appears more than once in an input list, its weights accumulate.

Known-material helper APIs:

- `full_spectrum_ks_profile_matches_color(hex)`
- `full_spectrum_ks_profile_td_mm_for_color(hex)`
- `full_spectrum_ks_profile_id()`
- `full_spectrum_ks_profile_specular_mode()`
- `full_spectrum_ks_profile_backing_condition()`

Tests assert that the active profile recognizes the current measured color aliases and exposes their TD values.

## Unknown Valid Colors

Unknown valid hex colors do not use learned residuals, but they still participate in spectral prediction.

The engine estimates a K/S spectrum from the input hex by:

1. converting sRGB to linear RGB
2. separating neutral, red, green, and blue components
3. building an approximate reflectance curve from Gaussian red, green, and blue bases
4. converting that estimated reflectance curve to K/S

This keeps valid custom colors in the KM/K-S path instead of silently dropping back to the legacy RGB mixer. The output is still an estimate; it is not a calibrated profile match.

## TD Weighting

Pair residuals are trained at each known material's profile TD. When runtime TD is present, finite, positive, and the `Use TD` toggle is enabled, known materials use a profile-relative optical-strength correction:

```text
raw_weight = percent * profile_TD / runtime_TD
```

At the native profile TD this reduces to `raw_weight = percent`, preserving the exact composition used during training. For an unknown material, the same dimensionless correction uses a neutral 6 mm reference TD:

```text
raw_weight = percent * 6 mm / runtime_TD
```

When TD is missing or disabled:

```text
raw_weight = percent
```

All raw weights are then normalized:

```text
weight_i = raw_weight_i / sum(raw_weight)
```

Lower runtime TD than the material's reference value means the filament reaches opacity faster, so it contributes more optical strength for the same nominal percentage. Tests require native profile TD to reproduce the trained no-adjustment result, changed TD to change the prediction, and disabling TD to return to the no-adjustment result.

## Baseline Mixing

For every wavelength:

```text
ks_mix(lambda) = sum(weight_i * ks_i(lambda))
```

This is the plain KM/K-S estimate. It works for any valid input color because unknown colors receive estimated K/S spectra.

Known profile colors are more accurate at this stage because they use measured anchor spectra rather than estimated spectra.

## Pair Residuals

Plain KM/K-S mixing is not enough for printed filament pairs. Layer cadence, scattering, opacity, surface texture, and pigment interaction can make a measured pair differ from the baseline spectral prediction.

The learned-pair model stores residual coefficients for measured material pairs. Each residual contains:

```cpp
struct PairResidualCoefficients
{
    int material_a = 0;
    int material_b = 0;
    std::array<double, SPECTRUM_SIZE> b0 {};
    std::array<double, SPECTRUM_SIZE> b1 {};
    std::array<double, SPECTRUM_SIZE> b2 {};
};
```

For each residual pair, the engine reads the normalized known-material composition:

```text
pa = composition[material_a]
pb = composition[material_b]
```

If either side is absent, that residual is skipped. Otherwise:

```text
d = (pa - pb) / (pa + pb)
product = pa * pb
residual(lambda) = product * (b0(lambda) + b1(lambda) * d + b2(lambda) * d^2)
ks_mix(lambda) += residual(lambda)
```

This shape has useful behavior:

- the residual is zero when either material is absent
- the residual is strongest near balanced mixes because `pa * pb` is largest there
- `b0` captures the central pair correction
- `b1` captures directionality between A-heavy and B-heavy mixes
- `b2` captures symmetric curvature as the mix becomes skewed

For ternary and four-color previews, every residual pair present in the embedded profile is evaluated against the current material composition. That means pair residuals can compose across larger recipes, but only for known material pairs included in the profile.

Unknown colors have no material index, so they add baseline K/S but do not trigger learned residuals.

## Reflectance To Display Hex

After residuals are applied, each K/S sample is converted back to reflectance. The reflectance spectrum is then integrated with built-in D65 illuminant samples and CIE 10 degree observer color matching samples over the 400-700 nm grid.

The pipeline is:

```text
reflectance spectrum -> XYZ -> Lab -> linear RGB -> sRGB hex
```

The output conversion clamps the final RGB channels into displayable sRGB. That makes the preview robust, but it also means the final hex is a display approximation of the predicted Lab color, not the full spectral output.

## Fallback Behavior

The calibrated API returns `std::nullopt` when it cannot produce a valid spectral prediction. `MixedFilamentManager` then uses existing FilamentMixer behavior.

Common fallback cases:

- no colors
- invalid hex
- no positive percentages
- only one positive material

This is why pure-ratio edge cases still return the pure source color. The calibrated engine is used when it can make a meaningful mix prediction; existing behavior handles everything else.

## Multi-Color Recipes

The same `blend_color_multi()` path handles pair, ternary, and larger recipe previews.

For known profile materials:

- all source K/S spectra contribute to the baseline
- all available pair residuals among present materials contribute corrections

For unknown materials:

- estimated K/S contributes to the baseline
- no pair residual is applied for that unknown material

This lets measured pairs improve multi-color predictions without requiring a separate measured table for every ternary or quaternary recipe. The tradeoff is that higher-order optical interactions are not explicitly modeled yet.

## Relation To Swatch Data

Swatch generator output becomes training data for future profile headers:

- reflective anchors become measured source K/S spectra and TD baselines
- pair mixes become residual fitting targets
- pair-order rows stay separate unless stack order becomes a model input
- ternary and four-color rows validate whether pair residuals compose cleanly
- raw spectra are preferred because the active engine learns and applies wavelength-level corrections

For the broader generator data contract, see `Swatch-Data-Generator-and-KM-KS-Learned-Pair-Engine.md`.

## Profile Scope And Limits

The active profile is scoped to its measurement conditions. Its predictions should be treated as calibrated only for matching conditions:

- sidewall measurement geometry
- 0.08 mm layer height
- SCE spectra
- black backing
- D65 illuminant and CIE 10 degree observer conversion
- material lots represented by the embedded profile

Predictions outside that scope still run, but should be understood as extrapolation:

- unknown colors use estimated spectra
- different layer heights may change cadence and surface appearance
- white backing, top-face, glossy, backlit, or transmission use cases need separate data
- different material families may scatter differently
- TD weighting is a useful optical-strength approximation, not a full translucency model

## Updating Or Extending The Engine

When updating the profile:

1. Generate and measure swatches with stable manifests.
2. Keep raw spectra and repeated readings.
3. Accept or reject rows with explicit defect flags.
4. Train all accepted profile families into one material database rather than replacing the previous export with the newest source.
5. Preserve unique measured material identities and apply the documented deterministic alias policy.
6. Regenerate `FullSpectrumMaterialDatabaseProfile.h`.
7. Run the mixed-filament color tests and verify all expected material families and pair counts remain present.
8. Report metrics per source family and mix order as well as the aggregate score; adding a harder profile must not hide a regression in an existing family.

When extending the model, prefer adding explicit profile fields rather than overloading existing meanings. Likely future extensions include:

- profile-level measurement geometry constants beyond specular/backing strings
- layer-height-specific profiles
- top-face or backlit profiles
- pair-order or stack-order inputs
- higher-order corrections for ternary and four-color mixes
- direct exposure of predicted Lab or spectra for debugging and validation

## Debug Checklist

If a preview color looks wrong:

- Confirm the selected engine is `KM/K-S learned pair`.
- Confirm `Use TD` is in the intended state.
- Check whether each input hex matches `MATERIAL_HEX`.
- Check whether TD values are present, finite, and positive.
- Confirm percentages are positive and sum to a meaningful recipe.
- Compare with FilamentMixer to separate engine behavior from UI refresh issues.
- Check `full_spectrum_ks_profile_id()`, specular mode, and backing condition.
- Add or update tests before changing expected calibrated output.
