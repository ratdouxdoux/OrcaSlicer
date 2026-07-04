# Full Spectrum Color Reference

This reference is for users who want a practical color guide while the production color algorithm is still under development. It is based on the currently measured swatch datasets plus baked reference predictions.

Open the standalone page here:

- [Full Spectrum Color Reference HTML](full-spectrum-color-reference.html)

## Scope

This reference is valid for the data profile it was built from:

- 0.08 mm layer height
- sidewall measurements
- D65 illuminant and CIE 10 degree observer
- SCE black-backed material database profile for precomputed reference swatches
- static display hex values in the HTML page

The source calibration data includes the full Grey-key black-backed measured set. The reference displays only the thickest anchor for each physical filament, plus every measured mix swatch:

| Dataset | Measured records shown | Computed fills | Total swatches |
| --- | ---: | ---: | ---: |
| CMY + grey | 177 | 144 | 321 |

## Physical Materials

The generated reference fixes all four slots:

| Slot | Material | Hex | TD |
| --- | --- | --- | ---: |
| F1 | Cyan | `#008BB3` | 6.4 mm |
| F2 | Magenta | `#AD4A76` | 5.0 mm |
| F3 | Yellow | `#EBBE00` | 9.7 mm |
| F4 | Grey | `#7B7F80` | 6.8 mm |

## How To Use The HTML Reference

The HTML page is a static swatch reference with two sections. The first section shows the four thickest black-backed reflective anchors and all 173 black-backed measured Grey-key mix swatches, sorted by the mixed-color numbers used in the 3MF and grouped into pair, 3-filament, and 4-filament combos. The second section shows 144 computed extra recipe fills, also grouped into pair, 3-filament, and 4-filament combos.

Measured manifest Lab values are converted to display hex. Measured swatch tile numbers match the mixed-color/extruder numbers stored in `Calibration SwatchesPanchroma.3mf`; computed fills are numbered after the measured range because they are not present as mixed colors in that 3MF. Computed fills use precomputed reference hex values. The page intentionally contains no live prediction engine, spectra, Lab values, or model coefficients.

## What The Prediction Means

The baked prediction path was generated from the KM/K-S reference profile before export. The HTML page itself only exposes display hex values and recipe percentages.

This reference should be used as a color planning aid. It should not be treated as a final production guarantee.

## Important Limits

- The page is limited to the material lots in the embedded profile. Same-named filament from another lot may differ in hex and TD.
- The prediction profile is sidewall/SCE/black-backed. Top faces, glossy photo appearance, white backing, other layer heights, and transmission/backlit behavior need separate data.
- Monitor calibration and browser color handling can affect perception.
- Unknown colors are intentionally not exposed in this reference page. The goal is a reliable measured reference, not a universal color promise.
