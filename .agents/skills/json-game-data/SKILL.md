---
name: json-game-data
description: 'Implement nlohmann JSON 3.12 loading, game definitions, settings, and save serialization while preserving this project''s schemas and resource references.'
---

# JSON Game Data

## Project Map

Read `external/json-3.12.0/README.md` and `docs/mkdocs/docs/` for the specific access/conversion API. `Json` aliases `nlohmann::json` in `src/engine/serialization/serializable_api.hpp`. `FileIO`, `Saveable`, and `Settings` handle persistence. `IAssets::registerDataType` and ADL `from_json` functions in `src/game/items/` load `assets/data/`; `ResourceManager` reads `assets/definitions.json`.

## Schema & Loading Decisions

- Keep required fields explicit with `at().get<T>()` or `get_to`; use `value` with a correctly typed default for optional fields. `value("volume", 100.0f)` returns a float; an integer default changes the requested conversion type. Missing keys and wrong types are different failures; `contains` alone does not validate a type or numeric range.
- Avoid unchecked const `operator[]` on missing keys. Non-const subscripting inserts values and can silently change a document being inspected. Validate object/array shape before traversal and report the file and field for conversion failures.
- Validate game constraints before narrowing numbers: block IDs, opacity ranges, dimensions, and material references must fit their runtime representation. `Block` currently uses an 8-bit counter; do not treat arbitrary JSON counts as safe to assign. Reject duplicate names before they overwrite lookup entries.
- Keep `from_json` adjacent to the owning type for ADL. Review field names against actual fixtures; neighboring code can contain mismatches and is not proof of a schema. Register new data types and update manifest generation when necessary.
- Preserve `Settings::saveVersion`, optional-field defaults, and `extra` data when changing persistence. Test old/minimal settings as well as new round trips. Deserialization currently assigns fields directly and emits signals; ensure validation happens before subscribers receive values.
- Parse at loading boundaries, then use typed data during generation and rendering. Publish immutable loaded data to workers. Avoid JSON parsing or shared JSON mutation in per-chunk hot loops.
- For save robustness changes, check stream failures. `FileIO::writeJson` is unchecked and non-atomic (settings still use it); `FileIO::writeJsonAtomic`/`writeBytesAtomic` write `path.tmp` and rename it over the target, throwing on failure.
- World saves keep only metadata in JSON: `WorldMeta` (`src/game/world/save/world_meta.hpp`) is each world's `world.json`. It stores the 64-bit `seed` as a decimal string, rejects unknown `formatVersion` values, and keeps unrecognized keys in `extra` so they are written back. Bulk world data is binary (`SectionContainer`), not JSON.

## Validation & Sources

Build `app`. Use valid, missing-field, wrong-type, boundary-number, duplicate-name, and malformed fixtures relevant to the change; verify save/load compatibility and material lookup. Review generated manifest diffs.

Primary sources: [value and default types](https://nlohmann.github.io/json/api/basic_json/value/) and the vendored README's access and serialization sections.
