# Platform Guidelines

See the [repository guidelines](../../AGENTS.md) for shared conventions.

## Responsibilities & Structure

`platform.hpp/.cpp` implements `IPlatform` from `../engine/core/platform_api.hpp`. It owns SDL initialization and the window, translates SDL events into engine `InputEvent` objects, emits resize/close signals, supplies timing and FPS information, and prepares native window data for bgfx.

## Making Changes

Keep SDL-specific event and window handling here. Feed normalized events into `InputSystem`; let game states and UI decide their meaning. Preserve press, release, held-key, modifier, pointer, wheel, and text-input behavior when modifying event translation.

Window resize updates both stored dimensions and subscribers through `onResize`; verify both paths. Native window integration must continue to supply the handles expected by bgfx. Review initialization and teardown ownership before changing `close()`, the destructor, or renderer interactions. Text-input changes should account for SDL UTF-8 strings and the engine's event representation.

## Build & Validation

From the repository root, run `cmake --build build --target app --parallel` and launch in a graphical session. Check window resizing, close events, held movement keys, modifier combinations, both scroll axes, pointer interactions, and text entry. Validate timing or FPS-limit changes while idle and while generating a world.
