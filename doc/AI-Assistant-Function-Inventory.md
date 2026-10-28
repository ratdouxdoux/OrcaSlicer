# AI assistant function inventory

Draft for product review and process-test planning. Baseline: the current working tree on 28 October 2026, including the DeepSeek, credential-persistence and global-preset changes. This is an implementation inventory, not a statement that every function has completed end-to-end or physical-printer validation.

Companion document: [Proposed process-test scope: effectiveness of print recommendations](AI-Assistant-Test-Scope.md).

## 1. Project Introduction

The assistant is a conversational interface inside Snorca/Snapmaker Orca for understanding an FFF printing project, editing supported models and settings, and explaining an existing slice. It connects to an external AI provider. The model requests actions through a fixed tool interface; local application code checks targets, types, ranges and project freshness before changing anything.

“Project Introduction” here covers the product overview and project entry/context. Opening, importing, saving and exporting ordinary projects remain native slicer workflows; they are not assistant file-management tools.

### Project and assistant functions

| ID | Offered function | Capability boundary |
| --- | --- | --- |
| P01 | Open, hide and resize the assistant sidebar from Edit > AI assistant. | Hiding cancels an active request and retains the conversation in memory. |
| P02 | Natural-language requests, follow-up questions, streamed answers and a new conversation. | Conversation history is session-only. Provider/model changes start a new conversation. |
| P03 | Attach, preview and remove PNG/JPEG images; retain sent images in conversation context. | Up to four attachments per message. No automatic viewport capture, camera access or video analysis. The selected model must support images. |
| P04 | Choose OpenRouter, Anthropic, official DeepSeek, BigModel or BigModel Coding Plan; edit the model ID and view the fixed endpoint. | Access depends on the provider account and model capabilities. A model ID does not change the selected endpoint or billing route. |
| P05 | Save and restore provider, per-provider model and API key; replace or clear a saved key. | Keys use the OS credential store, separated by provider and application data directory. Availability depends on the platform build and credential service. Chat is not persisted. |
| P06 | Inspect model/object/part/instance IDs, names, dimensions, transforms, plate membership, active-plate bounds, text and available physical/mixed filaments. | Context is structured data, not a transmitted mesh or rendered image. Settings tools target active-plate objects. Ambiguous intent still needs clarification. |
| P07 | Show tool activity and edit summaries; stop a request; review a cumulative preview using Apply changes, Dismiss and Revert changes. | Automatic edits are already visible in the project before Apply. Apply accepts them; it does not execute them again. Later conflicting manual edits block restoration. |
| P08 | Export public assistant context and schemas, and import an offline proposal through Settings > Developer tools. | Intended for diagnostics and repeatable tests. Proposal import uses the same local validation and preview path; this is not ordinary project import/export. |

The default provider remains OpenRouter. The direct DeepSeek connection defaults to `deepseek-flash`; model defaults are configurable and may change with provider availability.

A typical workflow is: open a project in the slicer, choose the assistant's edit scope, describe the task, inspect the cumulative live preview, keep or dismiss it, and use the native slicing workflow to evaluate the result.

Saving or exporting a project while a live preview is pending can include those visible edits. Dismiss the preview first if the edits should not be saved. Starting a new chat dismisses a pending preview; accepted changes remain.

## 2. Model Editing

### Automatic model operations

The following 17 operations are exposed through `propose_model_operation` and can update the cumulative preview after validation. Work occurs on the current project, using stable IDs and fresh context.

| ID | Operation | Offered behavior and boundary |
| --- | --- | --- |
| M01 | `transform` | Move, rotate, scale, resize to XYZ dimensions or mirror an object instance or part. Uses mm/degrees and the target centre; an explicit instance can be targeted. |
| M02 | `drop_to_bed` | Place the object's instances on the build surface. |
| M03 | `add_primitive` | Create cube, cylinder, sphere, cone, torus or wedge geometry. Create a standalone solid object or add a solid, negative, modifier, support blocker or support enforcer to an existing object. New standalone shapes default to the active plate centre with their bottom on the bed; an explicit position denotes the world-space centre. |
| M04 | `add_text` | Add editable native raised or engraved text to a flat axis-facing surface. Supports face, size, depth, bold style, offset and rotation. Curved placement requires the native editor. |
| M05 | `edit_text` | Replace an existing native text part and place it on the specified face. It is not a general converter for arbitrary imported lettering meshes. |
| M06 | `duplicate` | Duplicate an object with new IDs and a world-space offset for each copy; 1–20 copies per operation. |
| M07 | `delete` | Remove an object or specified part through the preview workflow. Removing the last solid part requires deleting the whole object. |
| M08 | `rename` | Change an object or part name. |
| M09 | `set_part_type` | Convert a part to solid, negative, modifier, support blocker or support enforcer. At least one solid part must remain. |
| M10 | `assign_filament` | Assign an available physical or mixed filament to an object or part. Does not load an arbitrary filament profile or change the physical printer. |
| M11 | `paint_surface` | Paint color, support, seam or fuzzy-skin channels on whole mesh facets selected by face direction and/or a world-space box. Supports channel-appropriate painting, enforcement, blocking and erasing. Other facets retain their painting. Precise brush strokes require the native editor. |
| M12 | `split_parts` | Split a disconnected volume into parts while preserving its transform. |
| M13 | `boolean` | Union, subtract or intersect two parts of the same object. Consumes the second part and replaces result geometry; painting/text metadata on the result is cleared. |
| M14 | `simplify` | Reduce a part's triangle count with the native algorithm, using a 1–95% requested reduction. Clears painting/text metadata on that part. |
| M15 | `cut` | Cut an object with a world-space plane and retain the upper side, lower side or both. Connectors and dovetails require the native cut editor. |
| M16 | `create_mix` | Create a native recipe from loaded, compatible physical filaments and optionally assign it. Supports component percentages, simple/layer-cycle distribution, manual patterns and gradients. Components must be distinct and percentages total 100; three or more components require layer-cycle distribution. |
| M17 | `match_color` | Use the native color solver to choose a recipe for a target RGB color, then create/optionally assign it. Preview color is approximate; physical color accuracy is not guaranteed. |

Automatic boolean, simplify, cut, add-text and edit-text operations reject objects exceeding 500,000 triangles. Other limits and parameter types come from the operation schema. Creating a modifier does not make modifier-specific settings editing an assistant capability.

### Native editor and action handoffs

These are two additional operation types. The assistant offers a separate workspace action that the user activates. Availability depends on selection, printer technology, platform features and whether another editor/job is active. Opening or dispatching a tool is not proof that its work completed.

| ID | Operation | Complete offered choices |
| --- | --- | --- |
| M18 | `open_editor` | Move, rotate, scale, lay flat, cut, boolean, supports, seam, fuzzy skin, color painting, text, SVG, measure, assembly, simplify, brim ears, variable layer heights, color mixing and repair. |
| M19 | `native_action` | Arrange, orient, split into objects, merge objects, separate instances, fill the bed with instances and fit to the print volume. |

An outstanding automatic preview must be accepted or dismissed before a native handoff. Arrange/orient may continue as background jobs; other actions may open native dialogs. Repair is subject to the native build/platform's availability. Native actions are not included in the assistant's cumulative automatic-preview transaction.

## 3. Configuration Modification

### Writable scopes

| ID | Scope | Target and boundary |
| --- | --- | --- |
| C01 | Object | Sparse settings overrides on a supported object on the active plate. A sole object can be inferred; multiple objects require explicit IDs. |
| C02 | Global process | The currently edited shared print/process preset. Works without an object, including on an empty plate or with complex objects. |
| C03 | Global filament | The currently edited filament preset. Does not select an arbitrary filament slot automatically. Works without an object. |
| C04 | Global printer | The currently edited printer preset. Works without an object. Does not change connection credentials or submit printer commands. |

The user selects the writable scope in the assistant dropdown. The assistant can inspect public values from other scopes, but cannot switch the dropdown itself. Global preset edits do not remove plate, object or part overrides; those overrides may mask the resulting effect. Presets are not automatically saved as permanent user profiles.

### Configuration functions

| ID | Offered function | Capability boundary |
| --- | --- | --- |
| C05 | Read current settings and search the catalog by key, label, category or description. | Empty search lists the current catalog. Values must be interpreted according to scope and override source. |
| C06 | Propose typed settings changes with explanations and dependencies. | Only keys exposed in the current catalog are editable. Types, ranges, enum choices, array lengths and native configuration dependencies are validated locally. |
| C07 | Apply validated settings to the cumulative preview; refresh native controls and slice validity. | A failed operation must not leave a partial settings patch. Earlier successful steps in the same preview may remain available. |
| C08 | Keep, dismiss or revert the settings preview. | Revert must not overwrite later conflicting manual edits. Native preset normalization that changes additional unproposed values causes rollback. |

The catalog covers the native editable FFF settings that pass the assistant's scope and visibility filters: layer height and line width; walls and wall generation; infill and solid surfaces; seams and surface effects; supports, rafts, brims and adhesion; bridging and overhang behavior; speeds, acceleration and jerk; temperature, cooling, flow and retraction; multimaterial behavior; machine geometry and limits; and custom G-code fields where exposed.

This inventory is complete at the function/operation level. Individual parameter keys are generated from native configuration definitions and the selected presets, rather than a separate fixed assistant allowlist. For a release's exact parameter inventory, export the context for every scope and representative printer/filament profile, or collect `search_settings` with an empty query. Keep key, type, unit, range, enum values, vector length and value source with the test baseline.

Object-scope mutation currently requires solid geometry and one instance. Solid multipart objects and negative volumes are supported, including supported native lettering. Variable layer height, height-range overrides, settings/support modifiers, material-specific overrides and part-level settings other than extruder assignment block automatic object-settings edits. Inspection remains available. These restrictions do not block global preset edits and do not define the eligibility of independent model tools.

Excluded configuration includes SLA settings, read-only/internal fields, preset inheritance/bookkeeping and printer connection credentials. Editing a custom G-code setting changes configuration text; it does not execute the commands or edit an already generated G-code file.

## 4. Slicing Process Control

The current implementation offers slice inspection and workflow guidance. It does not expose an assistant-controlled slicing-job lifecycle.

| ID | Offered function | Capability boundary |
| --- | --- | --- |
| S01 | Report active-plate slice availability. | Distinguishes available, unavailable, processing, stale, needs-slice and saved-but-not-loaded states. Refuses to read results while jobs or configuration updates make them unsafe/stale. |
| S02 | Summarize an existing completed slice. | Reports estimated duration, time by extrusion role, filament volume/length/mass where available, native warnings and available conflict/out-of-bed/bed-mismatch information. Estimates are not physical measurements. |
| S03 | Inspect a band around a specified Z height. | Reports move counts, extrusion roles, speed, fan, temperature, line width, layer height and volumetric-rate ranges; bounded move samples and relevant G-code lines. The band covers the whole active plate, including repeated heights in sequential printing. It does not isolate an object's complete path. |
| S04 | Read a numbered excerpt from the application's current slice G-code. | Requires a fresh slice ID. At most 200 lines/64 KiB per call; individual long lines are truncated. Does not accept arbitrary file paths, write files or execute G-code. |
| S05 | Explain likely causes and next steps using slice evidence, settings and attached images. | Must distinguish observations from hypotheses, explain unavailable/stale evidence and ask for relevant missing information. Cannot verify physical print success. |

No assistant tool currently starts, cancels, pauses or waits for a slicing job; runs automated comparison slices; exports a G-code file; submits/starts/stops a print; monitors a camera; or closes a calibration loop. The native slicer still owns those workflows. Applying edits notifies the native background-processing system and invalidates previous results; this does not constitute an exposed assistant slicing-control API.

## Tool interface and evidence

The complete top-level tool interface is:

| Tool | Function IDs covered |
| --- | --- |
| `get_model_context` | P06 |
| `list_model_tools` | M01–M19 discovery and availability |
| `propose_model_operation` | M01–M19 |
| `get_print_context` | C01–C05 |
| `search_settings` | C05 |
| `propose_settings` | C06–C08 through the preview workflow |
| `get_slice_context` | S01–S03 |
| `read_gcode` | S04 |

S05 and conversational behavior are model behavior guided by the returned evidence, not separate tools. Provider setup, image selection and preview controls are application UI functions.

Sources of truth: [tool contracts](../src/slic3r/Utils/AIPrintAssistantChat.cpp), [model-operation catalog](../src/libslic3r/AIModelTools.cpp), [native adapters and slice availability](../src/slic3r/GUI/AIModelTools.cpp), [settings validation](../src/libslic3r/AIPrintAssistant.cpp), [settings catalog filters](../src/libslic3r/AIPrintAssistantCatalog.cpp), [slice inspection](../src/libslic3r/AISliceTools.cpp), [sidebar and preview controls](../src/slic3r/GUI/AIPrintAssistantDialog.cpp), and [credential storage](../src/slic3r/Utils/AIPrintAssistantCredentials.cpp).
