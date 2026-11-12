# SkyLane prime tower

SkyLane is an experimental prime tower for tool-changing printers. It gives each
tool somewhere to extrude and establish flow before returning to the model.
Different parts of the tower can grow at different heights, so it can fit purge
material into the available space below the current model layer. This is useful
when tool changes are frequent or unevenly distributed through the print.

This guide describes the implementation in this repository, on branch
`codex/skylane-toggle`, its preview behaviour and its relationship to the
standalone SkyLane source pack supplied in `SKYLANE_PACK_RADU_2.zip`.

To enable it:

1. Select a supported tool-changing printer and switch to Advanced settings.
2. Under **Process → Multimaterial → Prime tower**, enable the prime tower and
   **SkyLane tower (experimental)**.
3. Leave **No sparse layers (beta)** and **tower interface features** disabled.
4. Choose Rectangle, Cone or Rib walls, then slice the plate.

The toggle is saved in the process preset and project. Existing presets leave
SkyLane off. The current integration supports tool changers, including planned
Local-Z tool changes. Single-extruder multimaterial and Bambu-specific towers
are not supported.

**The tower is divided into lanes along its depth.** A lane is a region used by
the planner, not necessarily a separate compartment surrounded by walls. Each
lane tracks its own deposited height. During busy stretches, SkyLane places
purge material in a low lane and raises it towards the current printing height.
One purge request can occupy more than one pass.

For example, if the model is at Z = 10.0 mm and a lane surface is at 9.7 mm,
there is 0.3 mm of height available there. Subject to the configured dip and
pass-height limits, the tower can use that space for a purge. Its deposition
height does not have to match the thickness of the current model layer.

**Mixed materials get fixed purge regions.** SkyLane uses the adhesion category
in each filament preset. For example, with two Snapmaker PLA colours and two
Snapmaker PETG colours, both PLA tools purge in one region and both PETG tools
purge in another. This applies to every adhesion category used on the plate.
For example, the Snapmaker ABS and ASA presets share a category, while PA and
PC each have their own. Those regions keep their positions for the life of the tower.
At a change from PLA to PETG, outgoing ramming takes place on the PLA side and
incoming priming on the PETG side. Within each region, colours can share a
deposition level. If the tool-change sequence visits a lower sublayer, SkyLane
can use independent lanes inside those same boundaries. The brim and outer
fence serve the whole tower. On a partitioned tower, the outer fence can span
several thin model layers in one pass. Its step height follows the nozzle and
dip limits and gets smaller where ribs or cone walls taper steeply. If a material is absent for several layers and
will be needed again, the active tool can lay sparse support in its region.

With one adhesion category, the planner considers tool changes in their
printing order. It tries two through six lanes, searches for the required
depth, and replans with a 15% depth reserve. Among candidates within 3% of
the shallowest depth, it uses
estimated tower time and requires a 2% improvement to choose more lanes.
There is no manual lane-count setting. The configured tower
width stays fixed; depth is calculated from the schedule, purge volumes,
spacing and height limits. The body is **rectangular, with no requirement for
equal width and depth**. Brims, ribs and cone outlines add to its footprint.

**Quiet stretches in a single-category tower can use one shared cell.** When
independent lanes are not needed, their heights are brought together and
purges occupy neighbouring strips within a shared round. A round is a tower deposition level; it is not
necessarily one model layer or one tool change. The round has a shared outer
wall. Unused space is closed with open grid, and the tower can split into lanes
again when the schedule requires it.

SkyLane adds structural fill when a lane falls too far behind the model or
when regions need support before the next round. The foundation is solid and
the brim follows its outline. Taller neighbouring lanes also reduce the width
available for a lower pass, leaving clearance for the nozzle.

On a first layer with no tool change, a shared tower can start with a thin
grid and outline. For example, with a 0.25 mm first layer and 0.20 mm reserved
for upcoming purges, the grid prints directly on the bed at Z = 0.05 mm. On
the next visit the tower completes its base to 0.25 mm. The model still prints
its first layer at the configured height.

The three wall types use the same lane-planning approach:

| Wall type | Shape and behaviour |
|---|---|
| **Rectangle** | A rectangular shared cell. Its depth can step inward, centred on the original footprint, when the remaining schedule fits in a smaller area. |
| **Cone** | Adds the conventional tower's stabilization arcs on the long sides. The arcs shrink with height and follow reductions in cell depth. A low tower with a small cone angle can have the same outline as Rectangle. |
| **Rib** | Adds four arms at approximately 45° from the corners. Extra arm length decreases with height, and Fillet wall rounds the contour. The central cell retains its full planned depth. The whole outline of each shared tower level uses one active filament. |

Rectangle and Cone only shrink after the planner checks the remaining
requests with the same reserve used for sizing. The tapered cell stays at
least 8 mm deep and at least 4 mm per planned lane. Rib does not use this depth reduction. The current geometry has no
extra inner corner frames: the outer rib contour supplies the perimeter.

The main settings affect the tower as follows:

| Setting | Effect in this build |
|---|---|
| **Width** | Sets the rectangular cell width. Depth is planned independently. |
| **Prime volume** | The standalone pack uses this volume for every tool change, plus applicable outgoing-tool ramming. It replaces the previous adaptation's use of individual planner purge volumes. |
| **SkyLane maximum dip** | Limits how far the lane surface may lag below the current printing height. A larger allowance can provide more room for purges, subject to pass-height and clearance limits. |
| **SkyLane flow ramp volume** | Defaults to 4 mm³. Over this extruded volume, the new tool's volumetric flow ceiling rises from 25% to 100% of its filament limit. Zero disables the ramp. Other speed limits still apply. |
| **Purge lines spacing** | Spreads purge rows farther apart as the percentage increases. The allocated purge volume is preserved, so wider spacing generally requires more room. |
| **SkyLane fence height** | Maximum height of the separate fence around lanes growing from the bed. Defaults to 1000 mm; zero disables it. It needs enough brim underneath and stops at the first merged zone. |
| **SkyLane profile speeds** | Enabled by default. Uses filament flow limits, tower speed, initial-layer speed and the ramp. Disabled: fixed limits of 15 mm³/s and 250 mm/s for purge, 30 mm/s on the first layer, without a ramp. |
| **Brim width** | Extends the foundation around the wall outline. |
| **Wall gap** | Omits a short part of the shared perimeter near the entry point. It does not connect or separate adjacent purge rows. |
| **Rib length, rib width and Fillet wall** | Control the rib outline when Rib is selected. |
| **Stabilization cone apex angle** | Controls the cone outline when Cone is selected. |

**Maximum dip = 0 means automatic, not zero movement.** In this build it uses
the largest configured Z hop, with a minimum of 75% of the smallest nozzle
diameter. An explicit dip also limits the maximum pass height. The planner uses
a simplified nozzle-clearance envelope, including a 40° cone and at least a
0.5 mm tip radius. This is not a model of the entire toolhead or printer.
When a rib corner is already higher than the target lane, the nozzle lands
farther inside the lane if needed. The purge rows also stop short of the
printed corner. A deeper dip needs more room for the wider part of the nozzle.

The ramp controls a flow ceiling, not a promise that every move will reach a
particular speed. Maximum tower speed, initial-layer speed, filament volumetric
limits and applicable ramming limits still constrain the emitted moves. These
profile-derived limits apply with Profile speeds enabled. Where the wall has separate
side sections, the final purge row can carry the nozzle between them, avoiding an
extra trip across the tower. This rearrangement waits until the incoming flow ramp
is complete; short purge bands keep their wall after the rows. Preceding grid extrusion
counts towards priming, but the grid itself is not slowed by the purge ramp.

Several features of the preview are intentional:

| What you see | Explanation |
|---|---|
| Parallel purge rows with open ends | The nozzle moves between rows without extruding. Extruded U-turn connections are not part of the SkyLane path. |
| Adjacent colour bands | Outgoing ramming and incoming purge have separate bands of complete rows. Rectangle and Cone wall sections use the adjacent band's material. Rib uses one colour for the whole outline at each shared level. No empty separator is required. |
| A short break in an otherwise continuous outer wall | This is the optional Wall gap. |
| Open regions inside a larger tower outline | Lanes can be at different heights, unused regions can contain sparse grid, and the footprint must accommodate busier parts of the schedule. Inspect surrounding heights and moves to distinguish these cases. |
| A tower level that differs from the selected model layer | SkyLane uses its own deposition heights below the current model height. |
| A smaller upper rectangular cell | Rectangle and Cone can reduce cell depth once the remaining schedule allows it. |

**Extruded U-turns between rows are not required by the intended design.**
The original C++ source explicitly moves to the next row with a `G1 X...` or
`G1 Y...` command that has no extrusion value. Our emitter likewise uses a
travel move. The structure relies on deposited walls and support grid,
rather than a continuous extruded zigzag joining every row. Adding extruded
turns would add plastic and require adjusting the volume calculation.
Their absence in the preview is not, by itself, a missing-path defect.

**Touching colour transitions do not mean the printer skips the tool change.**
Ramming is material deposited by the outgoing tool before it is parked. At
eligible visits, SkyLane reserves a separate band for that material before the incoming
purge. For example, 5 mm³ of ramming followed by a 38 mm³ purge still requires
43 mm³ of purge-path material, apart from additional structure. The outgoing
tool completes its rows and assigned wall paths, the printer performs its
tool-change sequence, and the incoming tool starts its own band. A tool change
does not split a row between colours. The actual docking motion is determined
by the printer profile's tool-change macros.

On shared levels, deposits of the same material stay together. The first
material grows from one end of the level and the other bands from the opposite
end, so the first colour can return beside its earlier deposit. If a later
return cannot fit beside its own colour, the remaining space is closed using
the outgoing material and a new deposition level begins. No permanent tower is assigned to a colour.

With **Rib**, the four arms and their connecting outer wall form one outline.
That outline is printed once per shared tower level, using the filament already
active when the level starts. Its colour can change at the next height, but
does not change around the same outline. This adds no tool changes and keeps
the interior purge bands separate. Independent-lane support sections retain
their staged construction for nozzle clearance.

Independent lanes retain their separate support heights and nozzle-clearance
requirements. The closure can add support material, so removing wall detours
does not guarantee a shorter total print time.

Within a visit, the next band is entered from a nearby corner. A move directly
from the shared boundary into an unprinted band at the same height can stay
level when no taller tower structure is present. Other band entries retain
the clearance lift, as does departure from the tower.

For the supplied four-colour cube, the single-colour Rib outline reduces the
slicer's estimate from **2h17m38s to 2h16m06s**, a further 1m32s saving after
the travel optimization. The original ungrouped version was **2h16m22s**.
The revision preserves the 510 tool changes, 18 mm³ incoming purge per change
and interior colour-band allocation. Tower XY travel falls from 61.5 m to
59.1 m. Total estimated material is 23.26 g; assigning the outline to different
filaments also changes the mass calculation because their profile densities
differ. These are slicer estimates from the same project and profiles, not
measured printer timings.

**Wall gap is a separate feature.** It opens 2.5 line widths in the shared
perimeter near the entry position. At a line width of 0.5 mm, the opening is
1.25 mm long along the perimeter. Its location moves between rounds, and on
a ribbed tower it can fall on an arm or rounded segment. The original guide
describes it as keeping the first incoming plastic off that part of the wall.
SkyLane emits the opening without the conventional tower's additional slow
nozzle wipe. It is not a gap between different filaments or between every
pair of purge lines.

**No sparse layers must remain off in this integration.** SkyLane already
schedules its own structural support and lets lane heights lag within the dip
limit. It cannot combine that schedule with the conventional tower's
No sparse layers mode. Turning that option off does not make SkyLane solid:
its own open grid remains part of the design. Tower interface features must
also remain off because this emitter does not implement their material
interface temperature treatment. Incompatible enabled options stay visible
so they can be disabled; slicing rejects unsupported combinations.

Framework, brim chamfer, wall-filament selection, bridging distance and extra-flow controls are
hidden while SkyLane is active. Tower rotation is supported. The G-code path
lifts before crossing lanes and leaves the nozzle above the tower. The next
object extrusion lowers it after travelling away. With ramming disabled,
non-flexible outgoing tools can arrive retracted and wipe during the
tool-change retraction. Profile macros remain responsible for docking.

SkyLane is intended to use tower space efficiently when tool changes are
uneven or occur at closely spaced heights. A smaller footprint or lower
material consumption is not guaranteed for every model. Walls, support, ribs
and the brim also consume material. Performance figures in the original
author's guide describe that author's builds and test plates; they are not
benchmarks of this integration.

The implementation now imports the standalone planner and emitter supplied in
`SKYLANE_PACK_RADU_2.zip`, replacing the earlier reconstruction based on the
HTML demonstration. The original algorithm handles lane scheduling, selection
by depth and estimated time, tapering, rib roots and outer supports, cone
contours, grid transitions, wall gaps, ramming and the volume-based purge ramp.
It does not require NeoTower.

The integration is adapted to this fork's existing tool-change system. Stock
WipeTower2 blocks provide the tool-change sequence; SkyLane replaces their
purge geometry at export time. This fork stores outgoing-tool unloading in a
separate block, so that old path is removed when SkyLane supplies its own
ramming. Planned paths are replayed to generate the Prepare-view contours,
material estimate and collision bounds, and the runtime state is reset before
each export.

The existing preset keys `enable_skylane`, `skylane_max_dip` and
`skylane_ramp_volume` remain compatible. The added keys are
`skylane_wall_height` and `skylane_profile_speeds`.

Some adaptations are deliberate. Automatic dip retains this fork's largest-Z-hop
rule, and nozzle sizes constrain pass height and clearance for mixed-nozzle
setups. Impossible placements stop export instead of allowing partial purges
or silently falling back to the stock tower. Purge-line flow correction is
included when enforcing the flow ceiling; brim, fence and ramming paths also
respect their applicable flow limits. The simplified nozzle envelope is
still not a complete toolhead collision model.

Regression coverage includes the supplied Dragon sequence, all three wall
types, lane/merged transitions, purge volume and flow limits, wall timing during
the ramp, repeat exports, real tool selections and FullSpectrum Local-Z output
with rotated towers, plate offsets and tool offsets. The 8 November 2026
Windows Release build passes **10 targeted test cases**, including the
510-change four-colour schedule with both equal and asymmetric ramming volumes,
an internal travel-distance limit, checks on level entries into adjacent bands,
and a complete single-colour Rib outline at every shared level. The supplied
cube's exported G-code has 130 rib-outline levels, each using exactly one filament.
The supplied cube was also re-sliced before and after the routing change:
model extrusion paths and speeds, tool order, band schedule and required purge
amounts are preserved. Physical printing has not yet been validated for this adapted build.

The material-partition regression uses two PLA and two PETG colours. It checks
the G-code coordinates and volumes of outgoing ramming and incoming priming
across the full tower height, including a lower sublayer and a material that
disappears and returns.

The relevant implementation files are `src/libslic3r/GCode/SkyLanePacker.cpp`
for planning, `src/libslic3r/GCode/SkyLaneTower.cpp` for geometry and emission,
and `src/libslic3r/Print.cpp` / `src/libslic3r/GCode.cpp` for the integration. Regression coverage
is in `tests/libslic3r/test_skylane.cpp` and
`tests/libslic3r/test_product_port.cpp`. The targeted test command is:

```text
libslic3r_tests "[SkyLane],[WipeTower]" --reporter compact
```
