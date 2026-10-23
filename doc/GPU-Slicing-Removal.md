# Removal of experimental GPU slicing acceleration

On 23 October 2026, the experimental Vulkan slicing backends were removed from `proposal_UI_changes_GPU_Accelerate_V2`. The later CPU optimizations reduced the work available to offload, while Vulkan added setup, transfer, synchronization and maintenance costs. The measured cube slowdown and second-model comparison are recorded in `SML-GPU-On-Off-Comparison-2026-10-23.md`.

Implementation commit: `63cc06ab84` (`Remove experimental Vulkan slicing acceleration`).

## What changed

- Removed the Local-Z GPU preclassification pass, seam-visibility GPU backend, experimental seam-neighbor prefetch backend and closed-polygon GPU rejection wrapper.
- Removed both GPU controls and their explanatory section from Preferences. Existing saved values for these obsolete preferences are no longer read by slicing code.
- Removed Vulkan loading/queue management, compute shaders, SPIR-V embedding, SDK/header discovery, shader compilation and the Windows build-tools download script.
- Removed hardware-dependent tests and GPU capture/replay branches; retained the CPU geometry/reference tests.
- Kept the CPU Local-Z spatial classifier as `src/libslic3r/LocalZClipDecision.cpp/.hpp`, directly in `libslic3r`. Its implementation is token-identical to the former CPU implementation, ignoring whitespace and comments.
- Kept Local-Z clipping caches, the bounded integer spatial index, whole-path reuse, seam-neighborhood caching, CPU parallelism and the other CPU slicing improvements. Normal OpenGL viewport rendering and GPU selection are unaffected.

## Building and settings

The standard Windows command remains:

```powershell
.\build_release_vs2022.bat slicer
```

It no longer downloads Vulkan build tools, invokes a shader compiler or takes an `ORCA_ENABLE_VULKAN_COMPUTE` option. CMake no longer creates an `orca_vulkan_compute` target. There is no replacement GPU option to enable. The old Local-Z/seam/polygon GPU environment switches have no backend to activate; the retained seam-cache diagnostic setting accepts only `cpu`, `cache` and `profile`.

Previously downloaded SDKs or ignored build-tool caches are not deleted from the machine. Historical benchmark and design documents remain as records and are marked accordingly.

## Validation scope

The removal is checked against the prior build with GPU slicing disabled, not against a build that used GPU seam scoring. CPU reference tests cover Local-Z boundaries, holes, winding, rounding guards, reuse and seam-cache traversal order. The seam-cache test also exercises zero, negative, overflowing, infinite and NaN search radii and a NaN query coordinate against the original CPU tree implementation.

The separate full-mask path-reuse validation mismatch found with the cube's surface-paint settings is not repaired by removing Vulkan. It reproduced with GPU acceleration already off; its recorded CPU-only reproduction remains applicable.

## Checks completed on Windows

- Configured the existing VS2022 Release build after removing the old Vulkan cache entries. The generated build graph contains neither `orca_vulkan_compute` nor shader compiler steps.
- Built `Snapmaker_Orca` and `libslic3r_tests` successfully. The local development build used a batched check of the same 1,128 UTF-8 source files instead of spawning one encoding-check process per file; the release script's encoding checks are unchanged.
- Ran `[LocalZ],[CPUSlicing],[ClipperUtils]`: **41 test cases and 409,469 assertions passed**. This includes the retained integer classifier, reference clipping, path reuse, cache eviction/order and CPU slicing regression tests.
- Sliced the saved `CubeSML.3mf`, the complex terrain Local-Z project and `1MPolygonBridge.3mf` successfully in fresh CLI processes without any `ORCA_*` environment overrides.
- Checked the rebuilt DLL for the removed slicing switches and backend markers. Those are absent. The existing GLFW library still contains its own optional Vulkan-loader string; it is unrelated to these slicing backends and was not modified.

The running packaged application was not replaced. The rebuilt development DLL is `build-dev-release/src/Release/Snapmaker_Orca.dll`, SHA-256 `89ab7e0edbe28b4d6aca22dd58bac14fe50eb4e53b45a3296a3de525e8593b09`. The pre-removal comparison DLL is `9fa50025fd5f645fc3d5304eb1be7a2768763be9356c959b4a38f631ef300feb`, with the GPU backends disabled. Logs and G-code are retained under `build_logs/remove_vulkan_20261023/`.

These runs check build and output behavior. They are not performance measurements: another FullSpectrum build was running during part of this work.

## Output comparison

All three input hashes match their baselines. All three output pairs have the same effective configuration, tool sequence and nominal layer sequence. G-code commands are not identical; unchanged-build repeats already varied before this removal.

The spatial audit groups positive-extrusion centerlines by physical Z, material, role and layer height, using a 0.01 mm tolerance. Missing/added length describes differences between emitted centerlines, not missing/added printed volume.

| Model | Total deposited filament change, signed (mm) | Missing / added centerline (mm) | Reported mass, before / after |
| --- | ---: | ---: | ---: |
| Saved CubeSML | -0.00859 | 742.068 / 741.871 | 29.13 / 29.13 g |
| Terrain Local-Z | +0.00024 | 0.335 / 0.195 | 45.85 / 45.85 g |
| 1MPolygonBridge, ordinary slicing | -0.01393 | 0.727 / 0.209 | 244.79 / 244.79 g |

The cube differences are concentrated in wall paths, matching the scale of the previous unchanged CPU repeat (approximately 737 / 743 mm out of 562,037 mm of extrusion path). The terrain's small differences are confined to internal solid infill and are comparable in scale to the preceding repeated-run comparisons. The bridge reports 82,073.25 versus 82,073.24 mm of filament and the same 26 h 30 m 30 s print estimate; both fall within the recorded V2 repeated-run range. These checks support preservation of the CPU behavior on the tested inputs, but do not establish exact equivalence for every model or resolve the separate surface-paint mismatch above.

`GPU-Slicing-Removal.json` records the input/binary/output hashes, test count and compact audit results. The detailed per-group audits remain with the local logs. The former build's cube and terrain baselines are in `build_logs/sml_gpu_ab_20261023/`; the bridge baseline was generated specifically for this removal with all GPU backends disabled.
