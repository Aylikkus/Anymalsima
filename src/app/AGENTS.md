# Application Guidelines

See the [repository guidelines](../../AGENTS.md) for shared build and style conventions.

## Responsibilities & Structure

`main.cpp` is the composition root: it initializes localization, platform, renderer, assets, settings, UI context, and `Game`. It also connects settings signals and drives the frame loop. `CMakeLists.txt` defines the `app` executable and the `copy_assets` target.

## Making Changes

Keep startup, subsystem wiring, and shutdown here; put gameplay behavior in `../game/` and reusable services in their owning modules. Initialize dependencies before passing them into `UIContext` or `Game`. Preserve frame ordering: poll input, handle events, update, collect render commands, submit rendering, update audio, finish the game frame, and clear transient input and commands.

Settings callbacks capture subsystem references; ensure those objects outlive their callbacks. Review shutdown ordering when adding resources so game and renderer cleanup occurs while the platform is available.

## Build & Validation

From the repository root, run `cmake --build build --target app --parallel`, then `./build/src/app/app`. Asset and settings paths are relative to the working directory. Verify startup, menu interaction, world creation, settings changes, and clean exit. Check that the build copies changed assets beside the executable.
