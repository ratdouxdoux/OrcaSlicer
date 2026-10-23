# SML GPU acceleration after the CPU path-reuse improvements

**Follow-up:** Vulkan slicing acceleration was subsequently removed from this branch. This report records the pre-removal comparison; see `GPU-Slicing-Removal.md` for current build behavior.

The current packaged V2 build slices `CubeSML.3mf` faster with SML GPU acceleration disabled. Across five fresh-process runs per setting, the slicing/export pipeline takes **1.243 s with GPU off versus 1.449 s with it on** (16.6% longer). A second, much more complex Local-Z project shows slower Local-Z preparation and G-code export with GPU enabled, but essentially tied total slicing time. These results do not establish that Vulkan is slower for every model.

## Method and build

Tested on 23 October 2026 in `proposal_UI_changes_GPU_Accelerate_V2`, repository HEAD `f3d1802772`, using the existing packaged executable `build/Snapmaker_Orca/snapmaker-orca.exe`. Its `Snapmaker_Orca.dll` SHA-256 is `9fa50025fd5f645fc3d5304eb1be7a2768763be9356c959b4a38f631ef300feb`. The binary was not rebuilt for this comparison. All accelerated runs report **NVIDIA GeForce RTX 5060 Laptop GPU**, with zero fallback layers.

Each timed run uses a fresh hidden CLI process and an isolated data/output directory. Five runs per configuration alternate CPU/GPU order, reversing that order on even repetitions. No benchmark slices run concurrently. Existing user applications stay open; this is not an idle-machine laboratory measurement. SML itself remains enabled in both conditions. Existing CPU caching, spatial indexing and whole-path reuse remain enabled in both conditions.

The principal comparison varies only `ORCA_LOCAL_Z_CLIP=cpu` versus `vulkan`. This selects the same backend as the Preferences SML/Local-Z GPU checkbox. Seam visibility stays on CPU for these runs, as do the separate experimental polygon and painted-island backends. Validation, clipping capture and detailed CPU instrumentation are disabled during the timed series.

The main timing is the interval from `Starting the slicing process.` through `Exporting G-code finished`. It includes CPU slicing and G-code generation/processing, but excludes project loading, process startup/shutdown and GUI preview construction. Whole-process times have substantial startup variation and a 0.5 s polling resolution; they are unsuitable for measuring the cube's approximately 0.2 s difference. These are CLI engine measurements, not stopwatch timings of the open GUI.

Inputs:

- `C:\Users\radug\OneDrive\Documents\CubeSML.3mf`, SHA-256 `c0bbdd62899fa4052fc3530f7378f403fb837a3ed8d9f4f6f558a2b6db729275`.
- `C:\Users\radug\Downloads\打印出问题的3mf\开启混色细分层高导致局部层高过小.3mf`, referred to below as **terrain**, SHA-256 `0747970025e0dbc758023833e51173c42395c82966ece97f44445b62e64e2f63`.
- An isolated copy of the cube with `fs_surface_paint_only=1` and `fs_painted_zone_extra_perimeters=2`, referred to as **cube with UI settings**. Those two effective settings were found in the completed output owned by the user's currently open packaged instance; the saved original has them disabled. The copy's SHA-256 is `a58f634505c22a93e98715b8fe537a839b31383b769c571694971e92f40de0c8`. Other effective differences between the original CLI output and the GUI output were configuration provenance fields. Original projects and user preferences were not modified.

## Timings

Seconds; medians of five runs. Positive change means enabling the SML GPU option is slower.

| Project / operation | GPU off | GPU on | Change |
| --- | ---: | ---: | ---: |
| Saved cube: complete slicing/export | 1.243 | 1.449 | +16.6% |
| Saved cube: G-code export | 1.039 | 1.237 | +19.0% |
| Saved cube: Local-Z path preparation | 0.031 | 0.239 | +0.208 s |
| Cube with UI settings: complete slicing/export | 1.198 | 1.359 | +13.4% |
| Cube with UI settings: G-code export | 1.002 | 1.176 | +17.3% |
| Cube with UI settings: Local-Z path preparation | 0.023 | 0.217 | +0.194 s |
| Terrain: complete slicing/export | 15.330 | 15.186 | -0.9%; effectively tied |
| Terrain: G-code export | 3.518 | 3.746 | +6.5% |
| Terrain: Local-Z path preparation | 0.658 | 0.918 | +0.259 s |

Complete-pipeline ranges: saved cube **1.209–1.262 s CPU / 1.377–1.719 s GPU**; cube with UI settings **1.176–1.207 / 1.342–1.385 s**; terrain **14.901–15.640 / 14.879–15.201 s**. Terrain's GPU runs happened to have faster pre-export CPU work (median 11.418 versus 11.864 s), where this GPU option does not operate. That variation masks its export overhead; the total-time result is not evidence of GPU acceleration of mesh slicing.

Local-Z path-preparation time is the sum of the context-to-clipping-summary log intervals. It is a subset of export, not an additional stage to add to the total. Stage medians need not sum to the median total.

The user's saved preferences have the independent seam GPU option enabled. A further three alternating runs per SML setting hold **seam GPU on** and use the cube's observed UI process settings. Complete-pipeline medians are **1.292 s SML GPU off / 1.392 s on (+7.7%)**; export is **1.102 / 1.204 s (+9.2%)**. The corresponding Local-Z preparation is **0.019 / 0.155 s**. SML GPU initialization is cheaper after seam GPU work, but enabling it still loses. Thus the direction reproduces with that preference too; the repeatable engine difference is about **0.1–0.2 s**, not a demonstrated multi-second GUI delay.

## Why the GPU loses here

The Local-Z Vulkan path preclassifies extrusion/mask combinations that will clip to nothing. It does not run the complete clipping algorithm or generate toolpaths on the GPU. CPU setup gathers mask edges and path segments, uploads them, dispatches a batch, reads decisions back and skips proven-empty combinations. This preparation currently happens before the CPU path router handles the remaining paths.

The newer CPU implementation already uses spatial indexing, caching, fast empty-result rejection and direct reuse of paths wholly inside their region. Consequently, much of the expensive clipping that made the early Vulkan experiment attractive is gone. The GPU still packs and tests paths even when the CPU can now handle them cheaply.

For the saved cube, the GPU processes approximately **258,284 segment queries over 265 layers**, rejecting about **4,832 extrusion/mask combinations**. Median GPU preparation costs **204 ms**, including approximately **101 ms backend setup**, **47 ms upload**, and **45 ms dispatch/readback**. Component medians are independent and need not sum exactly. A separate CPU diagnostic run spends only **3.68 ms in the remaining intersection/difference calls**, of which **3.51 ms returns empty**. Its complete Local-Z preparation, including other work, is approximately 31 ms. There is little expensive clipping left for Vulkan to avoid.

Terrain has larger batches: approximately **768,951 segment queries over 21 layers**, with **59,957 rejected combinations**. GPU preparation still costs a median **389 ms**, and export is consistently slower across the measured ranges. Rejection count alone is therefore not an adequate criterion for deciding whether to offload.

Performance recommendation: leave this particular SML GPU option disabled for these workloads. This says nothing about the independent seam-visibility GPU option. A future adaptive implementation should measure the benefit after CPU filtering and reuse, skip cheap batches, and reduce backend/setup and synchronous transfer costs. Simply enabling GPU work for every eligible SML layer is no longer a reliable speedup.

## Output and validation checks

Normal CPU and GPU runs complete successfully with the same effective configurations, tool sequences and nominal layer sequences. Outputs are not byte-identical: unchanged CPU repeats also vary. Three audited comparisons per project include a CPU/CPU repeat control and two CPU/GPU comparisons, with physical-Z/material/role/height-aware centerline checks at 0.01 mm.

- Saved cube: the CPU repeat has about 737 mm missing / 743 mm added centerline coverage, mainly outer walls, out of about 562,037 mm total extrusion path. CPU/GPU comparisons are of the same order (roughly 741–746 mm in either direction). Total deposited filament-length differences are below 0.08 mm out of approximately 9,159 mm. These are differences in emitted paths, not a claim of removed model volume.
- Cube with UI settings: CPU-repeat and CPU/GPU centerline differences are approximately 734–741 mm out of 532,120 mm total path; total deposited filament-length differences are below 0.07 mm out of approximately 8,730 mm.
- Terrain: CPU-repeat centerline differences are 0.044 / 0.234 mm; CPU/GPU differences are below 0.36 mm in either direction, confined to internal solid infill in these comparisons. Total deposited filament-length differences are below 0.009 mm out of approximately 14,427 mm.

The same-mode variation prevents claiming exact output identity from these independent runs. GPU rejection validation also recomputes rejected paths through the CPU router in the same run, avoiding that comparison problem.

The saved cube and terrain both pass combined GPU rejection validation and full-mask CPU reference validation. Terrain's diagnostic check takes approximately 380 seconds because it deliberately performs the expensive reference geometry repeatedly; that duration is excluded from all performance tables. The GPU validates 4,827 rejected combinations on the saved cube and 59,957 on terrain, with no incorrect rejection reported. The cube with UI settings separately passes GPU-only rejection validation for 4,443 rejected combinations.

**Separate unresolved finding:** enabling the stronger full-mask reference checks on the cube with UI settings fails with `Local-Z whole-path ownership differs from reference clipping`, at the logged layer around Z=13.8 mm. It reproduces in two CPU-only runs as well as the combined GPU/reference run. GPU-only rejection validation passes. Disabling whole-path reuse while keeping full-mask reference validation enabled passes. This localizes the mismatch to the CPU whole-path reuse check under those settings; it is independent of the SML GPU performance regression. The exact boundary geometry and physical significance have not been established, so this report does not certify equivalence for that variant.

## Evidence and reproduction

Detailed run logs, isolated inputs and orchestration scripts are in `build_logs/sml_gpu_ab_20261023/`. The companion JSON retains compact timing, GPU-counter and validation evidence. Existing `scripts/benchmark_cpu_slicing.py`, `scripts/compare_cpu_slicing.py` and `scripts/audit_slicing_gcode.py` perform process measurement, stage extraction and G-code comparison.

For a single comparable run, start a clean child environment without inherited `ORCA_*` overrides, set `ORCA_LOCAL_Z_CLIP` to `cpu` or `vulkan`, and set `ORCA_SEAM_VISIBILITY=cpu`, `ORCA_POLYGON_COMPUTE=cpu`, `ORCA_PAINT_ISLANDS=cpu`. Run `scripts/benchmark_cpu_slicing.py` with the packaged executable, original project, a fresh output directory, `--debug 4 --timeout 900`. Repeat in alternating order. Do not include `ORCA_LOCAL_Z_VALIDATE`, `ORCA_LOCAL_Z_PROFILE`, capture flags or `ORCA_PREP_PROFILE` in performance measurements.

No production implementation or preference defaults were changed during this investigation.
