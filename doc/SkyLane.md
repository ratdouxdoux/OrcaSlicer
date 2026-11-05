# SkyLane prime tower

Enable **Process → Multimaterial → Prime tower → SkyLane tower (experimental)** in
Advanced mode. Prime tower must also be enabled. The setting is saved with the
process preset and the project; existing presets default to the conventional tower.

This implementation adapts the lane scheduler in the supplied October 2026
`TourHTML.zip/skylane.html`, which describes itself as a JavaScript port of
Neotko's SkyLane packer. It is a new integration and G-code emitter for this fork,
not a copy of the unavailable private upstream C++ implementation.

The planner tries two through six lanes, searches for the smallest depth that
holds the chronological purge requests, then adds 15% depth reserve. Lanes grow
independently below the current model layer. Support brings lagging lanes within
the maximum dip; quiet stretches can share a merged cell. Neighbouring taller
lanes reduce the printable band to leave nozzle clearance. The bottom is solid.
The emitter reserves additional space for oval extrusion cross-sections and lane
perimeters, so dimensions and times will differ from the supplied demonstration.

- **Maximum dip:** zero derives the value from the largest configured Z hop, with
  a floor of 75% of the smallest nozzle diameter. The current nozzle envelope uses
  a 40° cone and at least a 0.5 mm tip radius. It is not a machine collision model.
- **Flow ramp volume:** defaults to 4 mm³, ramping the volumetric limit from 25%
  to 100%. Zero disables the ramp. Tower speed, first-layer speed and filament
  volumetric limits still apply. Multi-tool ramming volume and flow are respected.
- Width, brim width, purge volume, purge spacing, bridging distance, wall gap and
  tower rotation are supported. Wall gap opens a 2.5-line-width break in each
  lane perimeter. Local-Z tool changes use the same preplanned lane schedule.

The current implementation supports **tool changers and rectangular towers**.
The classic cone, ribs, fillet, framework, brim chamfer, wall-filament selection
and extra-flow controls are hidden while SkyLane is active. Disable **No sparse
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
