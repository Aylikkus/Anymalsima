---
name: fmt-game-text
description: 'Use fmt 12.1 for game UI text, localized format strings, diagnostics, and allocation-sensitive formatting in this repository.'
---

# fmt Game Text

## Project Map

Read `external/fmt-12.1.0/README.md` and the relevant declarations in `include/fmt/base.h` or `format.h`. CMake links the compiled `fmt` target through `engine` and `i18n`; keep that integration rather than defining header-only mode in individual files.

The localization wrapper is `src/i18n/gettext_wrapper.hpp`; `_F` translates before formatting with `fmt::runtime`. World-preview labels use fmt in `src/game/states/create_world_state.cpp`; the FPS overlay formats text in `src/game/game.cpp`.

## Formatting Decisions

- Prefer literal format strings for developer-owned text so C++20 compile-time checking catches type/specifier mismatches. Use `fmt::runtime` for actual runtime strings, such as gettext output, and handle `fmt::format_error` at an appropriate UI/loading boundary when formats can be edited.
- Keep complete player-facing sentences translatable. Positional placeholders such as `{0}` and `{1}` let translations reorder values. Preserve argument indices, types, and numeric precision in PO catalogs. `_F` is not explicitly selected by the current extraction script; verify catalog extraction when adding it.
- Distinguish machine data from display text: do not use localized presentation to construct JSON numbers, resource keys, or save identifiers. Numeric output is locale-independent unless localization is explicitly requested; choose localized formatting deliberately.
- Avoid formatting per voxel or per mesh vertex. Reuse strings for unchanged labels; consider `fmt::memory_buffer` with `format_to` for measured hot paths. `format_to` does not append a terminating NUL. If using `format_to_n`, its returned total size may exceed the number of characters written.
- Never store `fmt::format_args` from a temporary argument store or queue it for later execution. It borrows argument storage; format an owning string before enqueueing diagnostics, or explicitly own every deferred argument.
- For custom game types, implement a `fmt::formatter` with a const `format` method or `format_as` where appropriate. Keep formatting free of gameplay mutation and heavy resource lookup.

## Validation & Sources

Build the affected target and `app`. Check representative values, large/negative numbers, multiline labels, and English/Russian/Japanese formatting. Run focused allocation measurements only when optimizing a demonstrated hot path.

Primary source: [fmt 12 API](https://fmt.dev/12.0/api/). Confirm exact overloads against the installed 12.1 headers.
