# Snapmaker Orca performance improvement history

Measured work from 12–16 October 2026 through checkpoint `87930d936e`, followed by the 20 October bridge comparisons, V2 CPU changes and 21 October Local-Z path reuse through `720a634227` on `proposal_UI_changes_GPU_Accelerate_V2`.

The largest improvements came from removing repeated CPU geometry work, reusing immutable results within a slice, and parallelizing preparation while preserving ordered output. Vulkan provided a useful additional improvement for Local-Z clipping. Several other GPU experiments were correct but slower after transfers, synchronization and remaining CPU work were included.

Across the recorded beginning and end points, **Starry Night with Local-Z fell from 320.42 to 105.97 seconds: 66.9% less time, or 3.02× faster**, saving about **3 minutes 34 seconds per slice**. Peak slicing private commit fell from **8.10 to 4.18 GiB**. These cumulative observations include the CPU improvements, both GPU options enabled at the final endpoint, and intervening correctness fixes; they are not an output-equivalent A/B benchmark. The [cumulative comparison at the end](#cumulative-comparison-from-first-measurements-to-latest-build) gives the timing, CPU and memory breakdown.

**Without Local-Z, Starry Night fell from its earliest measured 114.85 to 72.76 seconds: 36.65% less time, or 1.58× faster.** Aggregate CPU work fell **42.9%**, and peak slicing private commit fell **7.95 → 3.91 GiB**. The main gains came from CPU shell reuse, painting preparation, tree-support processing, classic walls and seam caching. Local-Z GPU clipping contributes nothing to this workflow. This earlier ordinary baseline already contains the first CPU cleanup, so it does not measure every improvement from the project's beginning.

The first **unpainted, single-material comparison on 1MPolygonBridge** gives a smaller result: **62.59 → 60.58 seconds, or 3.2% less time**, with both GPU options off. Walls improved **33.6%** and export **8.3%**, while peak memory was essentially unchanged. Three runs per build compare the original pre-slicing-optimization baseline with the pre-V2 CPU defaults. Output varies even between unchanged-baseline repeats, so this is an observed timing improvement, not a proven identical-output speedup. The [bridge comparison](#1mpolygonbridge-without-painting-or-local-z) explains the different workload and limitations.

**A second, V2 round targets single-color slicing directly.** On the same bridge project, a fresh three-run comparison gives **67.64 → 58.11 seconds median, or 14.1% less time**, with CPU-only operation. External surfaces improve **58.1%**, infill/finalization **30.7%**, and peak private commit falls **155.6 MiB**. The current-session reference includes stable mesh ordering but disables the four new speedup groups. Machine conditions differ from the earlier bridge series, so these percentages must not be compounded.

**The latest Local-Z terrain comparison removes most of the remaining export bottleneck:** direct reuse of paths proven to belong to each component pass reduces the complete slice **53.60 → 15.59 seconds**, export **41.60 → 3.06 seconds**, and Local-Z path preparation **39.00 → 0.55 seconds**. These are three-run medians with the same executable, unchanged project and CPU-only operation. Boundary cases retain clipping. The [path-reuse comparison](#guarded-direct-reuse-of-local-z-paths) includes full-model geometry checks, output differences and the separate U1 result.

This report covers preview memory, CPU slicing, Local-Z, Vulkan experiments, painting preparation, wall generation and G-code export. It consolidates the earlier measurements and adds the 20 October bridge and 21 October Local-Z comparisons. Palette, color-calibration and other product features are outside its scope except where their behavior affects benchmark comparability.

## Main measured results

Each row compares the before and after configurations of **one experiment**. The reductions are not additive.

| Improvement | Workload and measured scope | Before → after | Result |
|---|---|---:|---|
| Remove stale arc data and excessive retained capacity | Isolated G-code parser, previously oversized buffers; private commit | 1,561.1 → 292.2 MiB | 1,268.9 MiB less committed memory |
| Initial CPU cleanup and parallel channel tracking | Starry Night Local-Z, complete CLI run | 320.42 → 312.04 s | 2.6% less elapsed time |
| Local-Z CPU clipping and wipe eligibility | Starry Night Local-Z, complete CLI run | 304.09 → 184.07 s | 39.5% less elapsed time |
| Vulkan Local-Z empty-path rejection | Starry Night Local-Z, complete CLI run | 182.71 → 160.42 s | 12.2% less elapsed time; export 83.45 → 65.06 s |
| CPU seam-neighborhood cache | Ordinary / Local-Z Starry, seam alignment | 6.49 → 3.01 / 14.06 → 6.09 s | 53.7% / 56.7% less alignment time |
| CPU wall and shell reuse | Ordinary / Local-Z Starry, complete CLI run | 112.30 → 95.34 / 155.41 → 136.63 s | 15.1% / 12.1% less elapsed time |
| Bridge-outline reuse and parallel overhang filtering | Ordinary / Local-Z Starry, tree-support stage | 11.22 → 6.31 / 11.47 → 5.53 s | Clear stage improvement; smaller, noisier whole-job gains |
| Painting and Local-Z preparation | U1 / ordinary Starry / Local-Z Starry, broad preparation stage | 9.12 → 4.62 / 28.18 → 16.93 / 37.73 → 18.86 s | Approximately halves preparation on the Local-Z workloads |
| Painting preparation memory reduction | Ordinary / Local-Z Starry, peak private commit | 8.17 → 3.86 / 8.09 → 4.08 GiB | About half the slicing-process peak commit |
| Latest wall/export changes | Ordinary / Local-Z Starry, complete CLI run | 78.78 → 72.76 / 110.18 → 105.97 s | 7.6% / 3.8% less elapsed time |
| Single-color CPU V2 | Bridge, complete CLI process | 67.64 → 58.11 s | 14.1% less time; CPU-only |
| Guarded Local-Z path reuse | Terrain, complete CLI process / export | 53.60 → 15.59 / 41.60 → 3.06 s | 70.9% less elapsed time; CPU-only; Local-Z path preparation falls 98.6% |

## Improvements that apply without Local-Z

Most retained CPU changes do not require Local-Z. They accelerate shared slicing stages whenever a model and its settings exercise the relevant work. The ordinary Starry benchmark keeps painting and mixed materials but disables Local-Z; its gains therefore demonstrate useful improvements outside the Local-Z path.

| Shared CPU improvement | Why it helps without Local-Z | Measured ordinary result in its controlled experiment |
|---|---|---|
| Shell filtering and projection reuse | Avoid shell projection for regions without internal fill; calculate filter geometry only when needed and share compatible projections | Vertical shells **16.74 → 2.66 s**; the combined wall/shell reuse phase reduced the complete job **112.30 → 95.34 s** |
| Painting and region preparation | Reuse decoded painting/selectors, compact temporary meshes, parallelize painted-surface occlusion and reduce graph/region bookkeeping | Broad preparation **28.18 → 16.93 s**; complete job **88.36 → 80.37 s**; peak private commit **8.17 → 3.86 GiB** |
| Bridge-outline reuse and tree filtering | Share a support outline between material regions and process independent overhang layers concurrently | Tree-support stage **11.22 → 6.31 s** |
| Classic wall support offsets | Reuse identical support outlines and omit offset variants no consumer needs | Latest wall generation alone **7.63 → 4.59 s**; curl estimation essentially unchanged |
| Seam-neighborhood caching | Reuse the same ordered geometric searches during seam alignment | Seam alignment **6.491 → 3.009 s**; export **32.836 → 29.227 s** |
| Export parser and geometry cleanup | Parse Klipper velocity commands without repeated regex searches, borrow output buffers, skip unused curvature and move lookup structures | Parser replay **2.667 → 0.465 s**; the latest combined wall/export phase reduced ordinary export **26.28 → 25.33 s** |

These rows come from different controlled rounds and cannot be added together. The parser replay is a component measurement, not an isolated whole-export gain. Sources: [CPU geometry reuse](CPU-Geometry-Reuse-Results.md), [painting preparation](Painting-Preparation-Experiments.md), [tree-support follow-up](CPU-Overhang-Parallelism-Results.md), [seam caching](Vulkan-Shared-Geometry-Experiment.md), and [latest wall/export changes](Wall-Export-Experiments.md).

Applicability depends on the workload. Painting-specific changes need a painted model; sharing across material regions needs eligible repeated geometry. Classic-wall changes do not accelerate Arachne, tree-support changes need that support path, seam caching benefits the alignment modes, and the velocity parser change applies to Klipper commands. The unpainted, single-material bridge follow-up measured a modest **3.2% whole-job reduction**, mainly alongside faster walls and export, with zero shared shell-projection reuse. These results do not establish a universal percentage for ordinary slicing.

All these retained CPU improvements are enabled in normal builds without either GPU checkbox. Optional GPU seam visibility also runs without Local-Z, but its measured saving is only a few tenths of a second and it has not established a complete ordinary-slice gain. Local-Z planning/clipping, Local-Z wipe eligibility and the new Local-Z-only distance batch cache are inactive in an ordinary slice. The [ordinary cumulative comparison](#starry-night-without-local-z) separates the full historical change from the individual experiments above.

## How to read the measurements

The test machine is a Windows system with an Intel Core Ultra 9 285H, 16 logical processors, 64 GB RAM, Intel Arc integrated graphics and an NVIDIA GeForce RTX 5060 Laptop GPU. The builds are VS 2022 Release. Existing power settings were retained and clocks were not locked.

The main inputs are plate 1 of **STARRY NIGHT PRINT.3mf**, its separately saved Local-Z-disabled variant, and **U1+有换料暂停+打印前必须看说明.3mf**. “Ordinary” Starry still contains painting and mixed materials; it is not a simple single-material model. The Starry workloads use classic walls; U1 uses Arachne. Recipe gradient flags can activate Local-Z even when the global SML switch is off.

| Measurement | Meaning and limitation |
|---|---|
| Complete CLI elapsed time | Loading, slicing, G-code generation/processing and cleanup in a fresh process. Excludes GUI/OpenGL preview construction. |
| Stage elapsed time | Time between named log markers or enclosing direct timers. Some historical stages group several operations. |
| Process CPU-seconds | Execution summed across all threads. Ten busy cores for one second contribute about ten CPU-seconds. |
| Sampled function CPU-seconds | Approximate attribution from running-thread stack samples and process counters. Inclusive parents and children overlap. |
| Sum of per-operation timers in workers | Overlapping durations across concurrent calls; neither serial elapsed time nor necessarily process CPU time. |
| Replay time | Cost on captured operations. Useful for screening; does not include all live scheduling, state or pipeline effects. |
| Working set | Pages currently resident in physical RAM for the process. |
| Private commit | Private memory requiring backing; not the same as resident RAM. MiB/GiB use powers of 1,024. |

Most timing comparisons contain two observations per configuration; the U1 shortcut retry and 20 October bridge comparison contain three. With two observations, the arithmetic mean and median coincide. Ranges are observed variation, not confidence intervals. Workloads ran sequentially in fresh output/data directories, with reference validation and stack sampling separated from timing unless explicitly described otherwise. The first U1 shortcut timing series was invalidated by a concurrent user slice and was excluded completely.

Caches introduced here work **during the first slice**. They reuse an outline, mask, neighbor list or other immutable result when several consumers need it during that slice. They are not a disk cache requiring a previous successful slice.

## 1. Preview memory and GPU selection

### Remove accidental data duplication and oversized allocations

The initial live preview inspection found a **5.776 GiB working set**, **5.632 GiB private commit** and **9.823 GiB peak commit**. The largest confirmed avoidable allocations were:

| Finding in the original live process | Avoidable allocation |
|---|---:|
| Arc interpolation scratch data copied into non-arc moves | 564.41 MiB of payload, plus allocation overhead |
| Excess move-vector capacity | 588.39 MiB |
| Excess line-offset, G-code-ID, path and render-array capacity | About 162.46 MiB |
| Combined identified avoidable payload/capacity | About 1.284 GiB |

Commit `9a791daff1` attaches interpolation samples only to real arc moves, clears stale scratch state, and releases excessive preview/result capacity. The compaction policy targets waste of at least 1 MiB exceeding 25% of live storage, avoiding constant shrink/grow churn for modest headroom.

A controlled standalone parser comparison used the same 3,143,915-line G-code file:

| Parser scenario | Before private commit | After private commit |
|---|---:|---:|
| Fresh parser | 923.7 MiB | 296.7 MiB |
| Parser with the previously measured oversized capacities | 1,561.1 MiB | 292.2 MiB |

Every run produced 2,344,341 moves and 398 actual arcs. Stale non-arc interpolation fell from 587,134,044 bytes to zero; actual arc samples remained 100,296 bytes. The checksum over move attributes, actual arc geometry and line offsets matched. Six regression cases passed 110 assertions. These parser figures isolate the fix; they are not a whole-GUI before/after comparison.

### Prefer the discrete GPU for the viewport

The Windows launcher now exports `NvOptimusEnablement = 1` and `AmdPowerXpressRequestHighPerformance = 1`. The media-control code no longer clears those requests. This requests a high-performance adapter; it is not an unconditional override of operating-system policy. It also does not enable Vulkan slicing by itself.

The original process rendered through Intel and reported about **1.315 GiB shared GPU memory** with zero dedicated usage. A later live process confirmed the **NVIDIA RTX 5060 Laptop OpenGL renderer**, **1.081 GiB dedicated GPU memory**, and **58.4 MiB shared GPU memory**. Its working set was approximately **2.51–2.55 GiB** and private commit **3.502 GiB**. Different live snapshots had different move counts and allocator histories, so the observed GUI reduction cannot be assigned entirely to either the memory fixes or GPU selection.

Shared GPU memory means system memory used through the graphics memory manager; it is not additional dedicated VRAM. Driver mappings, process memory and GPU counters can overlap. In the NVIDIA snapshot, 1,067.6 MiB of graphics-like write-combined mappings had only 52.1 MiB resident in the process working set. Adding their full mapped size to resident RAM would overcount.

### What still occupied RAM

The NVIDIA snapshot identified these large CPU allocations. They are capacity measurements, not a complete partition of resident memory:

| Owner | Approximate allocation |
|---|---:|
| Move vector, 2,297,758 records | 227.9 MiB |
| Preview paths and draw bookkeeping | 91.7 MiB |
| Line offsets, move maps and G-code IDs | 73.4 MiB |
| Retained slicing/support/Local-Z/tower arrays | 200.0 MiB |
| Scene rendering, picking and level-of-detail arrays | 178.7 MiB |
| Source mesh and paint arrays | 25.2 MiB |
| Font inputs, glyph tables and CPU atlas pixels | 71.2 MiB |
| TBB cached/free memory and unused slab slots | 359.0 MiB |
| Free blocks retained by the Windows heap | 71.5 MiB |
| Additional arrays matching preview vertex buffers | 347.1 MiB |

The last row is consistent with driver-side copies, but allocation call stacks did not establish NVIDIA ownership. Approximately 0.9 GiB of resident memory was not matched to specific owners; this is not evidence that the entire remainder is a leak or recoverable.

Moves remain a contiguous, uncompressed `std::vector<MoveVertex>`, **104 bytes per record** in the measured x64 build. No general move compression, sparse arc-table redesign, font-atlas release or allocator-trimming policy was implemented in this series. Those were investigated possibilities, not delivered savings. The later preparation work reduced slicing peaks separately.

Evidence: [initial memory audit](../build_logs/memory-investigation-43764.md), [parser verification](../build_logs/preview-memory-fix-verification.md), [intermediate attribution](../build_logs/memory_current_52904/gpu-and-memory-details.md), and [NVIDIA live snapshot](../build_logs/memory_current_36560/report.md). These raw memory reports are local ignored artifacts; the main findings are reproduced above.

## 2. Initial CPU cleanup and profiling

Commit `df79223de4` made read-only extrusion traversal borrow leaves instead of cloning them, parallelized independent mixed-filament channels, removed unnecessary geometry copies, and made Local-Z diagnostic dumps opt-in. Layer order within each activation channel remained sequential. A common CLI-loading fix was included in both benchmark builds and received no optimization credit.

| Starry Local-Z metric | Before | After |
|---|---:|---:|
| Complete CLI run | 320.42 s | 312.04 s |
| Slicing before export | 129.62 s | 121.30 s |
| Broad mesh/painting/Local-Z preparation | 48.52 s | 38.39 s |
| G-code export | 183.71 s | 183.65 s |
| Aggregate CPU time | 933.73 CPU-s | 934.18 CPU-s |

The overall saving was **8.38 seconds**, with essentially unchanged CPU consumption and export time. Normal diagnostic output fell from **2,849 files, about 181 MiB per slice, to zero**. It remains available through `ORCA_LOCAL_Z_DEBUG=1`. The copy removals were useful ownership improvements, but this comparison did not demonstrate an export speedup from them alone.

The next sampling profile explained why export needed attention:

| Initial profiled stage | Local-Z active | Local-Z disabled |
|---|---:|---:|
| Loading/configuration | 5.20 s | 5.24 s |
| Mesh-plane slicing | 1.53 s | 1.45 s |
| Painting and preparation | 36.75 s | 27.00 s |
| Walls, including the later-separated curl work | 19.35 s | 15.57 s |
| Solid surface detection | 4.67 s | 4.31 s |
| Vertical shells | 16.71 s | 17.86 s |
| External surfaces, bridges and infill | 0.64 s | 1.16 s |
| Tree support | 11.09 s | 10.86 s |
| Wipe tower and slicing finalization | 28.60 s | 1.47 s |
| G-code export | 183.09 s | 30.71 s |
| Cleanup | 1.99 s | 1.63 s |
| **Complete profiled process** | **309.62 s** | **117.25 s** |

Turning Local-Z off changes the feature and output; it is **not** a performance optimization preserving the same print. It exposed the cost of Local-Z and left a separate ordinary benchmark for subsequent work.

Local-Z export averaged only **1.13 busy cores**. Approximately 56% of its sampled CPU work was clipping, with another 9% in a repeated leak diagnostic. Across the entire run, about 60% of exclusive leaf samples were in Clipper. Mesh-plane intersections took only about 1.5 seconds: the UI's broad early “slicing mesh” period also included painting and region preparation.

Sources: [initial CPU results](CPU-Slicing-Optimization-Results.md), [initial profile](CPU-Slicing-Profile.md), and [Local-Z-disabled profile](CPU-Slicing-Profile-Without-LocalZ.md).

## 3. Local-Z CPU geometry and wipe-tower planning

Commit `5dff53f7c0` reduced repeated geometry work while retaining the integer clipping algorithm:

- Prepare immutable mask bounds once per layer. Reject disjoint intersections and filter relevant scanline ranges conservatively before invoking Clipper.
- Preserve polygon and fragment ordering, including a full-mask fallback for ambiguous ordering cases. Mathematical area equivalence alone is insufficient because filtering can alter integer rounding and output order.
- Move completed clipped paths instead of copying them.
- Inspect clipping result trees directly for wipe eligibility, avoiding a second polyline collection.
- Prepare eligibility independently across print heights on TBB, then consume it in the unchanged sequential tool-order planner.
- Make repeated base-path leak checks opt-in through `ORCA_LOCAL_Z_VALIDATE=1`, which also enables expensive exact reference comparisons.

| Starry Local-Z metric | Before | After | Reduction |
|---|---:|---:|---:|
| Complete CLI run | 304.09 s | 184.07 s | 39.5% |
| Export | 181.59 s | 82.25 s | 54.7% |
| Wipe tower and finalization | 28.51 s | 2.47 s | 91.3% |
| Total CPU time | 911.38 CPU-s | 820.39 CPU-s | 10.0% |

This comparison used one fresh baseline and the median of two optimized runs. It removed about **120 seconds** from the whole job. The gain was concentrated in previously serial work; preparation and peak memory did not improve materially.

Separate sampled profiles estimated Local-Z export clipping at **116.0 → 36.8 CPU-seconds** and wipe eligibility/preparation at **27.5 → 9.2 CPU-seconds**. These inclusive estimates explain the change but are not extra elapsed savings to add to the table.

Full-model exact clipping and eligibility checks passed. Complete G-code equality did not: early internal-solid-infill variation exceeded the previously observed baseline variation, and its cause was not established by this experiment. Later determinism improvements strengthened validation, but do not retroactively turn this historical comparison into an exact-output pass.

Source: [Local-Z CPU results](LocalZ-CPU-Optimization-Results.md).

## 4. Vulkan compute experiments

The Vulkan backend uses bounded batches, explicit transfers and synchronization, device capability checks, and CPU fallback. Devices prefer a compatible discrete GPU. A shared Vulkan instance and explicit shutdown before application DLL unload address a driver lifecycle failure found during development. Failed shutdown runs were excluded from performance results.

### Seam visibility scoring

The first kernel scores candidate visibility using the existing mesh-sample KD tree. It does not perform seam alignment or replace all seam placement.

| Measurement | CPU | NVIDIA Vulkan |
|---|---:|---:|
| Visibility scoring, two-run median | 598.738 ms | 224.601 ms |
| Complete CLI run, corresponding median | 182.130 s | 186.301 s |

The targeted stage was **2.67 times faster**, saving approximately **0.374 seconds**, including setup, transfers and teardown. There was **no demonstrated complete-slice speedup**. One Intel run scored visibility in 351.016 ms, but it was only one performance observation.

NVIDIA full-model validation checked 2,596,069 scores with zero differences above `2e-6`; many scores were not bit-identical. This supports numeric agreement within that tolerance, not guaranteed identical downstream seam choices. Visibility uses Vulkan 1.0 compute and float32; it remains an optional preference for ordinary and Local-Z slicing.

Source: [seam visibility experiment](Vulkan-Seam-Visibility-Prototype.md).

### Local-Z empty-path rejection

The useful GPU path proves that an extrusion entity would produce **no valid clipped paths**. It skips only those complete entities. Boundary crossings, touches, unsupported coordinates and uncertain decisions retain CPU clipping; the GPU does not construct nonempty clipped polygons.

| Two-run median, same executable | CPU clipping | Vulkan rejection + CPU fallback |
|---|---:|---:|
| Complete Starry Local-Z run | 182.707 s | 160.422 s |
| Export | 83.454 s | 65.058 s |
| Aggregate CPU time | 812.37 CPU-s | 776.34 CPU-s |

The measured export saving was **18.396 seconds**. Whole-job savings were larger because unrelated stages also varied; those extra seconds are not GPU acceleration of other stages.

Separate direct timers showed where the gain occurred:

| Export clipping subtotal | CPU diagnostic | Vulkan diagnostic |
|---|---:|---:|
| Remaining CPU intersections | 11.006 s | 2.307 s |
| Remaining CPU differences | 22.089 s | 10.186 s |
| GPU preparation, transfers, dispatch and reduction | — | 3.093 s |
| **Subtotal** | **33.095 s** | **15.586 s** |

Full-model validation checked **602,110 proposed skips** using the original post-seam CPU clipping, with no false-empty result. The kernel uses bounded integer geometry and requires `shaderInt64`; unsupported inputs/devices use CPU. Its original approximate 33 MiB GPU/staging budget is separate from full-process peak memory.

This path applies only to Local-Z export. An ordinary slice dispatches no Local-Z clipping work. Sources: [Local-Z Vulkan experiment](Vulkan-LocalZ-Clipping-Experiment.md) and [current controls](#current-build-and-runtime-behavior).

### GPU paths that did not provide a useful improvement

| Experiment | Measured result | Current disposition |
|---|---|---|
| GPU seam-neighbor prefetch | Ordinary alignment 3.050 s with CPU cache versus 4.145 s with seed prefetch; Local-Z 5.979 versus 6.820 s in exploratory runs | Diagnostic-only; CPU cache is the useful default |
| Prefetch every seam candidate | Ordinary alignment 9.012 s; millions of speculative lists and evictions | Not enabled by default |
| Empty closed-polygon detection in painting | Zero eligible empty production results on tested Starry and U1 painting operations | Diagnostic-only; no painting speedup |
| Empty closed-polygon detection in walls/shells | Captured shell CPU clipping 511.55 ms; NVIDIA prepass 1,504.79 ms while avoiding only 4.96 ms | Diagnostic-only; not a recommended path for these models |
| U1 all-stage polygon GPU retry | 242 valid skips, but shell stage 0.561 → 1.311 s; complete process 38.55 → 40.13 s medians | Real work skipped, insufficient saving to cover overhead |
| Indexed CPU Local-Z classifier | Live preparation 2.696 s versus Vulkan 3.552 s; whole-process timings too variable to establish a win | Developer-only comparison; no default switch |
| Vulkan FP64 boundary-distance traversal | Captured CPU serial 174.33 ms, parallel CPU 56.82 ms, batched GPU 288.02 ms plus 203.16 ms initialization | Prototype removed from production source |
| Latest wall empty-result replay | GPU 67.89 ms to avoid 5.97 ms of CPU clipping | Rejected for production |

Large regular batches can favor GPU compute, but a kernel's speed does not determine slice speed. Packing, transfers, synchronization, fallback, small batches and lost CPU concurrency can outweigh it. Full nonempty polygon clipping and robust Voronoi topology were not ported to Vulkan.

Sources: [shared geometry experiment](Vulkan-Shared-Geometry-Experiment.md), [indexed classifier](LocalZ-Indexed-Classifier-Results.md), [clean U1 retry](U1-Paint-Shortcut-Results.md), and [latest wall/export experiments](Wall-Export-Experiments.md).

## 5. CPU seam caching and wall/shell reuse

### Reuse seam neighborhoods

The seam cache stores immutable, ordered geometric neighbor lists. Seam choices and finalized-perimeter state remain live CPU decisions. It is bounded to 64 MiB of accounted entries and 131,072 entries, and is destroyed after the object's alignment. Later changes borrow cached lists and reduce duplicate lookups.

| Two-run median | Ordinary before → after | Local-Z before → after |
|---|---:|---:|
| Seam alignment | 6.491 → 3.009 s | 14.059 → 6.093 s |
| Export | 32.836 → 29.227 s | 68.330 → 60.538 s |
| Complete process | 124.170 → 120.427 s | 173.219 → 164.246 s |

This helps aligned/rear seam modes that perform those searches. Nearest/random modes do not acquire the same benefit. Local-Z Vulkan clipping was held enabled and seam visibility held on CPU in this comparison.

### Avoid redundant wall and shell geometry

The next phase reused identical lower-layer support offsets, borrowed wall outlines, prepared shell filter geometry lazily, shared ordered top/bottom projections across compatible regions, and skipped projection for regions without any internal fill. Interface-shell cases retain region-specific geometry when sharing would be invalid.

| Two-run median | Ordinary before → after | Local-Z before → after |
|---|---:|---:|
| Complete process | 112.30 → 95.34 s | 155.41 → 136.63 s |
| Total CPU time | 708.96 → 498.83 CPU-s | 775.12 → 559.67 CPU-s |
| Historical wall stage, including curl | 15.44 → 13.32 s | 19.17 → 16.00 s |
| Vertical shells | **16.74 → 2.66 s** | **16.26 → 2.03 s** |
| Export | 26.51 → 26.30 s | 56.19 → 55.12 s |

The largest gain came from avoiding shell work for empty internal regions. The CPU seam cache was enabled in both builds, so this comparison does not count its earlier gain again. Shell caches have a 64 MiB shared geometry budget and a 4 MiB entry limit; admission refinements retained only **3.84/3.88 MiB** on the tested ordinary/Local-Z model. Cache accounting excludes allocator overhead.

General mixed-color island splitting was rejected after full-model validation changed geometry. The restricted uniform-island shortcut remains opt-in. It qualified on no tested Starry multi-island layers and on 41 U1 layers; the clean U1 retry did not establish a useful whole-slice benefit.

Sources: [shared cache measurements](Vulkan-Shared-Geometry-Experiment.md), [CPU geometry reuse](CPU-Geometry-Reuse-Results.md), and [U1 shortcut results](U1-Paint-Shortcut-Results.md).

## 6. Tree support and parser follow-up

This phase compiled Klipper velocity-limit regex patterns once, reused a lower-layer bridge-support outline across material regions, removed shared writes from tree-overhang summaries, and ran independent final overhang filtering with at most four TBB workers. Ordered reduction publishes results after workers finish. Cross-layer sharp-tail propagation remains sequential.

| Two-run median | Ordinary before → after | Local-Z before → after |
|---|---:|---:|
| Tree-support stage | 11.22 → 6.31 s | 11.47 → 5.53 s |
| Complete process | 98.79 → 96.91 s | 140.71 → 136.16 s |

A separate same-executable ordinary control measured the final filter at **2.472 seconds serial**, versus **0.666–0.688 seconds parallel**. The whole support stage includes bridge-outline reuse and other work, so the entire stage reduction cannot be credited to that loop alone. Whole-job gains of 1.9%/3.2% were less decisive than the stage evidence.

Sampled inclusive CPU time in Klipper velocity parsing fell **6.00 → 1.99 CPU-seconds**, but export elapsed time in those diagnostic profiles stayed approximately **26.45/26.47 seconds**. That parser was saving work without being the pipeline's critical bottleneck. A later direct scanner supersedes the regex-based implementation.

Serial-versus-parallel validation compared all 1,257 layers in both Starry configurations. The synchronized expensive-detail cutoff still depends on timing; this change did not establish global deterministic slicing.

Source: [CPU overhang and parser results](CPU-Overhang-Parallelism-Results.md).

## 7. Painting, region processing and Local-Z preparation

The broad preparation phase contains much more than slicing triangles with horizontal planes. The retained changes address five parts of that work:

| Part of preparation | Implemented change |
|---|---|
| Recover/project painting | Reuse an immutable triangle selector per volume/phase; skip absent states; extract compact strict meshes while preserving coordinates and ordering |
| Construct material-region graphs | Store small adjacency lists inline and remove nested single-element candidate allocations; keep the robust Voronoi algorithm |
| Process painted top/bottom surfaces | Parallelize independent occlusion slots and reuse bounded color-independent outline intersections |
| Track mixed regions and prepare Local-Z | Short-circuit overlap checks once activation is established; batch independent geometry work while keeping cadence and dependency resolution sequential |
| Install material regions | Calculate extruder bounds once and reuse surface-type buckets against the same parent region |

Outline cleanup was already parallel. Initial grid construction measured only about 20 ms on U1 and 81 ms on Starry, so no additional GPU/grid implementation was added. After selector reuse, side projection was also too small to justify the proposed GPU pilot on these inputs.

Local-Z preparation batches admit at most 64 layers and 131,072 input vertices, with an oversized layer processed alone. This is an input-admission bound, not a hard cap on Boolean output. Retained batch vertex payload peaked around 2.5 MiB on U1 and 3.1 MiB on Starry, excluding containers, allocator overhead and in-flight temporaries. Workers use an immutable snapshot of mixed-row lookups rather than invoking a seemingly const method that can update a legacy cache.

### Controlled preparation results

These comparisons toggle the switchable preparation fast paths in the **same final executable**, with both groups retaining the determinism fixes and compact graph adjacency. Local-Z clipping and seam visibility use CPU here; these totals are not comparable directly with GPU-enabled rounds.

| Workload | Complete process before → after | Broad preparation before → after | Whole-job reduction |
|---|---:|---:|---:|
| U1, Local-Z | 37.39 → 33.37 s | 9.12 → 4.62 s | 10.8% |
| Starry, ordinary | 88.36 → 80.37 s | 28.18 → 16.93 s | 9.0% |
| Starry, Local-Z | 150.64 → 134.48 s | 37.73 → 18.86 s | 10.7% |

Here broad preparation starts at `Starting the slicing process` and ends at `Generating walls`; it includes initial mesh slicing. The latest stage table below separates mesh slicing from the rest.

| Direct elapsed substage | U1 before → after | Starry Local-Z before → after |
|---|---:|---:|
| Painted top/bottom processing | 4.737 → 1.400 s | 13.376 → 4.012 s |
| Side projection | 0.385 → 0.260 s | 1.146 → 0.830 s |
| Side-region construction | 1.131 → 1.096 s | 5.878 → 6.265 s |
| Local-Z plan preparation | 1.097 → 0.116 s | 9.152 → 1.136 s |
| Mixed-region tracking | 0.031 → 0.016 s | 3.269 → 1.704 s |
| Region installation | 0.267 → 0.274 s | 1.582 → 1.607 s |

Graph/installation cleanup did not demonstrate an individual elapsed-time improvement in these combined runs. The main savings were top/bottom processing and parallel Local-Z geometry preparation.

| Slicing-process memory peak | U1 before → after | Ordinary Starry before → after | Local-Z Starry before → after |
|---|---:|---:|---:|
| Resident working set | 1.85 → 1.35 GiB | 5.45 → 3.75 GiB | 5.35 → 3.84 GiB |
| Private commit | 2.77 → 1.48 GiB | 8.17 → 3.86 GiB | 8.09 → 4.08 GiB |

These are peak CLI slicing allocations, separate from the earlier retained GUI-preview memory investigation. Total CPU time was nearly unchanged for Starry Local-Z, **578.6 → 581.1 CPU-seconds**: parallelism lowered elapsed time without reducing aggregate work in that sample.

### Determinism fixes and stronger checks

An initial U1 thread-count audit found 17 geometrically different layer/color pairs. Projected-line tie-breaking, shell groups based on TBB chunk boundaries, and slab lines accumulated in worker-finish order were corrected. Afterward, U1 and Starry had zero ordered/geometric differences in the tested segmentation audit.

Final U1 G-code matched after timestamp and runtime object-label-ID normalization; Local-Z Starry matched after timestamp normalization alone. Ordinary Starry still varied downstream. Two exact preparation-boundary snapshots for ordinary Starry, **182,808,633 bytes each**, were byte-identical, including ordered geometry and surface metadata. This establishes that preparation boundary for those executions, not full ordinary-output determinism.

Claude's CLI review helped identify slab ordering and mutable-cache risks. The implementation decisions were checked in source and validated with references and tests; review agreement is not itself a performance or correctness result.

Source: [painting preparation experiments](Painting-Preparation-Experiments.md).

## 8. Latest wall generation and export improvements

The last experiment series retained five changes, all enabled by normal CPU defaults:

1. **Classic wall support masks:** generate only the consumed positive offset and share it across wall widths. Unused negative offsets are omitted. This does not accelerate Arachne.
2. **Klipper velocity parser:** replace repeated regex searches with a scanner preserving the historical matching grammar, including malformed-command behavior.
3. **Preview input:** borrow the existing NUL-terminated G-code output buffer rather than create a full temporary string copy for parsing.
4. **Extrusion quality:** omit curvature that export does not consume, reuse speed sections, and move layer indexes instead of copying them. Support/curl callers retain curvature.
5. **Local-Z distance batching:** precompute exact boundary distances for known perimeter/bridge vertices in bounded CPU batches. New, clipped or seam-created points use the original query. Limits are 65,536 points per object/layer and 131,072 total. Ordinary objects are excluded because their measured benefit was inconsistent.

Both existing GPU options were enabled in both builds throughout the main comparison; the baseline already contained painting/preparation improvements.

| Complete CLI elapsed time | Before runs | After runs | Before → after mean | Observed reduction |
|---|---:|---:|---:|---:|
| U1 | 29.20, 29.11 s | 28.68, 29.13 s | 29.15 → 28.90 s | 0.9%; within noise |
| Starry ordinary | 75.05, 82.50 s | 71.76, 73.75 s | 78.78 → 72.76 s | 7.6% |
| Starry Local-Z | 110.47, 109.88 s | 106.17, 105.77 s | 110.18 → 105.97 s | 3.8% |

| Stage mean | U1 before → after | Ordinary before → after | Local-Z before → after |
|---|---:|---:|---:|
| Wall generation alone | 2.10 → 2.16 s | **7.63 → 4.59 s** | **9.06 → 5.28 s** |
| Curled-extrusion estimation | 0.99 → 0.98 s | 5.59 → 5.53 s | 6.53 → 6.52 s |
| Entire G-code export | 14.07 → 13.61 s | 26.28 → 25.33 s | 55.07 → 54.39 s |

Classic walls improved about **40–42%**. Curl estimation was effectively unchanged. Ordinary process CPU time fell **457.02 → 403.58 CPU-seconds** and Local-Z **505.95 → 461.87 CPU-seconds**. Peak private commit was essentially unchanged in this phase.

The isolated parser replay processed 602,826 real commands: its median fell **2.667 → 0.465 seconds**, an **82.6%** reduction. Skipping export-only curvature reduced captured-path processing from **206.3–207.0 ms to 128.8–130.3 ms**, with identical output positions/distances. These are component savings, not additive reductions in a pipelined exporter's elapsed time.

A final same-executable Local-Z control disabled only distance batching. It took **107.66 seconds overall**, **56.81 seconds exporting**, and **45.30 seconds in the pipeline**, versus enabled two-run means of **105.97**, **54.39**, and **42.94 seconds**. This supports the admission policy, but the single control is not a new repeated benchmark.

### Experiments removed after measurement

| Experiment | Result |
|---|---|
| Indexed travel containment | Delayed build/query/fallback took 2.797 s versus 2.530–2.678 s for original clipping; removed |
| Fixed-mask guard cache during export | 1,843 hits, but context work 6.854 s versus 6.838 s; removed |
| Per-filament parallel mask compensation | No eligible multi-active-mask passes on the tested plans; removed for lack of demonstrated opportunity |
| Bounding-box pruning in wall replay | 52.64 → 48.73 ms across 9,565 operations; insufficient saving to retain another production path |
| Independent-component union replay | 9.335 → 8.961 ms across 4,065 unions; only 27 split, and three changed output order; rejected |

The Vulkan distance and polygon failures are recorded in section 4. Their archived prototypes/replays are not new production GPU options.

Source: [wall/export experiments, tests and rollback details](Wall-Export-Experiments.md).

## Latest complete stage breakdown

These are means from the **two final, unprofiled timing runs** per workload in the wall/export series. They use current CPU defaults and both GPU options enabled. Stage intervals were extracted from the retained logs; no new slices were run for this report. Lightweight preparation timers remained enabled consistently.

| Elapsed stage | U1 | Starry ordinary | Starry Local-Z |
|---|---:|---:|---:|
| Loading/configuration | 1.45 s | 5.18 s | 5.16 s |
| Mesh-plane slicing | 0.34 s | 1.40 s | 1.38 s |
| Painting, regions and Local-Z preparation | 4.21 s | 15.11 s | 16.55 s |
| Wall generation | 2.16 s | 4.59 s | 5.28 s |
| Curl estimation and handoff | 0.98 s | 5.53 s | 6.52 s |
| Solid surface detection | 0.82 s | 4.03 s | 4.40 s |
| Vertical shells | 0.48 s | 2.47 s | 2.09 s |
| External surfaces, bridges and infill | 0.11 s | 1.08 s | 0.63 s |
| Tree support | 3.30 s | 5.46 s | 5.75 s |
| Wipe tower and slicing finalization | 0.57 s | 1.34 s | 2.47 s |
| G-code export | **13.61 s** | **25.33 s** | **54.39 s** |
| Cleanup | 0.87 s | 1.25 s | 1.35 s |
| **Complete CLI run** | **28.90 s** | **72.76 s** | **105.97 s** |

Rounding can prevent displayed rows summing exactly. The boundaries are the existing profiling script's markers, with the end-of-perimeters debug marker separating walls from curl. Loading/cleanup include process launch/exit sampling uncertainty. “External surfaces, bridges and infill” is a log window, not a statement that every infill-related operation occurs in that row.

Export remains about **47% of U1**, **35% of ordinary Starry**, and **51% of Local-Z Starry** elapsed time. Its cost includes geometry and command processing, not just disk writes. The latest mean peak private commit was approximately **1.59 / 3.91 / 4.18 GiB** respectively. These are neither retained GUI memory nor VRAM figures.

### What was inside export before the latest pass

The last detailed sampled breakdown preceded the wall/export changes. It remains useful context, but its internal timings must not be presented as a fresh profile of the final binary. The same-build GPU comparison, after preparation improvements, measured:

| Local-Z export component | CPU options | Both GPU options |
|---|---:|---:|
| Seam geometry, candidates and support | 1.35 s | 1.35 s |
| Seam visibility | 0.60 s | 0.43 s |
| Seam alignment | 5.07 s | 5.25 s |
| GPU clipping preparation/transfers | — | 3.41 s |
| Remaining CPU intersections | 10.73 s | 2.22 s |
| Remaining CPU differences | 21.64 s | 9.86 s |
| Remaining layer generation, filtering and output | 28.15 s | 28.31 s |
| Final processing/rewrite | 4.40 s | 4.42 s |
| Other export initialization | 0.01 s | 0.01 s |
| **Total export** | **71.96 s** | **55.25 s** |

These were single profiled runs, with whole-process times **128.07 / 111.39 seconds**. Local-Z output matched after timestamp normalization. Ordinary visibility improved **0.529 → 0.207 seconds**, but its complete run worsened **77.60 → 81.89 seconds** due to other variation; no ordinary whole-job GPU gain was established.

The “remaining” row is an elapsed residual of the existing pipeline. It contains travel planning, extrusion commands, quality/speed analysis, mask compensation, wipe-tower integration, cooling, pressure advance, parsing, preview/time estimation, formatting and output. Filters overlap the ordered generator, so their CPU times cannot simply be summed into elapsed export time.

For example, the pre-pass CPU profile estimated ordinary/Local-Z pipeline work at **4.70/5.45 CPU-seconds** for other travel generation, **4.83/4.99** for G-code parsing/preview/time estimation, **2.96/2.67** for extrusion quality, and **2.71/2.25** for cooling. Local-Z mask compensation added about **4.40 CPU-seconds**. These are sampled attribution, not promised removable seconds.

Local evidence: [CPU breakdown](../build_logs/wall_export_profile_20261016/profile-results.txt), [GPU comparison](../build_logs/wall_export_gpu_profile_20261016/profile-results.txt), and [final timing summary](../build_logs/wall_export_experiments_20261016/final_summary.json).

## Current build and runtime behavior

Normal `build_release_vs2022.bat slicer` builds include all retained CPU improvements. They do not need a new preference or build flag. The Windows helper also provisions the Vulkan build tools by default, using a complete SDK when available or pinned, hash-verified tools otherwise. End users need compatible drivers, not the SDK.

| Feature | Normal behavior |
|---|---|
| Preview memory fixes and high-performance GPU requests | Enabled |
| CPU clipping, caches, shell/wall reuse and tree-support changes | Enabled |
| Painting/preparation improvements | Enabled |
| Latest parser/output/quality changes | Enabled; distance batching only for eligible Local-Z objects |
| GPU Local-Z clipping | Optional; preference off by default |
| GPU seam visibility | Optional; independent preference off by default |
| GPU seam-neighbor prefetch and closed-polygon prepass | Developer experiments only; off by default |
| Uniform painted-island shortcut and indexed CPU classifier | Developer experiments only; off by default |
| Rejected travel, export-mask, FP64 distance and polygon-partition experiments | Not active production paths |

The GPU controls are under **Preferences → General → Slicing performance**:

- **Use GPU for Local-Z slicing (experimental)**.
- **Use GPU for seam visibility (experimental)**.

Explicit `ORCA_LOCAL_Z_CLIP` and `ORCA_SEAM_VISIBILITY` environment variables override the respective preference. A CPU-only package disables the controls with an explanation. For Windows builds, `ORCA_ENABLE_VULKAN_COMPUTE=OFF` deliberately omits the backend; default `AUTO` and explicit `ON` provision tools. Direct CMake `AUTO` instead detects available tools, while `ON` requires them. Unsupported devices or runtime failures fall back to CPU in normal acceleration modes.

The backend has Windows/Linux loader support and macOS Vulkan/MoltenVK portability handling in source. Hardware validation in this work was **Windows on NVIDIA and Intel**. AMD, Linux, macOS/Apple Silicon and ARM64 end-to-end performance or packaging were not validated. API portability is not a substitute for those tests. Local-Z's `shaderInt64` requirement also differs from the lighter seam-visibility kernel's requirements.

Developer controls for diagnostics and ablation are documented in the individual reports. In particular, `ORCA_PREP_EXPERIMENTS=none` disables the switchable preparation paths, and `ORCA_SLICE_EXPERIMENTS=none` selects reference implementations for the latest pass. Neither removes every historical optimization. Expensive validation, captures and debug dumps must be disabled for comparable timing runs.

## Correctness, limits and remaining work

The acceptance process combined deterministic fixtures, exact within-run comparisons, complete model slices and output comparisons. Important findings were retained even when they limited the performance claim:

- Early full G-code comparisons failed, including repeated unchanged baselines. The first Local-Z CPU experiment also showed a larger unresolved internal-solid-infill difference; it was not declared harmless.
- Later preparation fixes removed the tested thread-dependent segmentation differences. Exact ordinary preparation snapshots matched, while downstream ordinary infill still varied.
- In the latest pass, **every final U1 and Local-Z output matched the original baseline** after generation timestamp and runtime object-ID normalization. One of the two final ordinary outputs also matched; the other varied in gap/internal-solid fill and had a one-second print-time estimate difference. This does not establish complete ordinary determinism or prove every difference pre-existing.
- The latest selected regression suite passed **240 cases**, with **two pre-existing expected failures**; reference-mode checks passed **23 cases / 186,876 assertions**. Release builds and all three final in-slice validation jobs passed.
- No physical-print validation or cross-platform production certification follows from these CPU/GPU experiments.

The strongest remaining targets are the stateful export pipeline, nonempty Local-Z clipping, painting Voronoi/Boolean geometry, and curl/distance processing. Curl estimation still depends on the previous layer. Travel, active tool, seam decisions, retraction, cooling, pressure and emitted stream state constrain export ordering. Independent preparation can run concurrently, but the entire layer generator cannot simply become an unordered CPU loop or GPU kernel.

Any further GPU experiment should use realistic batches and include structure construction, packing, transfer, synchronization, CPU fallback and teardown. It should then demonstrate a repeated complete-slice improvement over **current CPU defaults**, rather than an older uncached baseline. The failed experiments already show why a large CPU sample count or a fast isolated shader does not guarantee a useful reduction in elapsed slicing time.

## Evidence, reproduction and rollback

### Inputs and retained results

| Input | SHA-256 |
|---|---|
| Starry Night, original Local-Z recipes | `9ae317c5b99767a5f143e69e533db1588ed0e227e4d4400c56a2ff2bb9771ff2` |
| Starry Night, controlled Local-Z-disabled copy | `1fba784fec8ae005528cac2573a61ab9a3d0eba0aaaa4a5eaefa9ffdaed6449b` |
| U1 | `8e6e6441decac1408721b30d76597a4d343b81bf06882adb9feeb9ffe69edb8e` |

The latest benchmark compares baseline DLL `e437f540523f179857ee297a2e5e38015f246d306ec41f0398915c0f5f8b6d99` against final DLL `3719ee5bc7571734f8f85f66c9bb655a5dda6d0b3dd50c8d1f66ccb17a82892b`. Latest stage means come from `build_logs/wall_export_experiments_20261016/final1` and `final2`; baseline pairs are `final_baseline1` and `final_baseline2`.

Use [benchmark_cpu_slicing.py](../scripts/benchmark_cpu_slicing.py) for fresh-process timing, [profile_cpu_slicing.py](../scripts/profile_cpu_slicing.py) and [analyze_cpu_profile.py](../scripts/analyze_cpu_profile.py) for attribution, and [compare_cpu_slicing.py](../scripts/compare_cpu_slicing.py) for output comparisons. Preserve exact input/binary hashes and environment settings, alternate run order, and keep builds, validation and competing slices outside timed runs. The sampler briefly pauses the disposable CLI process; profiled totals remain separate from unprofiled timings.

Raw projects, G-code, captures, DLLs, matching PDBs and profiling traces are under ignored `build_logs/`. They are not included in a Git checkout. The linked `doc/` reports preserve methodology and measured results; their local artifact links require those directories when sharing the underlying evidence.

| Phase | Detailed report | Local evidence directory |
|---|---|---|
| Initial CPU | [CPU slicing results](CPU-Slicing-Optimization-Results.md) | `cpu_optimization_benchmark/` |
| Initial on/off profiles | [Local-Z profile](CPU-Slicing-Profile.md), [ordinary profile](CPU-Slicing-Profile-Without-LocalZ.md) | `cpu_slicing_profile/` |
| Local-Z CPU | [Local-Z CPU results](LocalZ-CPU-Optimization-Results.md) | `local_z_cpu_optimization/` |
| Seam visibility | [Vulkan seam prototype](Vulkan-Seam-Visibility-Prototype.md) | `vulkan_seam_prototype/` |
| Local-Z GPU | [Vulkan clipping](Vulkan-LocalZ-Clipping-Experiment.md) | `local_z_vulkan/` |
| Shared cache/GPU trials | [Shared geometry](Vulkan-Shared-Geometry-Experiment.md) | `vulkan_shared_geometry/` |
| CPU wall/shell reuse | [Geometry reuse](CPU-Geometry-Reuse-Results.md) | `cpu_reuse_optimization/` |
| Classifier and support follow-up | [Indexed classifier](LocalZ-Indexed-Classifier-Results.md), [CPU follow-up](CPU-Overhang-Parallelism-Results.md) | `classifier_review/` |
| U1 shortcut retry | [U1 results](U1-Paint-Shortcut-Results.md) | `u1_paint_shortcuts_retry_20261016/` |
| Painting/preparation | [Preparation experiments](Painting-Preparation-Experiments.md) | `preparation_experiments_20261016/` |
| Latest wall/export | [Wall/export experiments](Wall-Export-Experiments.md) | `wall_export_experiments_20261016/` |

### Commit checkpoints

Changes were committed in separate units for rollback. Later refinements can depend on earlier helpers; revert dependents before their prerequisites. Detailed reports list the individual commits inside grouped phases.

| Commit or range | Delivered work |
|---|---|
| `9a791daff1` | Preview memory fixes and discrete-GPU request |
| `df79223de4` | Initial CPU cleanup, channel parallelism and profiling tools |
| `5dff53f7c0` | Local-Z CPU clipping and wipe eligibility |
| `707c6e1853`, `8106485824` | Intervening tower/gradient behavior fixes; not credited as performance wins |
| `b6f5376796`, `2700657c53` | Vulkan backend/seam visibility and Local-Z rejection |
| `be55f05ae9` | CPU seam cache and shared GPU experiments |
| `57b9c8fe85` through `bf76803c0d` | Wall/shell reuse, copy reduction and default seam caching |
| `ccd694a876`, `b1321ce3f1`, `532904eacc`, `c794213bf7` | Vulkan build detection/provisioning and persistent preferences |
| `7c85c45667` | Indexed CPU classifier comparison |
| `045dedb7e8` through `24a8e7f2f6` | Parser characterization/reuse, bridge outline reuse and parallel tree filtering |
| `c294f2b5e9` through `f0d9347251` | Preparation instrumentation, determinism, CPU improvements and exact snapshots |
| `e93899ef0c` | Remove unused classic-wall support offsets |
| `37d869fd2a`, `874ea12936` | Velocity scanner and borrowed G-code buffer |
| `beecb11b36`, `c9a3e13031` | Travel-index experiment followed by removal |
| `d79fd3791d`, `feb0b1e1ba` | Export-quality simplification and bounded Local-Z distance batches |
| `8784142ea1`, `87930d936e` | Reproducible wall/export replays and final results |

The [Vulkan design](Vulkan-Compute-Acceleration-Design.md) and [later target review](Vulkan-Next-Targets-Review.md) also contain proposals. Their proposed work should not be counted as delivered optimization unless a later results report records its implementation and validation. Likewise, early experiment documents preserve historical build/runtime defaults; the current behavior is summarized above.

## Cumulative comparison from first measurements to latest build

### Starry Night with Local-Z

The starting point is the first benchmark **before the slicing-performance changes**: commit `9a791daff1` plus the common CLI-loading fix. It already includes the preview-memory fixes, which are compared separately below. The ending point is the final wall/export build, with all retained CPU improvements and **both Vulkan preferences enabled**. Both endpoints use the original Starry Night input and the same machine, with two fresh-process timing runs each.

| Measurement | Before slicing improvements | Latest build | Absolute reduction | Relative reduction |
|---|---:|---:|---:|---:|
| Complete CLI elapsed time | **320.42 s** | **105.97 s** | **214.45 s** | **66.9%** |
| Mesh, painting and Local-Z preparation | 48.52 s | 17.93 s | 30.59 s | 63.0% |
| Entire G-code export | 183.71 s | 54.39 s | 129.31 s | 70.4% |
| Aggregate process CPU time | 933.73 CPU-s | 461.87 CPU-s | 471.87 CPU-s | 50.5% |
| Peak slicing private commit | 8.10 GiB | 4.18 GiB | 3.93 GiB | 48.5% |

The complete slice went from approximately **5 minutes 20 seconds to 1 minute 46 seconds**, a **3.02× runtime improvement**. Preparation and export are parts of that total, not additional savings. The preparation row uses the same broad boundary at both endpoints: initial mesh slicing through the start of wall generation. CPU-seconds and peak commit are separate measures of work and memory, not elapsed time.

Before-run totals were **319.05 and 321.78 seconds**; final totals were **106.17 and 105.77 seconds**. The table calculates reductions from the unrounded means, so displayed rounded values can differ slightly when subtracted. It compares endpoints directly rather than adding or multiplying the phase percentages elsewhere in this report.

This measures the observed cumulative progress of the evolving application. It does not isolate performance changes from the intervening tower/gradient and determinism fixes, and the early and final generated outputs are not established as equivalent. Early Local-Z tower warnings were addressed between these endpoints. For causal evidence about each optimization, use its controlled comparison in the preceding sections. No final CPU-only measurement covering this entire historical span is available; the 105.97-second endpoint specifically includes optional GPU acceleration.

Sources: [original baseline measurements](../build_logs/cpu_optimization_benchmark/comparison_final.json), [final measurements](../build_logs/wall_export_experiments_20261016/final_summary.json), and the [latest stage breakdown](#latest-complete-stage-breakdown).

### Starry Night without Local-Z

The ordinary workload uses the same controlled Local-Z-disabled project at both endpoints. It still contains painted and mixed-material regions. The earliest unprofiled measurement was taken after the first CPU cleanup, before the subsequent seam cache, wall/shell reuse, support, preparation and export changes. No earlier pre-all-improvements ordinary benchmark was recorded.

| Measurement | Earliest ordinary baseline | Latest ordinary build | Absolute reduction | Relative reduction |
|---|---:|---:|---:|---:|
| Complete CLI elapsed time | **114.85 s** | **72.76 s** | **42.09 s** | **36.65%** |
| Painting and region preparation, excluding initial mesh slicing | 26.76 s | 15.11 s | 11.66 s | 43.6% |
| Walls and curl combined | 15.13 s | 10.12 s | 5.01 s | 33.1% |
| Vertical shells | 16.57 s | 2.47 s | 14.10 s | 85.1% |
| Tree support | 11.36 s | 5.46 s | 5.91 s | 52.0% |
| Entire G-code export | 30.17 s | 25.33 s | 4.85 s | 16.1% |
| Aggregate process CPU time | 706.42 CPU-s | 403.58 CPU-s | 302.84 CPU-s | 42.9% |
| Peak slicing private commit | 7.95 GiB | 3.91 GiB | 4.04 GiB | 50.8% |
| Peak resident working set | 5.24 GiB | 3.67 GiB | 1.57 GiB | 29.9% |

The complete ordinary slice went from approximately **1 minute 55 seconds to 1 minute 13 seconds**, a **1.58× runtime improvement**. The largest historical stage reductions were shells and painting preparation, followed by support and walls. They reduce work before export, explaining why ordinary slicing improved substantially even though the largest Local-Z-specific optimization is absent. Initial mesh-plane slicing was nearly unchanged at **1.44 → 1.40 seconds**.

The baseline is one unprofiled run; the final value is the mean of **71.76 and 73.75 seconds**. Stage values above were extracted from those same runs using matching log boundaries, rather than substituting the older sampled profile. Walls and curl are combined because the earlier logs did not separate them; the latest **10.12 seconds** is **4.59 seconds walls + 5.53 seconds curl**. Reductions use unrounded measurements. These stage rows are parts of the whole job, and CPU and memory reductions are separate measures.

Both GPU preferences were enabled at the final endpoint, but **Local-Z clipping dispatches zero work** in this project. Seam visibility remains the only enabled GPU operation that applies. Its separate profiled comparison improved **0.529 → 0.207 seconds**, without establishing an overall ordinary-slice GPU benefit. The evidence for the substantial ordinary gains comes from the shared CPU experiments above; the historical 36.65% figure is not a GPU speedup claim or a final GPU-on/off comparison.

This is a historical comparison across builds and includes the later determinism fixes. Ordinary G-code still varies in some downstream infill between runs, including unchanged baselines, so complete output equivalence across these endpoints is not established. The exact preparation-boundary comparison and the per-operation checks provide narrower correctness evidence.

Sources: [earliest ordinary process metrics](../build_logs/cpu_slicing_profile/starry_no_local_z_unprofiled_1/metrics.json), [earliest ordinary report](CPU-Slicing-Profile-Without-LocalZ.md#unprofiled-timing-cross-check), [final process metrics](../build_logs/wall_export_experiments_20261016/final_summary.json), and [latest stage breakdown](#latest-complete-stage-breakdown). Stage extraction uses the earlier run's `stdout.log` and `final1/ordinary/slice/stdout.log` / `final2/ordinary/slice/stdout.log` in the final experiment directory.

### 1MPolygonBridge without painting or Local-Z

This new 20 October comparison uses the user's unchanged **1MPolygonBridge.3mf**, containing **2,179,126 triangles**, no painting and one used filament. Saved settings are Snapmaker U1, 0.4 mm nozzle, 0.12 mm layers, classic walls, aligned seams and supports off; output has 737 layers. Neither Local-Z nor mixed recipes is active. Both builds use CPU-only slicing, so GPU acceleration contributes nothing to this comparison.

The before build is the original pre-slicing-optimization checkpoint `9a791daff1` plus the common CLI-loading fix; the after build is the final retained CPU implementation at `87930d936e`. The baseline already includes the earlier preview-memory fixes. Three fresh-process runs per build executed sequentially in before / after / after / before / before / after order. The table reports medians; wall time includes loading and cleanup but excludes GUI preview construction.

| Measurement | Before | After | Observed change |
|---|---:|---:|---|
| Complete CLI elapsed time | **62.59 s** | **60.58 s** | **2.01 s / 3.2% less time** |
| Classic wall generation | 6.06 s | 4.02 s | 33.6% less time |
| G-code export | 12.90 s | 11.83 s | 8.3% less time |
| Aggregate process CPU time | 393.28 CPU-s | 368.94 CPU-s | 6.2% less CPU work |
| Peak resident working set | 3,129.68 MiB | 3,139.15 MiB | Essentially unchanged; +0.3% |
| Peak private commit | 3,282.05 MiB | 3,291.23 MiB | Essentially unchanged; +0.3% |

Before totals were **62.09, 62.59 and 63.07 seconds**; after totals were **61.58, 60.08 and 60.58 seconds**. Walls and export show the main reductions. External-surface processing remains about **10.54 s**, infill and remaining finalization **9.19 s**, bridge-over-infill processing **6.42 s**, and vertical shells **5.78 s**. Initial mesh slicing is only **2.01 s**. Bridge processing's median increased from **5.96 s**; this stage did not improve in the comparison. These are enclosing stage intervals, not exclusive function timings.

This workload has no painting, Local-Z or tree-support work to benefit from the larger historical optimizations. Every after run reports **zero shared shell projections reused**. Seam caching is active but alignment takes only about **0.067 s** in the first after run. This explains why ordinary painted Starry Night and an unpainted single-material model show very different gains.

**Full output equivalence did not pass.** All six exported configuration blocks match exactly, but toolpaths vary within both builds. Before 1 estimates **242.88 g / 1d 2h 12m 14s**, with fewer internal-solid-infill commands; the other baseline runs estimate **244.78–244.79 g / 1d 2h 30m 30s–33s**, close to the after runs' **244.79 g / 1d 2h 30m 29s–32s**. No run was discarded. The unchanged-baseline variation prevents attributing every output difference to the optimizations, but does not prove the differences harmless. A separate run with the built-in preparation, wall/shell and export reference controls completed without a reported mismatch; its timing is excluded. Complete toolpath equivalence remains unresolved.

A follow-up quantifies the variation: the current build's three repeats differ by **1.33 mm of filament out of about 82.07 m**, all round to **244.79 g**, and span **3 seconds** of estimated printing time. The original outlier differs from the other baseline runs by about **1.91 g and 18 minutes**. Two additional early-geometry captures differ on **58 of 737 layer outlines before wall/infill generation**, including actual enclosed-area changes. Ordinary triangle intersections are accumulated in worker-dependent order and then chained greedily; this is a concrete existing determinism lead, separate from the earlier painting-slab ordering fix. The exact cause of the large infill outlier is still unproven. These diagnostic captures are excluded from timing.

See the [full bridge report](Normal-Slicing-Bridge-Benchmark.md) for all stages, individual runs, configuration, hashes, variation diagnostics and validation scope. Raw evidence: [timing summary](../build_logs/bridge_normal_20261020/summary.json) and [output checks](../build_logs/bridge_normal_20261020/output_checks.json).

### V2 improvements for single color slicing

The V2 round removes gap-fill calculations whose results cannot be emitted, skips discarded solid-fill preparation when only sparse bridge anchors are needed, parallelizes independent bridge directions within complex layers, and shortcuts single-variant process/printer flow lookups. These CPU changes are enabled by default in normal builds.

The same unmodified, unpainted 1MPolygonBridge project was sliced in three fresh processes per configuration, with no Local-Z, painting, supports or GPU computation. The reference uses the same executable and mesh-order correction, with just the new speedups disabled.

| Measurement | V2 reference median | V2 enabled median | Observed reduction |
|---|---:|---:|---:|
| Complete CLI process | 67.64 s | 58.11 s | **14.1%** |
| External surfaces | 11.24 s | 4.71 s | **58.1%** |
| Infill and remaining finalization | 12.09 s | 8.38 s | **30.7%** |
| Bridge over infill | 6.82 s | 6.83 s | No whole-stage gain established |
| G-code export | 13.27 s | 12.79 s | Small difference with overlapping ranges |
| Aggregate CPU work | 391.91 CPU-s | 367.92 CPU-s | **6.1%** |
| Peak private commit | 3286.32 MiB | 3130.77 MiB | **155.6 MiB / 4.7%** |

Complete-process reference runs were **67.64, 70.62 and 66.46 seconds**; enabled runs were **58.11, 58.62 and 54.83 seconds**. The preserved pre-V2 executable took **66.61 seconds** in the current session, versus its earlier **60.58-second** median. Normal desktop applications remained open and clocks were not fixed. This is a new paired comparison, not a cumulative percentage to add to the earlier 3.2% result.

Separate diagnostics found **4,096 gap-fill calls** whose results were discarded, occupying **41.64 seconds summed across worker calls**. That is overlapping worker duration, not elapsed slice time or measured CPU time. Anchor preparation fell **5.68 → 2.74 summed worker seconds**, but did not materially reduce the enclosing bridge stage. A separate two-million-call lookup test fell **306.55 → 136.30 ms median**; that is an isolated lookup gain, not a 55.5% export improvement.

Gyroid wave generation occupied only **0.206 summed worker seconds**, so no new cache or Vulkan kernel was added. Horizontal-shell work was inactive for this configuration. More general vertical-shell window reuse would change Boolean operation order, and a structured G-code pipeline remains a larger architectural project.

A separate correctness fix restores serial triangle-face order before greedy loop repair. Two new early captures match on **all 737 layers**, compared with the previous **58 differing layers**. Full G-code still varies: across the six timing runs, displayed filament consumption spans **0.02 mm**, all report **244.79 g**, and estimated printing time spans **one second**. The changed geometry operations are also checked against original calculations on the same inputs. Complete toolpath determinism is not claimed.

See [Single color slicing improvements V2](Single-Color-Slicing-V2.md) for the experiments, tests, individual commits and reproduction controls.

### Memory from the initial investigation

| Measurement | Before memory fixes | After memory fixes | Observed reduction |
|---|---:|---:|---:|
| Live sliced-preview working set | 5.776 GiB | 2.549 GiB | 3.227 GiB / 55.9% |
| Isolated parser, fresh allocations: private commit | 923.7 MiB | 296.7 MiB | 627.0 MiB / 67.9% |
| Isolated parser, previously oversized buffers: private commit | 1,561.1 MiB | 292.2 MiB | 1,268.9 MiB / 81.3% |

The live-preview row compares the original Intel-rendered snapshot with the later NVIDIA-rendered snapshot after the memory fixes. It is an observed beginning-to-after-fix comparison, not a final-build rerun of an identical GUI session: move counts, graphics residency and allocator history differ. The two parser rows are controlled same-input comparisons and provide the stronger evidence for the memory fixes themselves. They are alternative scenarios, not amounts to add together. The final slicing peak in the preceding table measures a different process phase from retained GUI-preview RAM.

Sources: [initial live audit](../build_logs/memory-investigation-43764.md), [post-fix NVIDIA snapshot](../build_logs/memory_current_36560/report.md), and [controlled parser verification](../build_logs/preview-memory-fix-verification.md).

### U1 from its earliest clean baseline

U1 uses Local-Z and is a separate model from both Starry configurations. Its earliest clean CPU baseline already includes the earlier CPU reuse/support work: **38.55 seconds**, the median of three shortcut-retry runs. The final two-run mean with both GPU options enabled is **28.90 seconds**, an observed reduction of **9.65 seconds / 25.0%**. This is progress from a later checkpoint, not a pre-all-improvements baseline or a non-Local-Z result.

As with the other historical comparisons, sample sizes differ and complete output equivalence across the interval is not established. Sources: [clean U1 baseline](U1-Paint-Shortcut-Results.md#clean-timing-results) and [final timings](Wall-Export-Experiments.md#final-selection-and-repeated-slice-timings).

## Local-Z terrain clipping — 21 October 2026

The painted terrain project `开启混色细分层高导致局部层高过小.3mf` exposes a different Local-Z workload from Starry Night. Its 532,548 triangles include 267,952 painted triangles. The saved configuration uses 0.2 mm nominal layers, two mixed passes on each of 21 affected layers, 0.04–0.16 mm mixed-pass heights, and Local-Z infill. The project was not repaired or resaved. Its SHA-256 is `0747970025e0dbc758023833e51173c42395c82966ece97f44445b62e64e2f63`.

The initial current-build profile took **130.68 seconds**, including **116.95 seconds in G-code export**. An unsampled confirmation took **129.66 seconds**, with **117.02 seconds in export**. Painting and Local-Z preparation occupied approximately four seconds. The expensive work was repeated clipping of individual extrusion paths against the layer's mixed-color masks, using approximately one CPU core during export.

Two CPU changes are enabled by default in normal builds, including builds without Vulkan:

1. **Conservative empty-path classification.** Each sufficiently complex mask prepares the existing bounded-integer CPU spatial index. A query skips Clipper only when every segment is proven to produce an empty result. Hole winding, overlapping contours, boundary touches and rounding guards retain the original rules. Unsupported coordinates, oversized indexes and ambiguous cases fall back to Clipper. Nonempty results continue through Clipper; containment alone does not authorize returning the original polyline.
2. **Exact result reuse across matching passes.** An export layer shares a cache between clippers whose complete polygon, hole and vertex sequences are identical. Keys include the clipping operation and every input point in order, with full equality checks after hashing. Only masks used by multiple clippers retain results. Results are copied back to callers, leaving seam placement, extrusion properties and mixed-pass flow-height calculations in their original sequence. The cache is discarded after the layer, has a 32 MiB accounted-storage budget and a 65,536-entry limit, and falls back to ordinary computation when full. Ordinary layers without active Local-Z do not allocate it.

| Clipping operation in the instrumented profiles | Before | Combined |
|---|---:|---:|
| Intersections with mixed-pass masks | 75.16 s | 40.07 s |
| Subtraction of mixed regions from base paths | 38.62 s | 1.49 s |
| Complete G-code export | 116.95 s | 44.82 s |
| Complete process | 130.68 s | 59.09 s |

These are diagnostic runs, separate from the unsampled timing series. The original run made 110,010 intersection and 55,005 difference calls; the combined run made 109,944 and 54,972. Existing upstream variability changes some generated inputs between runs, so these counts are not claimed to be identical. The combined profile reused **54,972 results**. Its largest layer cache accounted for **1,345,736 bytes / 1.28 MiB**, excluding index storage and allocator overhead.

The slowest layer, Z = 9.4 mm, fell from **28.54 to 11.02 seconds** of clipping preparation in those profiles. Empty base-path differences fell from **37.46 to 0.20 seconds**. Nonempty intersections remain the dominant export cost and still use Clipper.

The repeated comparison alternates three reference and three enabled runs in fresh CLI processes. All six use the same final Release DLL, the unmodified project, and CPU geometry backends; `ORCA_LOCAL_Z_CPU=legacy` disables only the two changes above. Stack sampling, reference validation and clipping counters are off, with debug logging at level 4. Normal desktop applications remain open and clocks are not fixed. The final DLL SHA-256 is `a89ac2b272cc945cf891e58df4c4cb4ba4240a19b28be84b0abf6e91d5a709fb`.

| Measurement, three-run medians | Reference | Enabled | Observed change |
|---|---:|---:|---:|
| Complete CLI process | 141.14 s | 58.57 s | **58.5% less time / 2.41× speedup** |
| G-code export | 127.49 s | 44.30 s | **65.3% less time** |
| Local-Z path preparation within export | 124.48 s | 41.35 s | **66.8% less time** |
| Slowest layer's preparation, Z = 9.4 mm | 33.08 s | 10.94 s | **66.9% less time** |
| Aggregate CPU work | 182.55 CPU-s | 104.20 CPU-s | **42.9% less work** |
| Peak private commit | 776.82 MiB | 787.02 MiB | 10.20 MiB higher median; overlapping ranges |

Complete-process reference runs were **142.66, 140.63 and 141.14 seconds**; enabled runs were **59.07, 58.57 and 57.59 seconds**. Peak private commit ranged from 762.59–808.73 MiB before and 781.91–787.20 MiB after. The cache therefore produces a substantial time saving, without evidence of a process-memory reduction. The nested export and preparation rows must not be added together. These paired timings supersede the earlier one-run diagnostic comparison for estimating this change's speedup; they are not cumulative reductions to add to previous optimizations.

### Equivalence evidence and limits

The final Release test selection passes **402,593 assertions in 36 cases**. Coverage includes exact fragment ordering, holes, winding cancellation, boundary guards, degenerate paths, unsupported coordinate ranges, concurrent queries, changed masks, reversed inputs, operation-specific cache keys, independent returned results, and storage exhaustion. A separate replay of **3,182 previously captured clipping cases** passes **8,667 assertions** without a changed result.

A complete terrain slice with `ORCA_LOCAL_Z_VALIDATE=1` and both optimizations enabled checks **109,920 intersections and 54,960 differences**, including **54,960 cache hits**, against the original full-mask Clipper calculation. All **164,880 comparisons** pass exact fragment-count, ordering and integer-coordinate equality. There are no base-path leak, mixed-pass height or missing-path warnings. The earlier index-only full-model validation also passes. Validation deliberately executes both calculations and is excluded from performance measurements.

Full G-code equality is a separate criterion: the two unchanged original runs already differ in motion output, with 1,015,087 versus 1,014,703 total lines and print estimates of 3 h 12 m 43 s versus 3 h 12 m 41 s. Both report 45.85 g. These totals do not establish geometric equivalence; the same-input reference checks above provide the direct evidence for the changed clipping operations. This work does not fix the source project's floating-region warning or establish printer validation of its chosen layer heights.

All six final timing runs have identical Local-Z wipe-tower and mixed-pass schedules, and zero tracked Local-Z warnings. They all report 45.85 g and a cost of 1.16; print estimates span four seconds. The normalized full-file comparison fails, including between the three unchanged reference repeats. Thus exact clipping equivalence passes, while whole-file determinism remains unestablished. The comparison tool's nonzero exit denotes that full-file mismatch; all six slicer processes exit successfully without a timeout.

Developer control `ORCA_LOCAL_Z_CPU` selects `legacy`, `index`, `cache`, or `all` in a fresh process; omission means `all`. `legacy` disables only these two new optimizations, retaining previous CPU improvements. Existing Local-Z Vulkan preferences are unchanged. `ORCA_LOCAL_Z_VALIDATE=1` performs reference checks; `ORCA_PREP_PROFILE=1` emits cache diagnostics and `ORCA_LOCAL_Z_PROFILE=<path>` writes clipping counters. No control needs to be set by end users.

Atomic code commits: `db8987b950` (CPU empty-path index) and `a2a071be96` (exact clipping cache). Evidence, raw logs and the sequential experiment driver are retained under `build_logs/terrain_profile_20261021/`. `optimization_results.json` contains the per-run measurements and operation/schedule checks; `comparison.json` records the full-output comparison, including the mismatch; `tests-final.log`, `replay.log` and `validate_combined/` retain the correctness evidence. Reproduce the selected tests with `libslic3r_tests.exe "[LocalZ],[CPUSlicing],[ClipIndex]"`; run the terrain reference check with `ORCA_LOCAL_Z_VALIDATE=1` in a fresh slicer process.

### Comparison with upstream OrcaSlicer 2.5.0 alpha

The supplied `Orca-开启混色细分层高导致局部层高过小.3mf` was sliced with the user's portable OrcaSlicer 2.5.0-alpha executable. The two projects contain identical ordered mesh vertices and triangle indices: 532,548 triangles and 267,304 vertices. They have different painting, recipes and settings, so this is an investigation of an observed performance gap, not an output-equivalent implementation benchmark.

Fresh sequential CLI runs, using each project's saved settings, produced:

| Build and configuration | Complete process | G-code export | Output lines |
|---|---:|---:|---:|
| Upstream Orca 2.5.0 alpha, supplied Orca project | 16.06, 10.57, 11.03 s; **11.03 s median** | Approximately 2–3 s in the logged repeats | 1,010,513 |
| Current packaged Snapmaker fork, CPU, original project | **56.18 s** | **43.93 s** | 1,015,105 |
| Current packaged Snapmaker fork, both GPU preferences, original project | **58.58 s** | **45.04 s** | 1,015,185 |

The two current-fork configurations have one new run each; the earlier CPU repeats remain separate evidence. Upstream export measurements are its integer-second log counters, not high-resolution stage timers. Its first run did not capture the log file; subsequent runs used `--logfile`. All runs completed successfully, used isolated data/output directories, and left the original projects unchanged. No preview was rendered. The GPU run used the NVIDIA GeForce RTX 5060 Laptop GPU, with zero Local-Z fallback layers; its reported clipping preparation time was approximately 329 ms, including 207 ms of dispatch time. This comparison establishes no additional GPU gain on this workload.

The Orca project has Arachne walls, three wall loops, four top layers and 15% crosshatch infill, versus Classic walls, two wall loops, five top layers and 20% grid infill in the original. Painted triangle counts are 264,731 versus 267,952. Both use 72 nominal layers and emit actual thinner mixed passes, but the Orca output reports heights down to 0.024 mm versus 0.04 mm here. Output consumption is 39.41 g versus 45.85 g, and tool changes are 43 versus 47. Mixing ranges, paint assignments, line widths and printer profiles differ. These outputs are not equivalent, even though their mesh and overall G-code line counts are close.

The decisive source-level difference is where mixed-region ownership is retained. Upstream's [mixed sublayer exporter](https://github.com/OrcaSlicer/OrcaSlicer/blob/v2.5.0-alpha/src/libslic3r/GCode.cpp#L7659) looks up already-generated geometry for a virtual mixed slot, emits its existing perimeter/infill paths at each component's sub-Z, and [scales flow and reported height](https://github.com/OrcaSlicer/OrcaSlicer/blob/v2.5.0-alpha/src/libslic3r/GCode.cpp#L9206). Its [tool ordering](https://github.com/OrcaSlicer/OrcaSlicer/blob/v2.5.0-alpha/src/libslic3r/GCode/ToolOrdering.cpp#L2258) resolves the virtual slot to physical components and sublayer heights. This mixed-pass emission block does not re-clip every path against painted masks.

Our export loop in `src/libslic3r/GCode.cpp` around line 5705 walks perimeter/infill collections, clips them into each pass mask, and subtracts mixed regions from the base paths. The recent cache and empty-result index reduce that work substantially, but nonempty intersections remain expensive. The source comparison and export timings support this architectural difference as the main remaining cause; they do not assign every second of the cross-project gap to one operation.

The next experiment should retain explicit mixed-region ownership through toolpath generation and directly reuse paths when an entire region shares one mixing schedule. Only boundary-crossing or more complex Local-Z cases should require clipping. This requires preserving connected-region gradient behavior, height limits, mask compensation, seam decisions, mixed infill, tool ordering and wipe-tower scheduling. Generating paths for separate regions may change toolpath topology, so the previous exact-clipping validation alone would not validate that redesign. No production code was changed during this comparison.

Evidence and source snapshots are in `build_logs/orca_terrain_comparison_20261021/`, including `input_comparison.json`, `comparison_summary.json`, `ours_stage_summary.json`, `series.py`, and per-run logs/G-code. Orca input SHA-256: `1f5b155592e68c19553d77f99f20be95f2ab0dd5126ebdd1692af3ff536db8e9`; Orca DLL: `92bdf7493425b949934479164d0745ce5e28fe24c7a556af300792b788e94c8b`; current packaged fork DLL: `f1d318539269b4446751f45e3341a9ccaa2409c84f92cc79936921d80a39eec4`. The source snapshot is the upstream `v2.5.0-alpha` tag.

### Guarded direct reuse of Local-Z paths

The first implementation targets the remaining **Local-Z export** bottleneck. It verifies that every segment of a generated path belongs completely to the current compensated pass mask, then emits the existing geometry with the existing component height/flow calculation. Boundary crossings, holes, boundary touches, degenerate segments, unsupported coordinate ranges and oversized masks retain clipping. The same rule can retain complete base paths outside the exclusion mask. Ordinary export does not use this router.

This is an export-side step toward region-owned emission, not a complete replacement of region generation or the Local-Z planner. Wall generation can merge compatible regions; a virtual filament ID alone is therefore insufficient proof. The bounded-integer spatial index proves ownership of the actual path, after seam placement, against each particular pass's masks. Different islands and cycles keep their existing schedules. Mask construction and per-path ownership checks still occur.

Whole-path reuse preserves the generated vertices and direction instead of reproducing Clipper's direction and point normalization. Fitted arc metadata is discarded, matching the existing linear clipping contract. Width, role, reversal restrictions and inset metadata are copied; component volume scales by the existing `pass_height / source_height` ratio. Source paths remain immutable. The existing seam placement, boundary-fragment ordering, tool scheduling, Z placement and wipe-tower code remain in use. Height/perimeter modulation can remain separate stages, but this work does not port the advanced renderers from the separate FullSpectrum checkout or establish printer validation of those combinations.

Six fresh terrain processes ran sequentially in reference / reuse / reuse / reference / reference / reuse order, using the same Release DLL and unchanged original project. Both GPU options were off. The reference disables only the new path reuse; the previously retained CPU index and exact clipping cache remain active. Validation, clipping counters and stack sampling were disabled for timed runs; both configurations used debug level 4. Normal desktop applications remained open, and clocks were not fixed.

| Measurement, three-run medians | Reference | Path reuse | Observed reduction |
|---|---:|---:|---:|
| Complete CLI process | **53.60 s** | **15.59 s** | **70.9% / 3.44× faster** |
| G-code export | **41.60 s** | **3.06 s** | **92.7%** |
| Local-Z path preparation within export | **39.00 s** | **0.55 s** | **98.6%** |
| Aggregate process CPU work | 96.95 CPU-s | 60.73 CPU-s | 37.4% |
| Peak private commit | 775.71 MiB | 789.45 MiB | No reduction; 13.73 MiB higher median |

Reference totals were **53.60, 54.09 and 53.10 seconds**; reuse totals were **15.59, 15.56 and 15.65 seconds**. Peak commit ranges overlap: 771.17–802.01 versus 781.35–791.77 MiB. Preparation is part of export, not an additional saving to add to it. Each enabled run directly reused approximately **105,000 paths**. In the full validation run, all **60,035 remaining clipping operations** produced empty results; every nonempty routed path in that run used direct reuse.

The new export time is close to upstream Orca's earlier approximately 2–3-second export measurements. This does not make the two projects equivalent: the Orca file still has different painting, recipes, wall generation and other settings. Our complete slice remains about 15.6 seconds versus its earlier 11.0-second median. The remaining preparation stages, loading and export processing are outside the removed clipping bottleneck.

One supplementary terrain run with both GPU options enabled took **16.05 seconds**, including **3.50 seconds export** and **0.80 seconds Local-Z preparation**. Vulkan ran on the NVIDIA GeForce RTX 5060 Laptop GPU, with 21 layers and zero fallback layers. Preparation included approximately 342 ms of accelerator work. This is a compatibility check with diagnostic counters enabled, not a repeated CPU/GPU benchmark; the large measured gain above requires no GPU.

On the user's current U1 project, one fresh reference/reuse pair improved the complete slice **28.24 → 25.21 seconds**, export **11.96 → 9.70 seconds**, and Local-Z preparation **3.16 → 1.08 seconds**. Reuse covered **259,305 paths**. Both outputs report **160.39 g** and have matching Local-Z pass/wipe schedules. This is a separate current-project comparison, not an update to earlier historical U1 figures or a three-repeat performance claim.

The exact clipping API remains unchanged. Its existing exact-order reference checks still apply to fallback paths. For whole-path reuse, `ORCA_LOCAL_Z_VALIDATE=1` independently checks full-mask intersection and subtraction: no segment may be discarded, and retained length must match within one scaled coordinate unit or a relative `1e-10`, whichever is larger. This is a geometric/volume contract, not whole-file G-code equality.

The Release test selection passes **408,090 assertions in 39 cases**, including existing Local-Z planning tests, randomized whole-path coverage/volume checks, boundary fallback, holes, unsupported coordinates, seam-anchor/direction preservation, flow scaling and immutable source data. The three new reuse cases also pass **5,507 assertions** with full-mask validation enabled. Complete terrain and U1 slices with reference validation pass for **105,001** and **259,305** reused paths respectively, plus the exact reference checks for their fallback operations. Neither reports a base-path leak, local height mismatch, missing pass, wipe sequence mismatch or exhausted transition reserve. Validation runs are excluded from timing; the terrain run also overlapped the final build work at its beginning.

All six timed terrain runs have identical Local-Z pass and wipe-tower schedules and report **45.85 g**. A modal output audit finds identical tool sequences and sets of physical Z/tool/role/height groups. U1's positive XY extrusion totals and command counts match, including the per-group totals to floating-point precision. Terrain positive XY extrusion differs by at most **0.13235 mm of filament out of approximately 14,426.67 mm** relative to reference run 1; unchanged reference repeats also vary. The third enabled terrain run has fewer incoming path operations, indicating upstream input variability as well. These totals are diagnostics, not a proof of complete G-code equivalence; the same-input geometry reference checks are the direct evidence for the changed operation.

Path traversal intentionally changes because reuse preserves the generated direction. In terrain pair 1, non-extruding XY travel falls **195.95 → 163.89 m**, travel commands **82,674 → 61,475**, and estimated printing time **3 h 12 m 43 s → 3 h 4 m 18 s**, while extrusion totals remain effectively unchanged. U1 travel falls **1,573.99 → 1,569.07 m**, with **160.39 g** unchanged, and its estimate falls **25 h 19 m 6 s → 25 h 15 m 13 s**. The audit does not execute custom firmware macros, and these are slicer estimates rather than physically timed prints. Whole-file byte equality is neither obtained nor claimed.

Normal builds enable reuse on the CPU without Vulkan or an additional build flag. `ORCA_LOCAL_Z_REUSE=0` selects the preceding clipping implementation for a fresh-process reference run; `1` or omission enables reuse. This control does not disable the previously retained CPU index and exact clipping cache. Evidence and reproduction scripts are under `build_logs/local_z_path_reuse_20261021/`.

Implementation commit: `720a634227`. Release DLL SHA-256: `f502a753ec98f5e42d4040bdc2054a4ae5d5616def526e02f712322598ae33dc`. `run.py` and `series.py` reproduce the experiments; `results.json` contains individual runs and comparisons; `tests.log`, `tests-reference.log`, `validate_terrain/` and `u1_validate/` hold the reference evidence. `gcode_audit_before_1.json` and `gcode_audit_u1_reference.json` retain output grouping, extrusion and travel diagnostics. The development executable was rebuilt; already-open packaged GUI instances were not replaced or restarted.
