# Actual G-code path comparisons

These figures use the preserved `1MPolygonBridge.3mf` outputs from the 20 October 2026 CPU-only comparison. Each pair compares repeated runs of **the same executable**, with identical saved settings. Orange and blue identify runs, not filament colors.

| Figure | Compared runs | Printed layer / Z | Highlighted role |
|---|---|---|---|
| `original-layer-290-overview.png` | Before 1 / before 2 | 290 / 34.88 mm | Internal solid infill |
| `original-layer-290-detail.png` | Before 1 / before 2 | 290 / 34.88 mm | Internal solid infill |
| `current-layer-331-detail.png` | After 1 / after 2 | 331 / 39.80 mm | Internal bridge |
| `current-layer-323-detail.png` | After 1 / after 2 | 323 / 38.84 mm | Internal solid infill |

The left and middle detail panels draw all positive-extrusion XY centerlines for the selected role. Other extrusion paths provide faint context. The right panel compares spatial paths: orange occurs only in the first run, blue only in the second, and gray is coincident within 0.002 mm. Path order, segment subdivision, extrusion amount and feed rate do not determine spatial coincidence. Travel moves are omitted. Line thickness is for legibility, not an estimate of deposited bead width.

Layer 290 has approximately **275.03 mm of additional internal-solid-infill centerline** in original run 2, across the entire layer. None of run 1's selected-role centerline is unique beyond the overlay tolerance. The detail shows part of that difference; the overview locates the crop. This does not establish which output is correct.

Current layer 331 shows a shifted bridge pattern despite both runs having 174 extrusion commands in that role. Current layer 323 shows a small solid-infill loop present only in run 1. These are selected examples of spatial differences; they are not a representation of every changed layer.

Reproduce from the repository root:

```powershell
python -B scripts/render_bridge_gcode_variation.py
```

The renderer requires NumPy, Matplotlib and Shapely. It reads the ignored evidence in `build_logs/bridge_normal_20261020/`, verifies each source G-code SHA-256, and checks selected-layer role counts and positive E totals against the prior independent extraction. The saved files use linear motion, absolute XY and relative E; unsupported arcs or coordinate modes fail explicitly. No source G-code or project is modified. `manifest.json` records source hashes, crops and overlay measurements.
