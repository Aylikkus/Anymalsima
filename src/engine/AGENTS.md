# Engine Guidelines

See the [repository guidelines](../../AGENTS.md) for shared conventions.

## Responsibilities & Structure

This module contains shared contracts and utilities:

- `core/`: geometry, input, settings, signals, logging, and platform/asset interfaces.
- `render/`: render commands, targets, vertices, text layout, grid helpers, and `UIContext`.
- `audio/`: the audio service interface.
- `animation/`: tweens and animator implementation.
- `serialization/`: JSON file access, saveable objects, storage interfaces, little-endian `BinaryWriter`/`BinaryReader`, zstd `compression`, the tagged `SectionContainer` save format, and atomic file writes.
- `generation/` and `utils/`: noise, UTF-8, font, and enum helpers.

Many utilities are header-only; compiled implementations are listed explicitly in `CMakeLists.txt`.

## Making Changes

Keep shared abstractions here and concrete platform, rendering, and gameplay implementations in their modules. Coordinate interface changes with all implementations and callers. Preserve settings keys and serialized formats unless a migration is deliberately included. `SectionContainer` files start with `ANYM` plus a container version; each section has a fourcc tag and its own version. Never change an existing section's layout without bumping its version and keeping a decoder for the old one. Readers must throw `SerializationError` on malformed input rather than read out of bounds. Review callback captures and disconnect behavior when changing `Signal` consumers.

Rendering commands are collected into `RenderTarget` and consumed by the renderer; update both sides when changing command data. Keep text indices and UTF-8 handling consistent with UI and renderer expectations.

## Build & Validation

From the repository root, run `cmake --build build --target engine --parallel`, then build `app` to catch downstream interface breakage. Exercise affected behavior, including settings save/reload, input dispatch, animations, text editing, or render-command submission. Serialization has headless CTest coverage: run `ctest --test-dir build -L engine`.
