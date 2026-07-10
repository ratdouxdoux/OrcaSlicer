# Swatch Data Generator And KM/K-S Learned-Pair Engine

## Purpose

The swatch data generator creates printable color calibration coupons and a manifest that describes each coupon precisely enough to become training data. That data is used to build the KM/K-S learned-pair color prediction engine: a spectral predictor that starts from Kubelka-Munk K/S mixing and adds measured pair residuals for known filament combinations.

This document explains the data flow between those two systems. It is not a measurement protocol; use `Color-Calibration-Swatch-Test-Plan.md` for print handling, measurement order, and instrument workflow.

## Data Flow

1. Generate a swatch plan from the current physical filaments, TD values, requested ratio families, layer height, Local Z settings, backing choices, and layout settings.
2. Print the generated objects and save the JSON/CSV manifest next to the project or measurement run.
3. Measure each physical coupon and export Lab values, raw spectra when available, repeated readings, backing condition, instrument settings, and defect notes.
4. Join measurements back to the manifest by stable swatch identity.
5. Train or update a spectral profile from anchors and mix swatches.
6. Generate an embedded profile header consumed by the KM/K-S learned-pair engine.

## Generator Responsibilities

The generator lives in `src/libslic3r/ColorCalibrationSwatches.*` and is surfaced through the calibration swatch dialog and plater workflow.

The generated manifest is the contract between a physical coupon and a training record. It must preserve:

- `swatch_id`, `printed_reference`, `plate_reference`, and `sample_number`
- swatch type, object name, plate index, row, column, and XY position
- physical filament slots, names, short labels, source hex colors, and TD values
- ratio counts, percentages, effective layer counts, effective layer sequence, and effective layer cycle
- total swatch thickness, layer height, measurement side, backing, stack order, and top-material fields
- run-level layer height, swatch depth, Local Z state, direct multicolor state, warnings, and actual layer-time data when available

The printed reference should stay compact, such as `A37`. The full identity belongs in the manifest, not on the swatch body.

## Swatch Families

Reflective anchors calibrate the source filaments. They provide the measured baseline color, spectra, and TD behavior for each physical material.

TD ladders show how source filaments change with optical depth. They help confirm whether the chosen opaque depth is actually independent of backing.

Pair mixes are the primary training rows for the learned-pair engine. For each pair and ratio, the training step compares the measured black-backed side result with the plain KM/K-S prediction and learns the pair-specific residual.

Pair-order swatches expose top-layer and stack-order behavior. Keep them separate from ordinary pair mixes unless the model explicitly includes stack order as an input feature.

Ternary and four-color mixes are early validation and composition checks. They should first answer whether learned pair residuals compose cleanly before being used for higher-order correction terms.

## Measurement Data To Keep

Training should prefer raw spectra over derived colors. Lab and display hex are useful exports, but they cannot recover wavelength-level residuals later.

Keep repeated readings, averaged readings, outlier flags, instrument serial/model, measurement geometry, specular mode, illuminant, observer, backing condition, timestamp, and notes. Keep compromised or unmeasured rows as flagged records instead of silently deleting them, because missing data can otherwise bias profile generation.

The training/export schema should be versioned separately from the printable swatch manifest schema. The swatch manifest says what was printed; the training dataset says what was measured and how it was accepted.

## Engine Responsibilities

The engine is selected through `MixedFilamentColorEngine::FullSpectrumKSPairResidual` and the UI label `KM/K-S learned pair`. Its implementation is in `src/libslic3r/FullSpectrumKSPairResidual.*`, with generated profile data in `FullSpectrumMaterialDatabaseProfile.h`.

For a deeper implementation explanation, see `KM-KS-Learned-Pair-Engine.md`.

At prediction time the engine:

1. Normalizes valid source colors and maps known colors to embedded material spectra.
2. Estimates a K/S spectrum from hex for valid colors outside the embedded profile.
3. Weights each input by recipe percentage, optionally adjusted by inverse TD so lower-TD filaments contribute more optical strength.
4. Mixes the material K/S spectra.
5. Applies learned pair residual coefficients when both materials belong to the embedded profile.
6. Converts the predicted reflectance spectrum to Lab, then to display hex for preview.

Unknown valid colors therefore still use plain KM/K-S estimation. Learned residuals are applied only where the profile contains measured material pairs.

## Pair Residual Model

Plain KM/K-S mixing is the baseline. It assumes the mixed result can be predicted by combining source material K/S spectra according to their optical weights.

Printed filament pairs often deviate from that baseline because pigment interaction, layer cadence, scattering, surface texture, and filament opacity are not perfectly represented by source spectra alone. The learned-pair layer stores a residual for each measured material pair and applies it as a spectral correction after the baseline mix.

The current profile format stores pair residual coefficients per wavelength. The prediction code evaluates the residual from both pair composition and balance, so a 50/50 pair can differ from a skewed 1:7 or 7:1 pair without requiring a separate lookup table for every ratio.

## Profile Scope

A profile is valid only for the conditions that produced it. Do not merge or reuse measurements across these boundaries unless the model has an explicit feature for the difference:

- material set, spool lots, and material family
- source colors and TD values
- side or top measurement geometry
- layer height and actual layer-time behavior
- normal mode, Local Z, or direct multicolor Local Z
- backing condition
- specular mode, illuminant, and observer
- instrument and measurement workflow

The current generated material database profile is scoped as `0.08 mm sidewall, SCE spectra, black_backing`. Profile IDs should continue to encode enough context to prevent accidental reuse in incompatible preview modes.

## Maintenance Checklist

- Keep generator manifest fields stable when adding new swatch families.
- Add new manifest fields instead of changing the meaning of existing ones.
- Preserve raw spectra and repeated readings through import/export.
- Keep pair-order, backing, top-face, and backlit datasets separate until the engine models those variables.
- Regenerate embedded profile headers only from accepted measurement datasets.
- Update this document when the training schema or profile header format changes.

## Related Files

- `src/libslic3r/ColorCalibrationSwatches.hpp`
- `src/libslic3r/ColorCalibrationSwatches.cpp`
- `src/libslic3r/FullSpectrumKSPairResidual.hpp`
- `src/libslic3r/FullSpectrumKSPairResidual.cpp`
- `src/libslic3r/FullSpectrumMaterialDatabaseProfile.h`
- `src/libslic3r/MixedFilament.hpp`
- `doc/developer-reference/KM-KS-Learned-Pair-Engine.md`
- `doc/developer-reference/Color-Calibration-Swatch-Test-Plan.md`
