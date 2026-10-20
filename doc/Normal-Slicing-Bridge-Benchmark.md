# Normal slicing: 1MPolygonBridge before and after

Measured 20 October 2026. This compares the original pre-slicing-optimization build with the final retained CPU improvements, using the user's unmodified `1MPolygonBridge.3mf`. Both configurations use CPU-only slicing.

**The complete CLI median fell from 62.59 to 60.58 seconds: 2.01 seconds / 3.2% less elapsed time.** Classic wall generation fell **6.06 → 4.02 seconds (33.6%)**, and G-code export fell **12.90 → 11.83 seconds (8.3%)**. Aggregate CPU work fell **6.2%**. Peak working set and private commit were essentially unchanged.

This is a modest observed whole-job improvement on an unpainted, single-material workload. It does not reproduce Starry Night's much larger gains: this model does not exercise painting, mixed-filament tracking, Local-Z or tree-support generation. **Complete G-code equivalence did not pass, including between repeats of the unchanged baseline.** Treat the timings as a repeated comparison across builds with the output limitations described below, not a proven identical-output speedup.

## Input and configuration

| Item | Value |
|---|---|
| Original input | `C:\Users\radug\Downloads\1MPolygonBridge.3mf` |
| Input size | 43,493,855 bytes |
| SHA-256 | `e7f5810db93300928eec92c6f648b731d1c49995266411d62b45a4c770e11f2a` |
| Mesh | 2,179,126 triangles; 1,087,424 vertices |
| Objects and instances | One object, one instance, plate 1 |
| Painting | No triangle painting attributes |
| Printer and process | Snapmaker U1, 0.4 mm nozzle; saved 0.12 mm Standard process |
| Walls and seam | Classic walls; aligned seam |
| Material | Snapmaker PLA SnapSpeed; two configured slots, only filament 1 used |
| Support generation | Disabled |
| Local-Z and mixed recipes | Global and gradient modes off; no mixed-filament definitions |
| Output layers | 737 |

Despite its filename, the archive contains approximately **2.18 million triangles**. Both builds' preflight reports agree on the mesh statistics, including 2,398 open edges and 131 mesh parts. The benchmark preserves the saved geometry and orientation; it does not repair the model or alter print quality. The original input hash was checked again after the timing matrix and was unchanged.

Both builds report the same noncritical floating-cantilever warning. Their CLI logs also contain the same missing legacy BBL preset-lookup and exclude-triangle messages. The saved project supplies the configuration; all key/value entries in the six exported G-code configuration blocks are identical. Successful slicing and matching configuration do not establish identical toolpaths.

## Builds and method

- **Before:** commit `9a791daff1` plus the common CLI-loading fix, preserved in `build_logs/cpu_optimization_benchmark/baseline_runtime/`. This is before the slicing-performance changes; it already includes the earlier preview-memory fixes.
- **After:** final wall/export code checkpoint `87930d936e`, using `build-dev-release/src/Release/`. Subsequent commits through `66d6b5e98c` change documentation only.
- Both use the same repository resources. DLL hashes were checked for every run.
- Machine: Windows, Intel Core Ultra 9 285H, 16 logical processors, 64 GB RAM. Existing power settings were retained; clocks were not locked.
- Six fresh-process slices ran sequentially in order **before 1, after 1, after 2, before 2, before 3, after 3**. Each has a fresh data/output directory. No build, profiler or other benchmark ran concurrently.
- Existing GUI instances remained open and untouched. Together they consumed **0.70–0.98 CPU-seconds** during each approximately one-minute run, with no sign of a concurrent GUI slice. Ordinary desktop/background activity remained present; this was not a fully isolated machine.
- All inherited `ORCA_*` overrides were cleared. `ORCA_LOCAL_Z_CLIP`, `ORCA_SEAM_VISIBILITY`, `ORCA_POLYGON_COMPUTE` and `ORCA_PAINT_ISLANDS` were set to `cpu`. Retained CPU improvements use their normal defaults. No validation or capture mode was enabled in the timing runs.
- `--debug 4` enabled matching stage markers in both builds. Only CLI time/triangle limits were raised. Logs report `use_cache=0`; introduced CPU caches reuse work within each first slice.
- Process elapsed time includes loading, slicing, G-code export and cleanup, excluding GUI/OpenGL preview construction. CPU-seconds sum execution across all threads. The runner polls at 0.5-second intervals, limiting whole-process timing precision. Stage intervals use log timestamps.
- Memory values are the largest Windows peak counters observed by the runner. Private commit is backing required for private allocations; working set is resident physical memory. Sampled current private usage is not substituted for the peak commit counter.
- These are warm filesystem-cache runs, following preflight loads. Ranges are observed variation, not confidence intervals. No physical-print or cross-platform validation was performed.

| Runtime DLL | SHA-256 |
|---|---|
| Before | `87f5007d5fa0b119049522c1b559e07c52fa58e6a8f7f87f079d56bb8162a666` |
| After | `3719ee5bc7571734f8f85f66c9bb655a5dda6d0b3dd50c8d1f66ccb17a82892b` |

## Repeated measurements

| Metric | Before median (range) | After median (range) | Observed change |
|---|---:|---:|---|
| Complete CLI elapsed time | 62.59 s (62.09–63.07) | 60.58 s (60.08–61.58) | **3.2% less time** |
| Slicing before export | 44.63 s (44.46–45.12) | 42.99 s (42.95–43.39) | 3.7% less time |
| G-code export | 12.90 s (12.60–13.03) | 11.83 s (11.80–11.87) | **8.3% less time** |
| Aggregate process CPU time | 393.28 CPU-s (380.55–393.78) | 368.94 CPU-s (366.64–369.33) | **6.2% less CPU work** |
| Peak working set | 3,129.68 MiB (3,091.35–3,136.05) | 3,139.15 MiB (3,128.93–3,150.71) | +0.3%; overlapping ranges |
| Peak private commit | 3,282.05 MiB (3,261.75–3,292.80) | 3,291.23 MiB (3,275.14–3,302.65) | +0.3%; overlapping ranges |

| Run in execution order | Complete CLI, s | Slicing, s | Export, s | CPU-s |
|---|---:|---:|---:|---:|
| Before 1 | 62.086 | 44.455 | 12.596 | 380.547 |
| After 1 | 61.577 | 42.948 | 11.869 | 368.938 |
| After 2 | 60.081 | 42.990 | 11.826 | 366.641 |
| Before 2 | 62.590 | 44.628 | 12.900 | 393.781 |
| Before 3 | 63.074 | 45.116 | 13.028 | 393.281 |
| After 3 | 60.578 | 43.386 | 11.805 | 369.328 |

All three after runs were shorter than all three before runs. The observed reduction is small, and the output variation prevents treating it as a strict equal-work A/B result. All runs remain in the table, including the first baseline's different infill result; none was discarded to improve the headline.

## Stage breakdown

Elapsed seconds, median of three observations for each build:

| Operation | Before | After | Interpretation |
|---|---:|---:|---|
| Startup and project loading | 4.21 | 4.22 | Essentially unchanged |
| Initial mesh slicing | 2.05 | 2.01 | Small part of this job despite the triangle count |
| Preparation after mesh slicing | 0.59 | 0.59 | No painting or Local-Z work |
| Classic wall generation | **6.06** | **4.02** | **33.6% lower** |
| Curled-extrusion estimation | 2.78 | 2.73 | Small change |
| Solid-surface detection | 1.59 | 1.66 | Small increase |
| Vertical shells | 5.69 | 5.78 | No shared-projection reuse; overlapping timing ranges |
| External-surface processing | 10.56 | 10.54 | Essentially unchanged |
| Bridge-over-infill processing | 5.96 | 6.42 | 0.46 s higher median; not a retained improvement claim |
| Infill and remaining slice finalization | 9.28 | 9.19 | Small change |
| G-code export | **12.90** | **11.83** | **8.3% lower** |
| Post-export work and process cleanup | 0.85 | 1.07 | Includes runner exit-detection granularity |
| **Complete process** | **62.59** | **60.58** | **3.2% lower** |

Each row is independently medianed, so stage medians need not sum to the complete-process median. Startup/cleanup are approximate remainders around the logged stages. The initial mesh interval ends at `Make overhang printable...`; preparation ends at `Generating walls...`. Walls end at `Generating perimeters in parallel - end`, separating curl estimation. Bridge processing uses its explicit start/end markers; the following interval includes infill generation and remaining finalization through the start of export. These are enclosing stage intervals, not exclusive function-level profiles.

### Why the improvement is smaller here

The classic-wall result is consistent with removing unused lower-layer offsets and sharing the consumed outline. The export result combines the retained export changes; this comparison does not isolate parser, output-buffer, geometry or seam contributions.

The larger historical opportunities are absent or small:

- No painting, mixed-material channel tracking or Local-Z geometry exists to accelerate.
- Supports are disabled, so tree-support parallelism cannot contribute.
- Every after run reports **zero shared shell projections reused**. A single active material offers no cross-material projection reuse.
- Seam-neighborhood caching is active, but after 1 reports only **0.067 s** in alignment and **0.234 s** in CPU seam visibility. These are already small compared with the complete slice. No Vulkan query work was dispatched.
- External-surface processing, infill, shells and bridge processing remain substantial. Optimizing those requires a new targeted investigation; stage time alone does not establish that they are suitable GPU kernels.

This supports the workload-dependent interpretation of the earlier results. Ordinary painted Starry Night benefited greatly from preparation, multi-region shells and supports. An unpainted single-material bridge benefits mainly from shared wall/export work in this comparison. Neither result predicts a universal normal-slicing percentage.

## Output checks and reference validation

The full output was hashed after normalizing the generated-at timestamp. A second comparison normalized only runtime object IDs in printing-object comments as well. **Neither comparison made all outputs equal.** All six configuration blocks match exactly, but commands and print statistics still vary. Per-role hashes are diagnostics, not geometric-equivalence proofs.

| Run | G-code lines | Positive-extrusion commands tagged internal solid infill | Filament estimate | Printing-time estimate |
|---|---:|---:|---:|---|
| Before 1 | 7,726,650 | 591,063 | 242.88 g | 1d 2h 12m 14s |
| Before 2 | 7,950,653 | 682,655 | 244.78 g | 1d 2h 30m 30s |
| Before 3 | 7,950,736 | 682,654 | 244.79 g | 1d 2h 30m 33s |
| After 1 | 7,950,624 | 682,741 | 244.79 g | 1d 2h 30m 32s |
| After 2 | 7,950,395 | 682,592 | 244.79 g | 1d 2h 30m 29s |
| After 3 | 7,950,385 | 682,567 | 244.79 g | 1d 2h 30m 30s |

Before 1 differs materially from the other unchanged-baseline runs in internal solid infill and print estimate. The other baseline runs are close to the after runs, but remain unequal. The follow-up below localizes some variation to geometry before wall generation; the exact cause of the large infill outlier remains unresolved. The repeated-baseline failure shows that a difference cannot simply be attributed to the new optimizations. It does not prove that every before/after difference is harmless.

A separate after-build run enabled `ORCA_SLICE_VALIDATE=1`, `ORCA_PREP_VALIDATE_REUSE=1`, `ORCA_WALL_SUPPORT=validate`, `ORCA_SHELL_VOLUME=validate` and `ORCA_SHELL_PROJECTION=validate`. It completed successfully with no reported reference mismatch. Its **65.59-second** runtime includes validation work and is excluded from the performance statistics. These checks cover exercised optimized operations, not complete output equivalence; inactive painting and shared-projection paths are not validated by this model. Seam reference validation was not enabled.

### Follow-up: how much varies, and where it starts

A diagnostic pass over the retained six G-code files grouped positive-extrusion commands by printed layer and extrusion role. The pass verified relative extrusion mode before summing positive E values. Its ordered hashes retain coordinates, extrusion, feed-rate text and command order, so a changed hash alone does not measure displaced geometry. Command-count differences are **net differences in counts**, not a count of all changed lines.

| Comparison | Filament-use spread | Print-estimate spread | Total G-code line-count spread |
|---|---:|---:|---:|
| All three original-build repeats | 639.04 mm; 1.91 g at reported precision | 18 min 19 s | 224,086 |
| Original repeats 2 and 3 | 2.64 mm; 0.01 g at reported precision | 3 s | 83 |
| All three current-build repeats | 1.33 mm; all round to 244.79 g | 3 s | 239 |

The original-build outlier is substantive: before 1 versus before 2 has **91,592 fewer internal-solid-infill extrusion commands**, approximately **13.4% fewer** than before 2. Summed positive coordinate-extrusion E in that role is **637.92 mm lower**, accounting for almost all of the approximately 636.40 mm difference in reported total filament use for that pair. Other roles have smaller positive and negative differences. This is not simply travel ordering, progress reporting or a timestamp.

For the current build, the 1.33 mm spread is approximately **0.0016%** of 82.07 m of filament, and the print estimate varies by 3 seconds over about 26.5 hours. Nevertheless, the after 1/after 2 comparison has some differing positive-extrusion command sequences on **86 of 737 printed layers**; after 2/after 3 has **83**. Close totals do not prove identical local paths. Before 2/before 3 differs on 91 layers; before 1/before 2 differs on 525.

Two additional current-build processes captured the prepared layer geometry at the end of `PrintObject::slice()`, before wall and infill generation. Both were intentionally stopped after the snapshot was closed and logged; these are diagnostic captures, not completed timing slices. They use the unchanged project and CPU-only defaults. Their snapshots differ on **58 of 737 layers**. Canonical comparison rotates each ring to a consistent starting point and sorts holes and polygon/surface groups while retaining winding and metadata; all 58 layers still differ. All 58 also have different net enclosed outline areas. The largest absolute net area difference is **0.4971 mm²**, at printed layer 111. This is a net area difference, not a spatial-displacement bound or a symmetric-difference area.

Source inspection identifies an existing order-dependent mechanism in ordinary mesh slicing:

1. [Triangle intersections](../src/libslic3r/TriangleMeshSlicer.cpp#L506) are appended to per-layer vectors under mutexes from a parallel triangle loop. Locking protects the container, but the append order depends on worker scheduling.
2. [Loop chaining](../src/libslic3r/TriangleMeshSlicer.cpp#L1130) starts from the first available segment and takes the first available matching continuation. Later passes connect open contours and close gaps. Input order and unresolved ties can therefore affect the chosen representation or connections, with downstream geometry processing potentially amplifying a difference.
3. The earlier source-face ordering fix applies to **slab projections used in painting**, not the ordinary `slice_make_lines` path. Both the original baseline and current source contain the ordinary append/chaining mechanism.

The snapshots establish that some variation starts before infill and export. The append/chaining code supplies a concrete scheduling-sensitive lead, but no intervention has yet isolated which choice produces each changed boundary or the large before-1 infill outlier. The input's 2,398 open edges make ambiguous contour reconstruction relevant to investigate; they are not proof of the cause. A suitable next diagnostic is stable ordinary source-face/segment ordering, followed by repeated early-boundary and full-output comparisons.

The observed Voronoi repair warning is not itself evidence of random behavior: its retry angles are fixed. The monotonic-infill path also constructs a local default-seeded generator. Neither finding justifies attributing this project's variation to a changing random seed. No GPU operation was active.

Evidence: [per-layer extrusion comparison](../build_logs/bridge_normal_20261020/variation_summary.json), [early geometry comparison](../build_logs/bridge_normal_20261020/early_variation.json), and the capture manifests/logs in `early_capture1/` and `early_capture2/` under the experiment directory. No production code was changed during this investigation.

## Reproduction and evidence

The retained measurement tools are [benchmark_cpu_slicing.py](../scripts/benchmark_cpu_slicing.py) and [compare_cpu_slicing.py](../scripts/compare_cpu_slicing.py). The latter deliberately fails its strict comparison when outputs differ. The experiment-specific runner and stage/output analysis are preserved with the raw evidence under `build_logs/bridge_normal_20261020/`.

With other `ORCA_*` overrides unset, representative single-run commands are:

```powershell
$env:ORCA_LOCAL_Z_CLIP = "cpu"
$env:ORCA_SEAM_VISIBILITY = "cpu"
$env:ORCA_POLYGON_COMPUTE = "cpu"
$env:ORCA_PAINT_ISLANDS = "cpu"
python -B scripts/benchmark_cpu_slicing.py --exe build_logs/cpu_optimization_benchmark/baseline_runtime/snapmaker-orca.exe --project "C:\Users\radug\Downloads\1MPolygonBridge.3mf" --output build_logs/bridge_before_new --debug 4
python -B scripts/benchmark_cpu_slicing.py --exe build-dev-release/src/Release/snapmaker-orca.exe --project "C:\Users\radug\Downloads\1MPolygonBridge.3mf" --output build_logs/bridge_after_new --debug 4
```

Use fresh output paths for every repeat and preserve the recorded runtime hashes when reproducing this exact comparison. Binaries, copied input and approximately 200 MiB of G-code per run remain local, outside Git.

Evidence: [manifest](../build_logs/bridge_normal_20261020/manifest.json), [timing and stage summary](../build_logs/bridge_normal_20261020/summary.json), [output/configuration and original-input checks](../build_logs/bridge_normal_20261020/output_checks.json), and [validation process metrics](../build_logs/bridge_normal_20261020/validation/slice/metrics.json). Each run directory also contains its environment, background CPU counters, raw logs and process-memory samples.
