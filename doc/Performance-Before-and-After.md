# Snapmaker Orca performance before and after

23 October 2026 · Branch `proposal_UI_changes_GPU_Accelerate_V2` · Current code `4a2badb7a4`

Compared with our starting measurements, the current CPU-only build reduces Starry Night's Local-Z slice from **320.42 to 177.68 seconds**, saving **142.74 seconds**. Normal slicing also benefits: ordinary Starry Night uses **8.9% less time**, and the single-material bridge uses **18.4% less**. The terrain model's later Local-Z optimization series produces the largest reduction: **141.14 → 20.30 seconds**.

These are historical starting measurements compared with fresh measurements of the current build. They include subsequent correctness changes; they are not an identical-output benchmark of performance changes alone. **All “after” slicing figures below use CPU defaults, with Vulkan removed.**

## Complete slicing time

Includes project loading, slicing, G-code generation and processing, and process cleanup. Excludes the graphical preview. Values are medians; lower is better.

| Workload | Before | After | Less elapsed time | Runtime speedup |
|---|---:|---:|---:|---:|
| Starry Night — Local-Z enabled | 320.42 s | 177.68 s | 44.5% | 1.80× |
| Starry Night — Local-Z disabled | 114.85 s | 104.65 s | 8.9% | 1.10× |
| 1MPolygonBridge — single material | 62.59 s | 51.10 s | 18.4% | 1.22× |
| Painted terrain — Local-Z enabled | 141.14 s | 20.30 s | 85.6% | 6.95× |

**Starting points:** Starry Local-Z and the bridge use the original pre-slicing-optimization build (`9a791daff1` plus the shared CLI-loading fix). Ordinary Starry uses its earliest recorded baseline, after the first CPU cleanup; it still has painting and multiple materials. Terrain was introduced later: its baseline already contains earlier shared optimizations, but precedes its new clipping index, exact-result cache and whole-path reuse. Its filename is `开启混色细分层高导致局部层高过小.3mf`.

**Output differences matter:** the original Starry Local-Z planner logged 2,848 sublayer passes; the current planner logs 10,900 for the same input archive. Thus its after build does substantially different planning/output work. Earlier reports' 105.97-second Local-Z and 72.76-second ordinary Starry results belong to intermediate builds with GPU options enabled; they are not current-build timings.

The slowdown from those intermediate checkpoints has not been isolated in a matched comparison. It should not be attributed to Vulkan removal from these measurements alone.

## Where time was saved

These operations are contained in the complete slicing times above. Their savings must not be added to the whole-job savings.

| Operation | Before | After | Less elapsed time |
|---|---:|---:|---:|
| Starry Local-Z: mesh, painting and preparation | 48.52 s | 31.67 s | 34.7% |
| Starry Local-Z: G-code export | 183.71 s | 85.31 s | 53.6% |
| Starry ordinary: vertical shells | 16.57 s | 2.77 s | 83.3% |
| Bridge: external surfaces and bridge preparation | 10.56 s | 4.48 s | 57.6% |
| Terrain Local-Z: G-code export | 127.49 s | 4.24 s | 96.7% |

G-code export includes path preparation, Local-Z clipping/routing, seam placement, command generation, filters and motion-time estimation—not just saving a text file.

## Memory

| Measurement | Before | After | Change |
|---|---:|---:|---:|
| Starry Night — Local-Z enabled: peak slicing private commit | 8.10 GiB | 5.43 GiB | 32.9% less |
| Starry Night — Local-Z disabled: peak slicing private commit | 7.95 GiB | 4.31 GiB | 45.8% less |
| 1MPolygonBridge — single material: peak slicing private commit | 3.21 GiB | 3.07 GiB | 4.3% less |
| Painted terrain — Local-Z enabled: peak slicing private commit | 0.76 GiB | 0.79 GiB | 4.1% more |
| Isolated G-code parser, fresh allocations | 923.7 MiB | 296.7 MiB | 67.9% less |
| Isolated G-code parser, previously oversized buffers | 1,561.1 MiB | 292.2 MiB | 81.3% less |

Private commit is memory requiring backing, not necessarily RAM currently resident. The parser rows are the original controlled memory-fix experiment on identical G-code, with matching move/arc checksums; they are alternative scenarios, not additive savings. Separately, live preview RAM was observed falling from **5.78 to 2.55 GiB** after the memory fixes and discrete-GPU selection. Those GUI snapshots had different allocation histories and move counts, so that observation is not a controlled same-session comparison.

## What changed

| Before | After |
|---|---|
| Repeatedly clip Local-Z paths against complex masks for component passes | Reuse exact clipping results and paths proven to stay within their region; spatially reject empty work and retain clipping at boundaries |
| Reconstruct and copy painting data repeatedly; retain large temporary meshes | Reuse decoded painting and selectors, compact temporary storage, and parallelize independent preparation |
| Recompute equivalent shell projections, wall/support outlines and seam neighborhoods | Share immutable geometry and cache repeated searches within the first slice |
| Build gap-fill geometry that is discarded; leave complex bridge layers with serial work | Skip calculations that cannot emit output and parallelize independent bridge directions |
| Copy stale arc scratch data into ordinary moves and retain oversized preview arrays | Store arc samples only for actual arcs and release excessive spare capacity |

These retained improvements are enabled by the normal build, including `.\build_release_vs2022.bat slicer`. No extra build flag, GPU preference or previous successful slice is required. Vulkan slicing experiments were removed; normal GPU viewport rendering remains.

## Measurement scope

Windows, Intel Core Ultra 9 285H, 16 logical processors, 64 GB RAM, VS2022 Release. Before measurements were recorded on 13, 20 and 21 October; after measurements were made on 23 October, twice per input, in sequential fresh CLI processes. Input hashes match across each before/after pair. Profiling, reference-validation and developer overrides were disabled for the new timings. Existing desktop applications remained open; clocks were not locked.

Before sample counts are two for Starry Local-Z, one for ordinary Starry, and three each for the bridge and terrain. The percentages compare endpoints directly, without compounding intermediate gains. Original and current builds can produce different toolpaths, and unchanged-build repeats also vary; these small-sample timing results do not establish identical G-code or universal print equivalence.
