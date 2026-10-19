# Snapmaker Orca performance improvements

Measurements: 12–16 October 2026. Sharing version prepared 19 October 2026.

The largest improvements came from removing repeated CPU geometry work, reusing immutable results within a slice, and parallelizing preparation while preserving ordered output. Vulkan provided a useful additional improvement for Local-Z clipping. Several other GPU experiments were correct but slower after transfers, synchronization and remaining CPU work were included.

Across the recorded beginning and end points, **Starry Night with Local-Z fell from 320.42 to 105.97 seconds: 66.9% less time, or 3.02× faster**, saving about **3 minutes 34 seconds per slice**. Peak slicing private commit fell from **8.10 to 4.18 GiB**. These cumulative observations include the CPU improvements, both GPU options enabled at the final endpoint, and intervening correctness fixes; they are not an output-equivalent A/B benchmark. The cumulative comparison at the end gives the timing, CPU and memory breakdown.

**Without Local-Z, Starry Night fell from its earliest measured 114.85 to 72.76 seconds: 36.65% less time, or 1.58× faster.** Aggregate CPU work fell **42.9%**, and peak slicing private commit fell **7.95 → 3.91 GiB**. The main gains came from CPU shell reuse, painting preparation, tree-support processing, classic walls and seam caching. Local-Z GPU clipping contributes nothing to this workflow. This earlier ordinary baseline already contains the first CPU cleanup, so it does not measure every improvement from the project's beginning.

This report covers preview memory, CPU slicing, Local-Z, Vulkan experiments, painting preparation, wall generation and G-code export. It consolidates the existing measurements; it does not represent a new benchmark run. Palette, color-calibration and other product features are outside its scope except where their behavior affects benchmark comparability.

Here, **Local-Z** is the mixed-filament workflow that adds local sublayers and associated material-mask clipping and tool-change planning. **Ordinary slicing** has that workflow disabled; it can still include painted models and multiple materials. **G-code export** includes preparing paths, generating printer commands, filtering them and estimating motion time, as well as writing the output file.

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

These rows come from different controlled rounds and cannot be added together. The parser replay is a component measurement, not an isolated whole-export gain.

Applicability depends on the workload. Painting-specific changes need a painted model; sharing across material regions needs eligible repeated geometry. Classic-wall changes do not accelerate Arachne, tree-support changes need that support path, seam caching benefits the alignment modes, and the velocity parser change applies to Klipper commands. Shared shell, wall, seam, export and preview-memory paths can also serve unpainted or single-material jobs, but this series does not establish a universal percentage for those jobs.

All these retained CPU improvements are enabled in normal builds without either GPU checkbox. Optional GPU seam visibility also runs without Local-Z, but its measured saving is only a few tenths of a second and it has not established a complete ordinary-slice gain. Local-Z planning/clipping, Local-Z wipe eligibility and the new Local-Z-only distance batch cache are inactive in an ordinary slice. The ordinary cumulative comparison separates the full historical change from the individual experiments above.

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

Most timing comparisons contain two observations per configuration; the U1 shortcut retry contains three. With two observations, the arithmetic mean and median coincide. Ranges are observed variation, not confidence intervals. Workloads ran sequentially in fresh output/data directories, with reference validation and stack sampling separated from timing unless explicitly described otherwise. The first U1 shortcut timing series was invalidated by a concurrent user slice and was excluded completely.

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

The memory fix attaches interpolation samples only to real arc moves, clears stale scratch state, and releases excessive preview/result capacity. The compaction policy targets waste of at least 1 MiB exceeding 25% of live storage, avoiding constant shrink/grow churn for modest headroom.

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

## 2. Initial CPU cleanup and profiling

The initial CPU changes made read-only extrusion traversal borrow leaves instead of cloning them, parallelized independent mixed-filament channels, removed unnecessary geometry copies, and made Local-Z diagnostic dumps opt-in. Layer order within each activation channel remained sequential. A common CLI-loading fix was included in both benchmark builds and received no optimization credit.

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

## 3. Local-Z CPU geometry and wipe-tower planning

The next CPU changes reduced repeated geometry work while retaining the integer clipping algorithm:

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

This path applies only to Local-Z export. An ordinary slice dispatches no Local-Z clipping work.

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

## 6. Tree support and parser follow-up

This phase compiled Klipper velocity-limit regex patterns once, reused a lower-layer bridge-support outline across material regions, removed shared writes from tree-overhang summaries, and ran independent final overhang filtering with at most four TBB workers. Ordered reduction publishes results after workers finish. Cross-layer sharp-tail propagation remains sequential.

| Two-run median | Ordinary before → after | Local-Z before → after |
|---|---:|---:|
| Tree-support stage | 11.22 → 6.31 s | 11.47 → 5.53 s |
| Complete process | 98.79 → 96.91 s | 140.71 → 136.16 s |

A separate same-executable ordinary control measured the final filter at **2.472 seconds serial**, versus **0.666–0.688 seconds parallel**. The whole support stage includes bridge-outline reuse and other work, so the entire stage reduction cannot be credited to that loop alone. Whole-job gains of 1.9%/3.2% were less decisive than the stage evidence.

Sampled inclusive CPU time in Klipper velocity parsing fell **6.00 → 1.99 CPU-seconds**, but export elapsed time in those diagnostic profiles stayed approximately **26.45/26.47 seconds**. That parser was saving work without being the pipeline's critical bottleneck. A later direct scanner supersedes the regex-based implementation.

Serial-versus-parallel validation compared all 1,257 layers in both Starry configurations. The synchronized expensive-detail cutoff still depends on timing; this change did not establish global deterministic slicing.

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

For diagnostic comparisons, `ORCA_PREP_EXPERIMENTS=none` disables the switchable preparation paths, and `ORCA_SLICE_EXPERIMENTS=none` selects reference implementations for the latest pass. Neither removes every historical optimization. Expensive validation, captures and debug dumps must be disabled for comparable timing runs.

## Correctness, limits and remaining work

The acceptance process combined deterministic fixtures, exact within-run comparisons, complete model slices and output comparisons. Important findings were retained even when they limited the performance claim:

- Early full G-code comparisons failed, including repeated unchanged baselines. The first Local-Z CPU experiment also showed a larger unresolved internal-solid-infill difference; it was not declared harmless.
- Later preparation fixes removed the tested thread-dependent segmentation differences. Exact ordinary preparation snapshots matched, while downstream ordinary infill still varied.
- In the latest pass, **every final U1 and Local-Z output matched the original baseline** after generation timestamp and runtime object-ID normalization. One of the two final ordinary outputs also matched; the other varied in gap/internal-solid fill and had a one-second print-time estimate difference. This does not establish complete ordinary determinism or prove every difference pre-existing.
- The latest selected regression suite passed **240 cases**, with **two pre-existing expected failures**; reference-mode checks passed **23 cases / 186,876 assertions**. Release builds and all three final in-slice validation jobs passed.
- No physical-print validation or cross-platform production certification follows from these CPU/GPU experiments.

The strongest remaining targets are the stateful export pipeline, nonempty Local-Z clipping, painting Voronoi/Boolean geometry, and curl/distance processing. Curl estimation still depends on the previous layer. Travel, active tool, seam decisions, retraction, cooling, pressure and emitted stream state constrain export ordering. Independent preparation can run concurrently, but the entire layer generator cannot simply become an unordered CPU loop or GPU kernel.

Any further GPU experiment should use realistic batches and include structure construction, packing, transfer, synchronization, CPU fallback and teardown. It should then demonstrate a repeated complete-slice improvement over **current CPU defaults**, rather than an older uncached baseline. The failed experiments already show why a large CPU sample count or a fast isolated shader does not guarantee a useful reduction in elapsed slicing time.

## Cumulative comparison from first measurements to latest build

### Starry Night with Local-Z

The starting point is the first benchmark **before the slicing-performance changes**, including the common CLI-loading fix. It already includes the preview-memory fixes, which are compared separately below. The ending point is the final wall/export build, with all retained CPU improvements and **both Vulkan preferences enabled**. Both endpoints use the original Starry Night input and the same machine, with two fresh-process timing runs each.

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

### Memory from the initial investigation

| Measurement | Before memory fixes | After memory fixes | Observed reduction |
|---|---:|---:|---:|
| Live sliced-preview working set | 5.776 GiB | 2.549 GiB | 3.227 GiB / 55.9% |
| Isolated parser, fresh allocations: private commit | 923.7 MiB | 296.7 MiB | 627.0 MiB / 67.9% |
| Isolated parser, previously oversized buffers: private commit | 1,561.1 MiB | 292.2 MiB | 1,268.9 MiB / 81.3% |

The live-preview row compares the original Intel-rendered snapshot with the later NVIDIA-rendered snapshot after the memory fixes. It is an observed beginning-to-after-fix comparison, not a final-build rerun of an identical GUI session: move counts, graphics residency and allocator history differ. The two parser rows are controlled same-input comparisons and provide the stronger evidence for the memory fixes themselves. They are alternative scenarios, not amounts to add together. The final slicing peak in the preceding table measures a different process phase from retained GUI-preview RAM.

### U1 from its earliest clean baseline

U1 uses Local-Z and is a separate model from both Starry configurations. Its earliest clean CPU baseline already includes the earlier CPU reuse/support work: **38.55 seconds**, the median of three shortcut-retry runs. The final two-run mean with both GPU options enabled is **28.90 seconds**, an observed reduction of **9.65 seconds / 25.0%**. This is progress from a later checkpoint, not a pre-all-improvements baseline or a non-Local-Z result.

As with the other historical comparisons, sample sizes differ and complete output equivalence across the interval is not established.
