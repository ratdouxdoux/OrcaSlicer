# AI assistant release notes

Portable Windows builds of Snapmaker Orca with the AI assistant, from the `feature/ai-print-assistant` branch. Newest first.

For how to use the assistant, see the [product guide](AI-Assistant-Product-Guide.md) and the [sidebar reference](AI-Print-Assistant.md).

## v0.5 — 3 November 2026

`Snapmaker_Orca_Windows_v0.5_portable.zip`, 230.8 MB. Commit `6f80dc61ca`.

### Longer replies

- Every provider now allows up to 64,000 tokens per reply, up from 8,192 (OpenRouter, Anthropic) or 16,384 (DeepSeek, BigModel). All the default models reason by default, and reasoning counts against this limit, so long replies, such as OpenSCAD code, could be cut off before. Replies stream, so the higher limit costs nothing unless a reply uses it.
- A streamed reply can now be 32 MB instead of 2 MB. Each token arrives in its own event of up to about 250 bytes, so the old 2 MB cap was the real limit at roughly 8,000–17,000 tokens.
- While a reply streams, the app copies only the part that has just arrived, instead of the whole reply so far on every update.

### Known limits

- The non-streaming proposal request used by **Settings > Developer tools** still asks DeepSeek and BigModel for at most 16,384 tokens.

## v0.4 — 3 November 2026

`Snapmaker_Orca_Windows_v0.4_portable.zip`, 230.8 MB. Commit `d2bdd4cdd9`.

### Design models with OpenSCAD

- New `generate_model` tool. Ask for a part, such as "Design a phone stand with a cable slot", and the assistant writes OpenSCAD code. The app runs it and puts the model straight on the plate, centred, with its bottom on the bed.
- A card in the chat shows what was made, with a **Discard** button that removes it.
- New `get_model_source` tool. Later messages can change the design ("make it 10 mm taller"). The assistant rebuilds it in place and keeps its position, filament and the object's other parts.
- Claude also receives a rendered picture of the result, so it can check its own work.
- Safety:
  - The code can't read files: `import`, `include`, `use`, `surface`, `file=` and the `dxf_*` forms are rejected.
  - OpenSCAD runs as a separate process in a temporary folder that's deleted afterwards.
  - A build is stopped after 90 seconds. On Windows, memory is capped at 4 GB.
  - Models over 1,000,000 triangles are rejected.
- OpenSCAD 2026.11-RC0 is bundled in the `openscad` folder, with its licence notice, which is why the zip grew by about 65 MB.
- The chat's welcome screen suggests "Design a phone stand with a cable slot".

### More reliable requests

- A reply now times out after 90 seconds with no data, rather than 180 seconds in total. Providers that send keep-alive messages while a request is queued are no longer cut off. A reply can run for up to 30 minutes.
- Busy, overloaded and dropped requests are retried automatically up to three times: HTTP 408, 425, 429, 500, 502–504, 520–524, 529, and connection failures. The app waits as long as the provider's `Retry-After` header asks, or 2, 5 and 15 seconds. It retries only while the reply has shown nothing yet. The status line says "Waiting for *provider* to respond".
- OpenRouter and Anthropic replies can be up to 8,192 tokens, up from 4,096.

The assistant now has 13 tools.

## v0.3 — 3 November 2026

`Snapmaker_Orca_Windows_v0.3_portable.zip`, 164 MB. Commits `9295b707c6` to `f79f8a7b78`. An earlier build from 2 November, before the 2.4.1 merge, is kept as `Snapmaker_Orca_Windows_v0.3_portable.pre-merge.zip`.

### The assistant can slice

- New `slice_plate` tool. The assistant slices the plate the same way **Slice plate** does, including its confirmations, and waits for the result. It then reads the summary, the slicer's print warnings and the G-code checks. If the slice is already up to date, it reuses it. If slicing fails, doesn't start or the plate changes meanwhile, the assistant is told why.
- Every message now carries the slicer's own checks:
  - mesh repair and placement problems for each object;
  - the last validation result and anything blocking a slice;
  - while a slice is current, its warnings.

### Snapmaker knowledge base

- New `search_knowledge` tool. It searches a knowledge base bundled with the app, on your computer, and defaults to the active printer's family.
- The knowledge base includes Snapmaker's U1 print quality corpus, translated to English and merged with the other sources. Where sources disagree, the entry says so. It has 843 entries.

### Asking before switching scope

- New `request_edit_scope` tool. When a change belongs to a different scope, for example a filament setting while **Object** is selected, the assistant explains why. A card in the chat offers **Switch** and **Keep**. The scope changes only when you click **Switch**.

### Other changes

- Editing the project by hand while the assistant works no longer blocks it. Your edit ends the live preview and keeps both sets of changes. The chat shows a "Changed manually" notice, and the assistant reads the project again before its next step.
- The wipe tower preview shows only physical filaments. Mixed filaments no longer add bands in filament 1's colour.
- Based on Snapmaker Orca 2.4.1.
- The Connect web UI is updated to 1.8.5.

The assistant now has 11 tools.

## v0.2 — 28 October 2026

Commit `2d60ae2f6b`.

### DeepSeek

- DeepSeek's own API is now a provider, with `deepseek-flash` as the default model. You can change the model field.
- Thinking is on, and its reasoning is kept across tool rounds and later messages, as DeepSeek requires. Replies can be up to 16,384 tokens.

### Saved settings and keys

- The provider and each provider's model are remembered after a restart.
- API keys are saved for each provider in the operating system's credential store: Windows Credential Manager, macOS Keychain, or Linux Secret Service. They're never written to app preferences or project files. To remove one, clear the key in **Settings** and save.

### Global scopes

- The scope selector now offers **Object**, **Global process**, **Global filament** and **Global printer**.
- The global scopes edit the shared presets directly and don't need an object. They work on an empty plate and with objects the Object scope can't edit.

## v0.1 — 22 October 2026

Commit `065727f3b1`. The first version of the assistant.

### The sidebar

- Open it from **Edit > AI assistant**. It docks on the right and can be resized.
- Replies stream in and support headings, lists, code, quotes, links and tables.
- You can attach up to four PNG or JPEG images per message.
- The **Activity** row groups what the assistant inspected and changed in each reply. Expand it to see each tool call.

### Providers

| Provider | Default model |
| --- | --- |
| OpenRouter (default) | `deepseek/deepseek-v4.1-flash-1017` |
| Anthropic | `claude-sonnet-5-5` |
| BigModel | `glm-5.3-flash` |
| BigModel (Coding Plan) | `glm-5.3-flash` |

### What it can do

- **Read the project.** Every message includes the active plate's objects with stable IDs, names, sizes and positions. You don't need to select anything; if one object is on the plate, the assistant uses it.
- **Change settings.** It can search every editable FFF setting, including walls, infill, supports, temperatures, speeds, cooling, retraction and custom G-code. It changes them in the scope picked in the selector: **Object**, **Process**, **Filament** or **Printer**.
- **Edit models.** It can:
  - move, rotate, scale, mirror and drop objects to the bed;
  - add cubes, cylinders, spheres, cones, tori and wedges;
  - add raised or engraved text;
  - duplicate, delete, rename and split;
  - run boolean operations, cuts and simplification;
  - assign filaments;
  - paint colour, supports, seams and fuzzy skin;
  - create mixed filaments and match colours.
- **Open native tools.** It can open the slicer's own editors and actions, such as the cut, text and painting tools, arrange and orient, for you to use by hand.
- **Read an existing slice.** It can read warnings, time and material estimates and the toolpaths at a given height, and read numbered G-code excerpts.

The assistant has 8 tools.

### Live preview

- Changes appear in the slicer at once, as one combined preview, while the assistant works through the steps.
- **Apply changes** keeps them all. **Dismiss** restores the models, mixed filaments and presets from before the first step. **Revert changes** stays available after you apply.

### Limits in this version

- API keys are kept only until the app closes.
- Replies can be up to 4,096 tokens, or 16,384 for BigModel. A streamed reply can be at most 2 MB.
- A request fails after 180 seconds in total.
- The assistant can't slice or send prints.
