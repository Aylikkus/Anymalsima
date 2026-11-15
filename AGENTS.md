# Repository Guidelines

## Project Structure & Module Organization

This C++20 voxel-world application uses SDL3, bgfx, and Vulkan. `src/app/` contains the entry point; `src/engine/` provides core systems, generation utilities, animation, and serialization. Game states and voxel generation, meshing, streaming, and lighting live in `src/game/`, particularly `src/game/world/`. Rendering, platform integration, audio, localization, and UI have separate source directories and CMake targets.

Runtime resources and JSON definitions live in `assets/`; shader sources live in `shaders/`. Translation sources are in `translations/`, and reference images are in `screenshots/`. Dependencies belong in `external/`, and build output belongs in `build/`.

When implementing/refining new/existing features read `IDEA.md` for lore-specific details and vision of the project as a whole.
You can edit `IDEA.md` only after approval and careful planning with me. 

All `CLAUDE.md` files are respective symlinks to `AGENTS.md` files. `.claude` directory is a symlink to `.agents` folder to respect development with various coding agents.

## Module Guides

Follow the guide for the source module you are changing:

- [App](src/app/AGENTS.md): startup, subsystem wiring, and the frame loop.
- [Audio](src/audio/AGENTS.md): sound effects, music, and volume controls.
- [Engine](src/engine/AGENTS.md): shared interfaces, utilities, and serialization.
- [Game](src/game/AGENTS.md): game states, world generation, and voxel systems.
- [Localization](src/i18n/AGENTS.md): gettext integration and translation catalogs.
- [Platform](src/platform/AGENTS.md): SDL windows, input events, and timing.
- [Render](src/render/AGENTS.md): bgfx rendering, text layout, and resources.
- [UI](src/ui/AGENTS.md): widgets, layout, input dispatch, and ownership.
- [Shaders](shaders/AGENTS.md): shader authoring, compilation, and renderer integration.
- [Tests](tests/AGENTS.md): test placement, gameplay coverage priorities, and conventions.

## Build, Test, and Development Commands

Install CMake 3.20+, a C++20 compiler, and Intl/Gettext. Populate `external/` with the exact dependency directory names listed in `README.md` before configuring.

- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`: configure a local debug build.
- `cmake --build build --parallel`: build the application and copy runtime assets beside it.
- `./build/src/app/app`: run from the repository root so relative asset and settings paths resolve.
- `python3 generate_assets.py`: compile shaders and refresh asset definitions after an initial build provides `shaderc`; verify its configured path first.
- `bash update_i18n.sh`: extract translatable strings and merge PO catalogs.
- `bash compile_i18n.sh`: compile Russian and Japanese catalogs into runtime MO files.

## Asset Management

Keep only runtime resources in `assets/`: CMake's `copy_assets` target copies the entire directory beside the executable. Keep contributor documentation here; do not create `AGENTS.md` inside `assets/` or its subdirectories.

- Place UI images in `assets/textures/`, material textures in `assets/materials/`, audio in `assets/audio/music/` or `assets/audio/sounds/`, and game definitions in `assets/data/<type>/`. Follow existing snake_case filenames and avoid duplicate resource keys.
- Run `python3 generate_assets.py` from the repository root after adding, moving, or removing resources. It compiles shaders and regenerates `assets/definitions.json`; verify its `shaderc` path first. Resource keys derive from relative paths, for example `assets/textures/ui/black/9slice.png` becomes `ui.black.9slice`. Update callers when renaming assets.
- The generator preserves the `fonts` mapping but rebuilds other manifest sections. Maintain font paths and roles explicitly; review generated diffs. New data types require registration in `ResourceManager` and compatible serialization, alongside their JSON files.
- Edit shader sources in `shaders/` and translation sources in `translations/`; regenerate runtime binaries using the commands above. Follow the [shader guide](shaders/AGENTS.md) for compilation checks.
- Rebuild `app` to refresh copied assets, then launch and exercise changed resources. Verify manifest paths, material references, fonts, and audio as relevant. Directory copying does not remove obsolete destination files; check for stale copies after renames or deletions.

## Coding Style & Naming Conventions

Follow `.clang-format`: four-space indentation, attached braces, and an 80-column limit. Format changed C++ files with `clang-format -i path/to/file.cpp`. Use `.hpp` headers, `.cpp` implementations, and snake_case filenames. Existing types use PascalCase and methods generally use camelCase; follow neighboring code for member names. Register new source files in the relevant module's `CMakeLists.txt`.

## Testing Guidelines

Keep project-owned tests in root `tests/`: `unit/game/` and `unit/engine/` mirror production modules, `integration/` covers cross-module behavior, and `fixtures/` and `support/` hold small datasets and shared helpers. See [test guidelines](tests/AGENTS.md) and the [CTest skill](.agents/skills/ctest-game-testing/SKILL.md).

Prioritize crucial gameplay logic: voxel rules, coordinate boundaries, raycasting, lighting, meshing, generation invariants, data/save compatibility, and job cancellation. Default tests should run without graphics or audio devices and execute production logic through narrow dependencies. Add other module tests as needed; use deterministic inputs and meaningful checks that remain active in Release builds.

The layout is established, but no project-owned automated suite, CTest registration, assertion framework, or coverage threshold is implemented yet. Until executable tests are added, build affected targets and manually exercise changed behavior. Record reproduction steps and results; an empty CTest run is not validation.

## Commit & Pull Request Guidelines

No formal convention is established. Use concise, imperative subjects describing the change. Keep pull requests focused, explain behavior and validation, link relevant issues, and attach screenshots for rendering or UI changes. Exclude local build output and third-party dependency trees.

## Library Skills & Maintenance

Use the relevant skill under `.agents/skills/` for library-specific game development. Local paths in these skills are relative to the repository root. Check vendored headers for installed API behavior; online documentation may describe newer versions.

- [bgfx](.agents/skills/bgfx-game-rendering/SKILL.md): GPU resources, mesh uploads, draw state, and shaders.
- [EnTT](.agents/skills/entt-game-ecs/SKILL.md): gameplay components, entity lifetimes, iteration, and synchronization.
- [fmt](.agents/skills/fmt-game-text/SKILL.md): UI text, localization, format checking, and argument lifetimes.
- [JSON](.agents/skills/json-game-data/SKILL.md): game definitions, schema validation, settings, and saves.
- [miniaudio](.agents/skills/miniaudio-game-audio/SKILL.md): effects, streamed music, spatial audio, and resource lifetimes.
- [SDL3](.agents/skills/sdl3-game-platform/SKILL.md): windowing, input, Unicode text, and display scaling.
- [stb](.agents/skills/stb-game-assets/SKILL.md): texture decoding, font metrics, glyph fallback, and atlases.
- [BS::thread_pool](.agents/skills/bs-thread-pool-world-jobs/SKILL.md): chunk jobs, cancellation, result publication, and teardown.
- [Tracy](.agents/skills/tracy-game-profiling/SKILL.md): profiling setup, frame timing, and worker instrumentation.
- [CTest](.agents/skills/ctest-game-testing/SKILL.md): gameplay test design, CMake registration, test isolation, and execution.

Agents may update any skill located under `.agents/`, including existing skills and supporting references, when implementation changes, dependency upgrades, discovered errors, or new project knowledge make an update useful. Keep edits relevant to the current task, verify them against source or authoritative documentation, and maintain accurate triggers and references. Routine skill maintenance needs no separate user confirmation; filesystem permissions still apply. Preserve unrelated guidance and validate changed skills with the skill-creator validator when available.
