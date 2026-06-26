# Color Calibration Swatch Test Plan

## Purpose

This test plan is for measuring how four source filaments with known colors and transmission distances influence the final side-visible color of printed mixes.

The dataset should let us model color from:

- Source filament colors and TD values
- Pair, ternary, and four-color mix ratios
- Layer height
- Mix mode: normal, Local Z, Local Z plus direct multicolor solver
- Printed swatch depth
- Measurement backing and lighting condition

The first priority is reflective side measurement. Backlit/transmissive measurement is valuable, but should be treated as a separate dataset because it answers a different optical question.

## Core Rules

Every swatch should be printed as a dense optical coupon.

- Measure from the side face, not the top.
- Print with enough walls/perimeters to fill the swatch completely.
- Avoid sparse infill, internal voids, or air pockets.
- Verify preview/G-code shows a dense solid body before printing a test plate.
- Keep nozzle, temperatures, speed, cooling, extrusion calibration, and filament drying state consistent.
- Use the spectrophotometer jig when possible and measure the same side/position every time.
- If the jig cannot be used, keep the measurement geometry as repeatable as possible and shield the sample from stray light, especially for thin samples.
- Take multiple readings per swatch, normally 3 or more, and store raw spectrum data when available.

Internal voids are expected to change scattering and therefore color. A swatch with voids is probably not representative of the actual material mix.

## Traceability And Plate Naming

Every printed plate should have a human-readable title and a unique plate/run identifier.

Recommended plate IDs:

- `P-1`
- `P-2`
- `P-3`

The printed plate label and manifest title should include:

- Plate/run ID
- Filament set name
- Source filament slots
- Source filament spool identifiers
- Material family
- Layer height
- Swatch depth
- Mix mode
- Plate number, if the set spans multiple plates

Each physical spool should have a visible identifier written on it or attached to it. Use stable IDs such as:

- `C1`, `M1`, `Y1`, `W1`
- `R2`, `Y2`, `B2`, `K2`
- `PLA-CYAN-HITD-01`

The spool ID should be recorded in the manifest notes or test log. Do not rely only on color names such as "cyan" or "white"; two visually similar spools can have different TD and pigment behavior.

Before removing any swatches, photograph the full plate from above with the plate label visible. This gives a recovery reference if a swatch is accidentally moved.

Each swatch should be marked with a compact printed reference only, such as `A37`. The plate label should show the same letter, such as `Plate A 1/1`. The letter identifies the plate/run reference and must be unique for the printed run. The number identifies the sample row in the manifest. The compact swatch reference should be engraved low on the rear sidewall, near the bottom edge, so it remains visible while staying away from the main measurement area. Use the manifest, not the printed swatch, for the full filament slots, ratios, colors, TD values, layer height, depth, and mode.

## Slicer Setup Verification

The calibration dialog fields for Local Z and direct multicolor should be treated as test metadata and verification flags.

Before slicing, manually verify the actual print settings match the intended test:

- For normal mode, `Subdivide Mix Layer` should be off.
- For Local Z mode, `Subdivide Mix Layer` should be on.
- For Local Z plus direct multicolor, both `Subdivide Mix Layer` and `Use direct multicolor Local-Z solver` should be on.

Do not rely only on the calibration dialog checkboxes or the manifest labels. The slice preview, project settings, and generated G-code are the source of truth for what will actually be printed.

Also verify:

- Wall/perimeter count is high enough to fill the swatch.
- Sparse infill is not creating internal voids.
- Prime tower placement does not collide with swatches or the jig.
- The expected filament slots and mixed filament definitions are present before slicing.
- The plate label matches the actual print settings.

## Depth Definitions

For each four-filament source set:

```text
max_td = highest TD among the four source filaments
opaque_depth = max(max_td + 1.0 mm, max_td * 1.2)
```

Round `opaque_depth` to a practical printable value.

Each source filament TD should be entered in the calibration swatch generator before creating the plate. Anchor depths are generated per filament, not from the set-wide maximum TD:

- `1.0 mm`
- `source_td / 2 + 1.0 mm`
- `source_td + 1.0 mm`

The full mix grid should first be printed at `opaque_depth`. The thinner depths should initially be used on a smaller representative subset so the matrix does not explode.

## Layer Height Test Steps

### Step 1: First Required Layer Heights

Start with normal mode only:

- `0.08 mm`
- `0.24 mm`

For normal mode, pair, ternary, and four-color mixes should be generated and measured at these two layer heights first.

### Step 2: Later Expansion

After Step 1 data has been reviewed, expand normal mode to:

- `0.12 mm`
- `0.16 mm`
- `0.20 mm`

Then add Local Z at:

- `0.20 mm`
- `0.24 mm`
- `0.28 mm`

Then add Local Z plus direct multicolor solver at:

- `0.20 mm`
- `0.24 mm`
- `0.28 mm`

## Phase 1: Source Filament Characterization

For each source filament set, enter the TD for every physical filament and print solid physical filament anchor swatches for each physical filament at:

- `1.0 mm`
- `source_td / 2 + 1.0 mm`
- `source_td + 1.0 mm`

Measure each anchor with:

- White backing
- Black backing

Goal:

- Confirm whether `source_td + 1.0 mm` is actually optically stable.
- Estimate how strongly backing affects thin and thick samples.
- Capture baseline Lab and spectrum for every source filament.

If white/black backing changes a `source_td + 1.0 mm` anchor significantly, the swatch is not optically opaque enough for that filament or material family.

## Phase 2: Main Opaque Mix Grid

For each filament set, print the generated mix grid at `opaque_depth`.

Include:

- Pair mixes
- Ternary mixes
- Four-color mixes

Run Step 1 first. Step 2 layer heights and modes should be printed later, after the Step 1 data has been checked.

Goal:

- Learn the color response at a depth that should be mostly independent of backing.
- Measure non-anchor mix swatches on black backing only.
- Compare normal layer stacking, Local Z, and direct multicolor Local Z.
- Determine how much layer height changes the same nominal ratio.

## Phase 3: Depth Model Subset

Do not print the full mix grid at every depth at first. Anchors already get the per-filament TD depth sweep:

- `1.0 mm`
- `source_td / 2 + 1.0 mm`
- `source_td + 1.0 mm`

If mix-depth modeling is needed after the anchor data, print a representative non-anchor subset at:

- `1.0 mm`
- `opaque_depth / 2`
- `opaque_depth`

Recommended subset:

- All source filament anchors
- All 50/50 pair mixes
- A few skewed pairs, such as `1:3` and `3:1`
- One balanced ternary per filament triple
- One skewed ternary per filament triple
- One balanced four-color mix
- Several skewed four-color mixes

Goal:

- Estimate how depth affects color and translucency.
- Check whether color can be interpolated between thin, half-depth, and opaque-depth samples.
- Decide whether the full grid needs additional depth sweeps.

## Phase 4: Backing And Lighting Subset

Backing should be tested as a subset, not immediately multiplied across the entire grid.

Use:

- White backing
- Black backing

Start with:

- Anchors
- Thin and TD-derived anchor depths

Backlit measurement should be a separate experiment:

- It may be important for lithophanes, lamps, and transmissive parts.
- It should not be mixed into the reflective side-measurement model without a separate field identifying the measurement geometry.
- It requires a transmission-capable instrument or a calibrated spectral irradiance setup, not the current NH9 reflective measurement workflow.

## Candidate Filament Sets

Start with one material family, such as PLA, before mixing material types.

Recommended first sets:

- High-TD CMY plus white
- High-TD CMY plus black
- Low-TD CMY plus white
- Low-TD CMY plus black
- High-TD RYB plus white
- High-TD RYB plus black
- Low-TD RYB plus white
- Low-TD RYB plus black

Useful optional sets:

- RGB plus white
- RGB plus black
- RGB plus clear or natural filament
- Neutral optical set: white, gray, black, clear/natural
- Opponent set: cyan, red, yellow, blue
- Same hue with different TD values
- Similar TD values with different hues

Keep fluorescent, silk, matte, glossy, PETG, ABS, and other special material families separate. They may have different scattering and surface behavior.

## Color Variation Sets

After the first clean dataset, add controlled variations:

- CMY sets with slightly different cyan, magenta, or yellow hues
- RYB sets with warmer/cooler red, yellow, or blue
- Similar Lab color but different TD
- Similar TD but different Lab color

These sets help answer whether the model generalizes by measured source color and TD, or whether it is too specific to one brand/color family.

## Plate Preparation And Printing

The printer should have XY head calibration completed before printing calibration swatches.

The build plate should be washed before printing, or otherwise confirmed clean and free of oil, dust, and old adhesive residue. Use the normal cleaning method for the plate surface, such as dish soap and water for PEI, then dry it fully. The point is to reduce adhesion failures, partial lifts, and inconsistent bottom contact.

Before starting a run:

- Confirm XY head calibration has been completed.
- Confirm the plate is clean and dry.
- Confirm all filaments are dry enough for reliable extrusion.
- Confirm all spool IDs match the manifest and slicer slots.
- Confirm the printer has enough material loaded for the full plate.
- Confirm the print is using the intended nozzle, layer height, material profile, and bed type.

During and after printing:

- Keep the heated bed at temperature until the plate is safely removed from the printer.
- Do not let the print cool and release on the printer if that risks swatches unsticking and losing their order.
- Move the entire plate carefully and keep it flat.
- Avoid bending the build plate in any direction before the swatches are documented.
- Store the full plate flat until measurement.

If any swatches detach early, mark the plate as compromised unless their identity and orientation are completely unambiguous.

## Plate Storage And Handling

After printing, plates should be treated as ordered sample trays.

- Store each plate with its manifest or plate ID.
- Keep plates flat in a clean box, tray, or labeled sleeve.
- Avoid stacking plates in a way that rubs swatch faces.
- Avoid touching the measurement side with fingers.
- Keep dust, oils, and loose filament debris away from the side faces.
- If possible, measure plates soon after printing or record storage duration and conditions.

Humidity, UV exposure, dust, and surface handling can all become hidden variables if plates sit for a long time before measurement.

## Measurement Handling

Measurement should preserve swatch order.

Before measuring:

- Put the plate on a stable table.
- Fix the plate to the table with blue tack or an equivalent removable system.
- Make sure the plate cannot slide, tip, or fall while swatches are being removed.
- Use the spectrophotometer jig when possible.
- If using the jig, fix it to the table with double-sided tape or another repeatable removable mount.
- Confirm the jig cannot rotate or drift during the measurement session.
- If measuring without the jig, keep the NH9 position and sample orientation repeatable and block avoidable stray light from the sample face.
- For thin samples, take extra care to avoid light pollution around the sample and backing.
- Open the slicer calibration tab and use the NH9 measurement workflow directly in the slicer.
- Verify the NH9 is in `I+E` mode before starting measurements: `Settings -> Sample mode`.
- Keep the plate photo and manifest visible.

Measure one swatch at a time:

1. Identify the next swatch from the manifest.
2. Remove only that swatch from the plate.
3. Place it in the jig when possible, with the intended measurement side facing the instrument. If the jig cannot be used, keep the same side, orientation, backing, and NH9 position as repeatable as possible.
4. Take the configured number of readings.
5. Store the measured swatch in a labeled completed-sample area.
6. Move to the next swatch.

Do not remove a batch of loose swatches at once. If several similar swatches are loose on the table, order recovery becomes unreliable.

If a swatch chips, bends, falls, rotates ambiguously, or loses its identity, flag it in the notes instead of silently measuring it.

## Measurement Workflow

For each plate:

1. Load or generate the swatch manifest.
2. Confirm the manifest records:
   - Filament slot IDs
   - Source colors
   - Source TD values
   - Layer height
   - Local Z mode
   - Direct multicolor solver state
   - Swatch depth
   - Plate number
3. Verify actual slicer settings for Local Z and direct multicolor mode.
4. Confirm XY head calibration has been completed.
5. Wash and dry the build plate, or confirm the print surface is clean.
6. Print with dense wall settings.
7. Keep the bed hot until the plate is safely removed.
8. Photograph the full plate before removing swatches.
9. Store and move the plate flat.
10. Open the slicer calibration tab and connect the NH9.
11. Verify the NH9 is in `I+E` mode using `Settings -> Sample mode`.
12. Fix the plate and jig to the table before measuring when the jig can be used.
13. Measure the session references: white backing and black backing.
14. Measure solid physical filament anchors on white backing and black backing.
15. Measure mixed-filament swatches on black backing only.
16. Measure swatches in manifest order, one swatch at a time.
17. Take the configured number of readings per swatch.
18. Remove outlier readings only after the full sample stack is collected.
19. Save Lab values and raw spectrum data.
20. Record backing, lighting condition, printer, nozzle, material family, and any print defects.

## Data Fields To Preserve

Each measurement row should preserve:

- Manifest name
- Swatch ID
- Swatch type
- Filament slots
- Filament names
- Source hex colors
- Source TD values
- Ratio counts
- Percentages
- Effective layer counts
- Effective layer cycle
- Layer height
- Mix mode
- Direct multicolor solver enabled/disabled
- Swatch depth
- Backing condition
- Session reference measurements: white backing and black backing
- Measurement condition for each measurement row, normally white backing or black backing
- Anchor swatches expanded into separate measurement rows for white backing and black backing
- Non-anchor swatches measured on black backing
- Measurement geometry, such as side d/8 for NH9 reflective measurements
- NH9 sample mode, expected to be `I+E`
- Specular/sample mode components captured by the NH9
- Illuminant and CIE observer
- Lab readings
- Averaged Lab
- Raw spectrum
- Instrument serial/model
- Timestamp
- Plate position
- Plate/run ID
- Spool IDs
- Plate handling notes
- Storage duration and conditions
- Notes and defect flags

## Open Questions

Questions to answer from early data:

- Does `opaque_depth` remove backing influence for every material family?
- Is `opaque_depth = max_td + 1 mm` enough, or should it be a larger multiplier?
- Does layer height change color mainly through physical layer cadence, translucency, or surface texture?
- Are normal mode and Local Z comparable at the same nominal layer height?
- Does direct multicolor Local Z reduce visible banding for ternary and four-color mixes?
- Are pair/ternary/four-color results predictable from source colors and TD, or do some pigment combinations require interaction terms?
- Is RGB useful as a calibration set, or is CMY/RYB more representative for subtractive filament mixing?
- Are white and black better treated as source filaments or as backing/reference conditions?
- Should clear/natural filament be treated as a source color, a TD/scattering control, or both?

## Recommended First Run

Do not start with every material set.

First run:

- One high-TD CMY plus white set
- One high-TD CMY plus black set
- One low-TD RYB plus white set
- One low-TD RYB plus black set

For each set:

- Print anchors at the three per-filament TD-derived depths.
- Print the full opaque-depth mix grid for Step 1 only: normal mode at `0.08 mm` and `0.24 mm`.
- Defer Step 2 layer heights and modes until after the Step 1 data has been reviewed.
- Print the depth-model subset only if the first anchor and mix results show that additional depth modeling is needed.
- Measure white backing and black backing for anchors.
- Measure all non-anchor swatches on black backing.

Use this data to decide whether the full matrix should expand to more material families or whether the swatch generator/protocol needs adjustment first.
