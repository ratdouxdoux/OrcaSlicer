# SkyLane prime tower

Enable **Process → Multimaterial → Prime tower → SkyLane tower (experimental)** in
Advanced mode. Prime tower must also be enabled. The setting is saved with the
process preset and the project; existing presets default to the conventional tower.

This implementation adapts the lane scheduler in the supplied October 2026
`TourHTML.zip/skylane.html`, which describes itself as a JavaScript port of
Neotko's SkyLane packer. The subsequently supplied `SKYLANE_PACK_RADU.zip` includes
the original C++ source, user guide and integration notes. Those were reviewed
against this implementation. This remains a separate WipeTower2 integration and
G-code emitter, not a full port of that source pack: the original depends on
NeoTower, and its proposed standalone integration is not implemented in the pack.

The planner tries two through six lanes, searches for the smallest depth that
holds the chronological purge requests, then adds 15% depth reserve. Lanes grow
independently below the current model layer. Support brings lagging lanes within
the maximum dip; quiet stretches can share a merged cell. Neighbouring taller
lanes reduce the printable band to leave nozzle clearance. The bottom is solid.
Purge spacing determines the packing density. Ramming and the incoming purge
share one allocation; the incoming tool continues the outgoing tool's path.
Each merged round has one outer wall, rather than a frame around every strip.
Unused areas receive open grid, and the brim follows the solid foundation.
Adjacent purge rows are joined by travel moves without extrusion. The source
pack's `skylane/SkyLaneTower.cpp` likewise emits `G1 X...` or `G1 Y...` without
an E value between rows; the walls and crossed grid support the separate lines.
Open row ends in the preview are therefore intentional. Extruded U-turns would
add volume and require a corresponding change to the purge calculation.

- **Maximum dip:** zero derives the value from the largest configured Z hop, with
  a floor of 75% of the smallest nozzle diameter. The current nozzle envelope uses
  a 40° cone and at least a 0.5 mm tip radius. It is not a machine collision model.
- **Flow ramp volume:** defaults to 4 mm³, ramping the volumetric limit from 25%
  to 100%. Zero disables the ramp. Tower speed, first-layer speed and filament
  volumetric limits still apply. Multi-tool ramming volume and flow are respected.
- Width, brim width, purge volume, purge spacing, bridging distance, wall gap and
  tower rotation are supported. Wall gap opens a 2.5-line-width break in the
  shared perimeter near the entry point, including across filleted segments.
  Local-Z tool changes use the same preplanned lane schedule.
- **Rectangle:** merged cells step inward, centred, when the remaining requests
  fit in a smaller footprint. Each reduction is checked against the entire
  remaining schedule.
- **Cone:** adds the stock stabilization arcs to the long sides, shrinking with
  height. Low towers with a small apex angle can have the same outline as
  Rectangle. The merged cell can also taper.
- **Rib:** retains the full cell depth and adds four 45-degree arms. Extra rib
  length shrinks with height; rib width and Fillet wall control the contour.

All wall types retain the configured cell width and plan the required depth.
The temporary square-sizing change and added inner corner frames have been
removed. The source uses the outer rib contour without those inner frames.

The implementation supports **tool changers** and all three wall types above.
Framework, brim chamfer, wall-filament selection and extra-flow controls remain
hidden while SkyLane is active. Disable **No sparse
layers** and **tower interface features** before slicing; SkyLane manages its own
support and does not implement material-interface temperature treatments. A
schedule mismatch stops export instead of silently skipping required support or
purging. Single-extruder multimaterial and Bambu-specific towers are unsupported.

The new G-code path lifts before crossing lanes, uses explicit pass heights,
parks outside the brim for tool-change macros, and restores the object height
before resuming. Profile macros remain responsible for the printer's docking
motion. This feature has automated packing and export tests; it has not been
validated on a physical printer. Review the preview and nozzle clearance before
printing. The supplied speed-test files are references, not hardware validation.

Targeted tests: `libslic3r_tests "[SkyLane]"` and
`libslic3r_tests "[ProductPort][LocalZ][GCode][WipeTower]"`.

The archive regression uses the 395-change sequence from Dragon D2/D4/D6 in
`SKYLANE_SPEED_TEST_2026-10-06`. It checks the 28.8997 mm planned depth, shared
wall coordinates, purge-strip endpoints, rounded rib extent, and 11.5599 mm
upper rectangular cell. A separate larger-angle case exercises visible cone
arcs. Export tests cover every wall type with Local-Z, rotated towers, plate
offsets and tool offsets. This verifies geometry and integration, not identical
G-code ordering, slicing times or physical print quality.
