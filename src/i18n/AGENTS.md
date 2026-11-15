# Localization Guidelines

See the [repository guidelines](../../AGENTS.md) for shared conventions.

## Responsibilities & Structure

`gettext_wrapper.hpp/.cpp` wraps GNU gettext and fmt. `i18n::init()` binds the `game` domain and UTF-8 encoding; `setLanguage()` changes locale state. `_()` translates immediately, `__()` marks a string for later translation, and `_F()` translates and formats a message.

Translation sources are in `../../translations/`. Runtime catalogs are `../../assets/i18n/<locale>/LC_MESSAGES/game.mo`, with Russian and Japanese catalogs compiled by the repository script. English strings also have a PO source catalog.

## Making Changes

Keep message IDs stable where possible. Preserve fmt placeholders in every translation and ensure arguments match the translated format. Deferred labels must be translated again when the language changes; check enum label caches in `../game/utils/` and UI rebuilding behavior. Locale changes affect process-wide state, so review concurrent callers before moving language switching between threads.

## Build & Validation

Run commands from the repository root: `bash update_i18n.sh` extracts `_` and `__` messages and merges catalogs; `bash compile_i18n.sh` builds runtime catalogs. If adding `_F` messages, verify extraction because the current script explicitly selects only `_` and `__` keywords. Build `app` afterward to copy catalogs. Check English, Russian, and Japanese switching, formatted labels, and CJK glyph rendering.
