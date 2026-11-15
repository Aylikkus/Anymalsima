---
name: sdl3-game-platform
description: 'Implement SDL3 3.4.8 windowing, input translation, Unicode text entry, high-DPI behavior, timing, and native bgfx window integration for this game.'
---

# SDL3 Game Platform

## Project Map

Read `external/SDL3-3.4.8/README.md`, `docs/README-migration.md`, and the relevant `include/SDL3/` header. `src/platform/platform.*` owns SDL video/window lifetime and translates input into `InputSystem`. bgfx performs rendering; avoid introducing a separate SDL renderer into that pipeline.

## Platform Decisions

- Use SDL3 signatures and boolean success conventions; do not copy SDL2 return-value checks or APIs. Inspect `SDL_GetError()` immediately after a failed operation and associate it with that operation. Keep video initialization, event pumping, and window operations on the main thread as documented per function.
- Preserve frame input semantics: distinguish key press/release, held movement, pointer events, wheel direction, and modifiers. Scancodes suit physical movement bindings; keycodes/text serve different purposes. Keep scancodes in a type wide enough for SDL's range; the existing narrowing to `uint8_t` is not a safe pattern to extend.
- Handle focus loss when maintaining held-key sets, so movement does not remain stuck after switching applications. Review key-repeat behavior separately from held-key generation.
- Start/stop text input for the focused text widget using SDL3's window-based API. `SDL_EVENT_TEXT_INPUT` contains UTF-8 text, potentially several bytes/codepoints. Decode the complete string using the project's UTF-8 utilities; do not cast its first byte into a Unicode character. Treat IME composition/editing events separately from committed text if implementing composition support.
- Distinguish window coordinates from drawable pixels. `SDL_GetWindowSizeInPixels` and `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` matter for bgfx backbuffer dimensions; UI coordinates, scale, and pointer hit testing must remain consistent. A logical resize alone may not describe a DPI change.
- Use native window properties from the existing bgfx platform-data path. Preserve renderer-before-window teardown, and measure frame duration with consistent units and monotonic timing APIs.

## Validation & Sources

Build `app` and run in a graphical session. Test resize/DPI changes where supported, focus loss while moving, modifiers, wheel axes, close events, and Russian/Japanese text entry. Validate the actual target window backend rather than assuming all SDL window properties exist everywhere.

Primary sources: [SDL3 migration guide](https://wiki.libsdl.org/SDL3/README-migration), [text input](https://wiki.libsdl.org/SDL3/SDL_StartTextInput), and [threading FAQ](https://wiki.libsdl.org/SDL3/FAQDevelopment).
