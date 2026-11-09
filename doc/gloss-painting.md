# Gloss painting

Select an object and open **Gloss painting** (the sparkle/cube toolbar icon,
shortcut **G**). Select one of four colors, set its surface time, and paint with
the circle, sphere, triangle, or smart-fill tool. Right-click or Shift-click
erases paint. The section slider exposes hidden surfaces. Each object has its
own four timing values; editing a value updates all patches of that color.

Times are **seconds per 100 mm of surface path**. For example, 2 s/100 mm requests
50 mm/s, and 4 s/100 mm requests 25 mm/s. A fixed reference length keeps the
requested speed independent of patch size. These are local surface timings:
one physical layer still has one total duration.

Painting affects outer walls and top solid surfaces. It does not create
extruders, material interfaces, extra walls, temperature commands, or dwell
commands. First layers, bridges, inner walls, support, and ironing retain their
normal settings. Ironing can change the finish over a painted top surface.

The panel shows a nominal range for the selected profile. During G-code export,
each painted path is constrained by its existing surface speed, cooling minimum
speed (or 1 mm/s when unspecified), configured X/Y/E maximum speeds, actual
extrusion cross-section, and filament volumetric flow limit. A maximum always
takes precedence over a conflicting minimum. Overhang handling and cooling may
slow the path further. Existing acceleration and jerk handling remains active,
so short paths may take longer than their nominal timing target. Inspect the
ordinary **Speed** preview for the emitted speed distribution.

The four colors identify timing presets, not predicted gloss levels. The
relationship between speed and appearance depends on filament, temperature,
cooling, geometry, and lighting. Calibrate a small sample with the intended
filament and fixed temperature/fan settings before using the finishes on a
larger part. No physical printer calibration is included with this change.

Gloss masks are independent of filament, support, seam, and fuzzy-skin paint.
They survive undo/redo, model copying, and both project 3MF formats. STL cannot
store paint. Older slicer versions ignore the new gloss attributes. Operations
that replace the mesh reset paint in the same way as the existing paint tools.

Implementation: `gloss_facets` stores states 1–4; `gloss_paint_times` is an
object configuration vector. Slicing creates separate masks without changing
material regions. Export splits surface toolpaths in their original order at
mask boundaries, retaining extrusion properties and interpolating scarf ramps.
Painted segments use the sampled polyline instead of fitted arcs. G-code
comments `GLOSS_COLOR:1` through `GLOSS_COLOR:4` identify painted spans; state 0
returns to normal handling.

Regression coverage is tagged `[GlossPainting]` in `libslic3r_tests` and
`fff_print_tests`: speed limits, ordered clipping including holes and overlaps,
unpainted geometry, model cloning, 3MF round-trips, actual surface feed rates,
palette-only invalidation, and paint erasure.

Validated locally on Windows: the Release app builds; the gloss, triangle
selector, and mixed-filament serialization suites pass (16 cases, 5,002
assertions). The GUI smoke test covers two painted colors, a timing edit,
paint/timing undo, paint redo, project save/reload, and the bounded timing
slider. Physical printer validation remains outstanding.

Manual validation: paint adjacent patches on a cube and a curved model, check
undo/redo and project reload, inspect Speed preview at boundaries, and print a
sample using the intended filament. Repeat with cooling, scarf seams, and
multiple filaments before relying on the finish for production.
