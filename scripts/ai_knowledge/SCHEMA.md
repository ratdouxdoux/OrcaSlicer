# Knowledge entry format for the Snapmaker Orca print assistant

Staging files are JSON arrays of entry objects, UTF-8, no comments, no trailing commas.
The assistant searches these entries with keyword search and shows them to an LLM that
helps users diagnose prints and choose slicer settings. `merge.pl` combines staging files
into `resources/ai_knowledge/snapmaker_knowledge.json`.

## Entry object

```json
{
  "id": "u1-nozzle-clog-cold-pull",
  "title": "Clearing a partly clogged U1 nozzle",
  "category": "troubleshooting",
  "machines": ["u1"],
  "tags": ["clog", "under-extrusion", "nozzle", "hotend"],
  "summary": "Two to four sentences, in your own words, that answer the question on their own.",
  "symptoms": ["Thin or missing lines mid-print", "Extruder clicking"],
  "causes": ["Heat creep after long pauses", "Debris or burnt filament in the nozzle"],
  "steps": ["Heat the hotend to the filament's print temperature", "..."],
  "settings": [{"key": "nozzle_temperature", "advice": "Raise 5-10 C if extrusion stays weak at high speed."}],
  "facts": ["Hotend max temperature 300 C"],
  "sources": [{"title": "Troubleshooting of Snapmaker U1", "url": "https://wiki.snapmaker.com/en/snapmaker_u1/..."}]
}
```

Required: `id`, `title`, `category`, `machines`, `tags`, `summary`, `sources`.
Optional (include when they apply, omit or use [] otherwise): `symptoms`, `causes`, `steps`, `settings`, `facts`.

- `id`: lowercase kebab-case, unique, <= 80 chars, start with your assigned prefix.
- `title`: <= 100 chars, specific (name the machine/component when relevant).
- `category`: exactly one of `machine` (specs, hardware, features), `troubleshooting` (a problem and its fix),
  `maintenance`, `calibration`, `setting` (what a slicer setting does and when to change it), `material`
  (filament behaviour, temperatures, drying, plates), `software` (Snapmaker Orca app, firmware, connectivity), `safety`.
- `machines`: one or more of `u1`, `j1`, `artisan`, `snapmaker2` (A150/A250/A350 and their T, Dual, Quick Swap
  and Bracing Kit variants), or `any` for machine-independent knowledge. Never mix `any` with others.
- `tags`: 3-12 lowercase keywords a user might type: symptoms ("stringing", "warping"), components ("extruder",
  "hotend", "toolhead"), error codes, materials ("petg"), feature names. Include synonyms.
- `summary`: 1-4 sentences, <= 600 characters.
- list items (`symptoms`, `causes`, `steps`, `facts`): each <= 300 characters, at most 12 per list. `steps` are
  ordered actions. Keep exact numbers (temperatures, distances, torques, error codes, part names).
- `settings`: only Orca setting keys defined in `src/libslic3r/PrintConfig.cpp` (`this->add("<key>", ...)`).
  `advice` <= 250 characters says which direction to change it and why. Do not invent keys.
- `sources`: 1 to 16 `{title, url}` pairs you actually read. Wiki URLs look like
  `https://wiki.snapmaker.com/en/<path>` (the header of each page cached by `fetch_wiki.sh` has the exact URL).

## Writing rules (important)

- Write every entry in your own words. Condense; do not copy sentences or paragraphs from a source, and never
  quote more than a short phrase. Facts, numbers, specs, part names and error codes are fine to state.
  Each entry must be much shorter than the material it summarizes.
- One entry = one answerable topic (one problem, one procedure, one setting, one spec sheet). Prefer several
  focused entries over one long one.
- Skip pure marketing, ordering/shipping/warranty text, navigation pages, laser and CNC content, and pages
  that only link elsewhere. Skip steps that only make sense with the source's photos unless you can describe
  them clearly in words.
- Be accurate. Only state what the sources support. If sources disagree, say so in the summary.
- Prefer knowledge a print assistant can act on: symptoms -> likely causes -> concrete checks and fixes, and
  which slicer settings to change in which direction.
