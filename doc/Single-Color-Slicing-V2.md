# Single color slicing improvements V2

Measured 20 October 2026 on branch `proposal_UI_changes_GPU_Accelerate_V2`, with retained code through `3de8bfb94f`.

**The unpainted bridge benchmark improved from 67.64 to 58.11 seconds median: 9.53 seconds saved, or 14.1% less elapsed time.** The largest gains came from parallel bridge-direction detection and avoiding gap-fill calculations that cannot emit an extrusion. Aggregate process CPU time fell **6.1%**, and peak private commit fell **155.6 MiB / 4.7%**. These CPU improvements are enabled in normal builds without an additional build flag or preference.

The comparison uses three fresh processes per configuration, the same executable and project, and CPU-only operation. Both configurations include the separate mesh-order correction described below. Full G-code still has small differences between unchanged repeats; the changed geometry operations also have same-input reference checks.

## Workload and comparison

The unchanged `C:\Users\radug\Downloads\1MPolygonBridge.3mf` contains 2,179,126 triangles, 1,087,424 vertices, 737 output layers, one object and one used filament. Its saved settings use 0.12 mm layers, classic walls, 15% gyroid infill, monotonic solid infill and narrow-solid-region detection. Painting, supports, mixed recipes and Local-Z are absent. The mesh has 2,398 open edges; this experiment does not repair the input.

Input SHA-256: `e7f5810db93300928eec92c6f648b731d1c49995266411d62b45a4c770e11f2a`.

The machine is the same Windows system with an Intel Core Ultra 9 285H, 16 logical processors and 64 GB RAM. Slices ran sequentially, with no builds, tests or other benchmark slices running concurrently. Normal desktop applications remained open, and background CPU use was recorded. Clocks were not locked.

- **Reference:** the V2 executable with `ORCA_INFILL_EXPERIMENTS=none`. Earlier retained CPU optimizations and the mesh-order correction remain active.
- **Combined:** the same executable with all four new optimization groups enabled.
- **Timed DLL SHA-256:** `42cb7fd8ac26d476b4cac929cb35496f8cab75e0387a4a3fc864a4d16e5609aa`.
- Profiling and reference validation were disabled during timing. Local-Z clipping, seam visibility, polygon compute and painting-island compute were set to CPU.
- The complete process includes loading, slicing, G-code export and cleanup, but no GUI preview.

| Measurement | Reference runs | Combined runs | Median change |
|---|---|---|---|
| Complete process, seconds | 67.64, 70.62, 66.46 | 58.11, 58.62, 54.83 | **67.64 → 58.11; 14.1% less** |
| Aggregate CPU seconds | 384.55, 406.66, 391.91 | 367.92, 372.42, 361.67 | **391.91 → 367.92; 6.1% less** |
| Peak working set, MiB | 3158.57, 3127.46, 3125.96 | 2974.16, 2988.11, 2998.50 | **3127.46 → 2988.11** |
| Peak private commit, MiB | 3308.78, 3286.32, 3281.39 | 3128.92, 3130.77, 3143.32 | **3286.32 → 3130.77** |

The preserved pre-V2 executable took **66.61 seconds** in an additional run during this session. Its earlier three-run median was **60.58 seconds**. That difference illustrates why the historical and current timing series should not be combined into a cumulative percentage. The additional legacy run is a context check, not a second three-run baseline.

## Elapsed time by stage

Each cell is the median of three runs. Separately calculated stage medians need not sum to the complete-process median. Changes outside the targeted stages are not credited as new improvements.

| Stage | Reference seconds | Combined seconds |
|---|---:|---:|
| Loading and initialization | 4.83 | 4.79 |
| Initial mesh slicing | 2.36 | 2.26 |
| Preparation after mesh slicing | 0.68 | 0.65 |
| Classic walls | 4.97 | 4.65 |
| Curled-extrusion estimation | 2.92 | 2.79 |
| Solid-surface classification | 1.79 | 1.79 |
| Vertical shells | 7.02 | 7.04 |
| External surfaces | **11.24** | **4.71** |
| Bridge over infill | 6.82 | 6.83 |
| Infill and remaining finalization | **12.09** | **8.38** |
| G-code export | 13.27 | 12.79 |
| Cleanup | 1.12 | 1.03 |
| **Complete process** | **67.64** | **58.11** |

External-surface processing improves **58.1%** and infill/finalization **30.7%**. The small export difference has overlapping ranges and does not establish a general export speedup.

## Retained changes

### Skip gap-fill work that cannot produce output

`Fill::_create_gap_fill` previously constructed extrusion-coverage polygons, subtracted and combined them, and sometimes built medial axes before checking whether the result could be emitted.

It now returns early for sparse density below one, bridge extrusion roles, or an empty permitted coverage area. The density predicate preserves the original behavior for NaN as well. Existing gap-fill preferences are still checked first. Full-density, non-bridge gap filling follows the original calculation.

In the diagnostic reference slice, **4,096 discarded calls occupied 41.64 seconds summed across worker calls**, compared with 3.28 seconds for the 1,712 calls whose results could be used. These durations overlap across threads and are neither elapsed slice time nor measured CPU seconds.

The gap-only timing run reduced infill/finalization to **8.78 seconds**. The combined median is **8.38 seconds**, versus **12.09 seconds** for the reference. Validation executes the original discarded calculation and verifies that it appends nothing.

### Prepare only the fill results needed by sparse anchors

Bridge-anchor generation uses only `stInternal` fills. Its old grouping path also prepared solid fills that the consumer subsequently discarded.

The specialized path retains the original ordering and raw masks used to subtract earlier, higher-priority fills. It skips the unused fills' own clipping, solid void extensions and narrow-solid splitting. Final printable infill still uses the full preparation path.

Diagnostic sparse-anchor preparation fell from **5.68 to 2.74 seconds summed across worker calls**. Ordered sparse geometry, parameters and region associations match the complete reference calculation. The enclosing bridge-over-infill median remained **6.82 → 6.83 seconds**; this is retained as a reduction in unnecessary preparation work, not a proven whole-stage speedup.

### Parallelize bridge directions within complex layers

External surfaces already run across layers in parallel. A layer with many bridges can nevertheless hold up the end of that stage while other workers become idle.

For layers with at least **32 bridges**, the new path prepares each bridge's range of source-sorted anchors and computes independent bridge angles with nested TBB parallelism. Each task reads immutable polygons and writes one separate angle. Per-bridge arithmetic and input ordering remain unchanged. The threshold is a dispatch-cost heuristic; smaller layers stay sequential.

The diagnostic maximum duration of a layer's direction call fell from **4.05 to 0.70 seconds**. The direction-only unprofiled run took **4.90 seconds** in the external-surface stage; the three combined runs took **4.71, 4.91 and 4.62 seconds**. Validation recomputes admitted angles serially and requires exact equality.

### Avoid unnecessary flow-variant lookups

Process and printer configurations with at most one declared flow variant always select index zero, whether the requested variant matches or falls back. They now return before inspecting the filament volume type. Filament-domain segment offsets and multiple-variant selection are unchanged.

A separate Release test made two million lookups per process:

| Mode | Three runs, milliseconds | Median |
|---|---|---:|
| Original lookup | 306.547, 300.335, 307.192 | **306.547** |
| Single-variant shortcut | 135.851, 137.001, 136.299 | **136.299** |

That is **55.5% less lookup time**. This isolated result is not an equivalent reduction in G-code export time.

## Mesh ordering and remaining output variation

The ordinary mesh slicer previously appended intersection segments in worker-completion order. Greedy loop reconstruction and open-edge repair could consequently choose different starting segments.

Transient face IDs now restore serial face order before loop reconstruction. Segment coordinates and connectivity calculations are unchanged. This is a correctness change, with additional metadata and sorting, rather than a claimed speed optimization.

- A test with approximately 115,000 faces, open patches, touching geometry and mirrored coordinates matches serial output across eight parallel comparisons. With the old ordering restored as a negative control, all eight comparisons fail.
- Two new real bridge preparation captures match exactly on **all 737 layers**. Earlier captures differed on **58 layers**, including enclosed-area changes.
- Full G-code remains nonidentical between repeats, so this does not establish complete determinism. Across the six primary timing runs, displayed filament consumption spans **82073.24–82073.26 mm**, all report **244.79 g**, and estimates span **26 h 30 m 30–31 s**. Total line counts span 39 lines; that is not a count of changed commands.

The geometry checks for the new optimizations compare original and optimized calculations within the same process on identical inputs. They complement, rather than replace, the full-output comparison.

## Assessed opportunities not implemented

| Candidate | Finding and decision |
|---|---|
| Gyroid period or wave caching | Period construction occupied only **0.033 seconds**, and all wave generation **0.206 seconds**, summed across worker calls. Clipping and connecting cost much more. No cache or GPU prototype added. |
| Old quadratic bridge-grouping implementation | The initially identified implementation is in an inactive `#else` branch. The compiled expansion path already uses union-find grouping. Profiling and the retained parallel change target the active path. |
| Horizontal-shell parallelism | This project uses `ensure_all`; the function took about **0.0001 seconds**. No benefit is available here. Other settings require preserving ordered updates to neighboring layers. |
| Vertical-shell sliding-window reuse | Existing cross-region reuse has no hits on this single-region model. Reordering window Boolean operations would require a separate geometry-equivalence investigation. Deferred. |
| Structured movement pipeline for export | A substantial architectural change spanning stateful filters and text parsing. This round retains the narrow flow-index shortcut instead. |
| Additional Vulkan kernels | The remaining polygon operations require exact reconstruction, while gyroid evaluation has a very small measured ceiling. The useful bridge-direction parallelism works on CPU without transfers or a new GPU option. No additional Vulkan implementation retained. |

## Validation and availability

The Release application and test executable build successfully. The broader focused selection passes **191,060 assertions in 24 cases**; the reference-mode subset passes **57 assertions in three cases**. Coverage includes the density boundary and NaN, bridge roles, gap-fill preferences, empty masks, flow-domain fallbacks and offsets, mesh ordering, and existing CPU/painting preparation regressions.

A bridge slice with the new geometry reference checks and existing export checks enabled completed without a reported mismatch. Validation and profiling runs are excluded from timing.

The final default-enabled build at `3de8bfb94f` also passed the 24-case selection. A fresh bridge slice without performance-control environment variables completed in **56.10 seconds**; this smoke check is separate from the three-run comparison above. Preserved U1, ordinary Starry Night and Local-Z Starry Night projects all completed with the new geometry reference checks and existing export validation enabled. These checks do not establish complete G-code determinism or a measured speedup on those three additional workloads.

The final `Snapmaker_Orca.dll` SHA-256 is `8fd32ead0d105f59b5c9635bf6695b1a5e3ba8fb502b691dfd8c0af73e012e84`. Its build record and the final validation results are saved in `final-build.json` and `final-validation.log` alongside the benchmark evidence.

Normal builds, including the standard Windows release build, enable the retained paths automatically. Developer controls are available for reproduction:

| Control | Purpose |
|---|---|
| `ORCA_INFILL_EXPERIMENTS=none` | Disable these four speedup groups while keeping earlier optimizations and stable mesh ordering |
| `ORCA_INFILL_EXPERIMENTS=gap,anchors,anchor_clip,directions,flow` | Select the retained paths explicitly; equivalent to normal defaults |
| `ORCA_INFILL_VALIDATE=1` | Run available original calculations on the same inputs and check their results |
| `ORCA_PREP_PROFILE=1` | Emit optional stage and worker-call diagnostics |
| `ORCA_MESH_LEGACY_ORDER=1` | Restore old scheduling-dependent segment order for diagnostic comparisons only |

Use fresh processes when changing developer controls. The existing `scripts/benchmark_cpu_slicing.py` runs an isolated slice without changing the source project or the open GUI session. The hidden Catch2 tag `[SingleFlowLookupBenchmark]` reproduces the lookup experiment.

## Commits and review

| Commit | Change |
|---|---|
| `19521f590a` | Restore serial face order before ordinary loop repair |
| `fffad37edb` | Optional surface and infill diagnostics |
| `892c37ebc6` | Skip discarded gap-fill geometry |
| `25e92d4885` | Omit discarded solid-fill preparation from sparse anchors |
| `bda8cb80c9` | Parallel bridge directions within complex layers |
| `3de8bfb94f` | Single-variant flow-index shortcut |

Claude reviewed the proposals and implementations through the CLI conversation **Vulkan GPU geometry acceleration review**, session `d30d83f1-1057-401e-b61d-16c037d2700e`. The review corrected the inactive-code proposal and checked the independence of bridge tasks and the flow-index fallback conditions. The build, tests and measurements provide the verification.

Raw evidence is preserved under `build_logs/single_color_v2/`: `summary.json`, individual `analysis.json` and `environment.json` records, `output_check.json`, the diagnostic profiles, early captures, unit-test logs, lookup measurements, saved baseline runtime and Claude exchanges. The historical comparison remains in [Normal slicing bridge benchmark](Normal-Slicing-Bridge-Benchmark.md).
