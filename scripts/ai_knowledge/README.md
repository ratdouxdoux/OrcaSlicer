# Print assistant knowledge base

`resources/ai_knowledge/snapmaker_knowledge.json` is the knowledge base behind the assistant's
`search_knowledge` tool (`src/libslic3r/AIKnowledgeBase.cpp`). Each entry is an original summary
of one topic (a machine spec sheet, an error code, a print failure, a setting, a material) with
the URLs it was written from. The format and writing rules are in [SCHEMA.md](SCHEMA.md).

Sources: the English Snapmaker wiki sections for the machines Snapmaker Orca supports (U1, J1/J1s,
Artisan, Snapmaker 2.0, the general/Dual Extrusion Module pages, Snapmaker Orca and the FAQ), plus
public printing references (OrcaSlicer, Prusa, Bambu Lab and Klipper documentation, filament data
sheets, the Snapmaker forum). Laser, CNC, Ray and policy pages are out of scope.

The `u1q-*` entries come from Snapmaker's U1 print quality corpus (108 issues from Snapmaker's
internal print-quality log, against Snapmaker Orca 2.4.1), translated from Chinese. That log is not
public, so corpus sources cite the public U1 knowledge base (`https://wiki.snapmaker.com/en/snapmaker_u1`)
and name the corpus issue code in the source title. Where a corpus issue duplicated an existing entry
(or another corpus issue) the two were merged into one entry citing both sources; where they disagree,
the summary names both positions.

The `u1x-*` entries come from Snapmaker's expert print-quality cases: twelve test models with the
features they stress, the settings an expert changed and the improvement seen, plus the list of
process settings whose defaults already print well (`u1x-keep-default-process-settings`; the
assistant enforces that list in `src/libslic3r/AIPrintAssistant.cpp`). The document is internal and
in Chinese, so sources cite the public U1 knowledge base and name the case in the source title. Its
photos are not used. Two cases that share one support recipe are one entry citing both.

## Refreshing

1. `scripts/ai_knowledge/fetch_wiki.sh <cache dir>` downloads the in-scope wiki pages as plain text.
   Pages that need a login are skipped. The cache is working material; never commit it.
2. Write or update entries as JSON arrays in `<cache dir>/staging/*.json`, following SCHEMA.md.
   Summarize in your own words; do not paste wiki or web text. `merge.pl` writes only what is in
   staging, so when adding to the shipped file without the earlier staging files, first save its
   `entries` array as `<cache dir>/staging/00-existing.json`.
3. `perl scripts/ai_knowledge/merge.pl <cache dir>` validates every entry (fields, categories,
   machines, setting keys against `PrintConfig.cpp`, sources), reports entries that share 10 or more
   consecutive words with a cached wiki page, and writes the sorted knowledge base.
4. Run `ai_print_assistant_tests`; the "shipped knowledge base" test loads and validates the file
   with the same rules the application uses.
