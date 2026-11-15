# Audio Guidelines

See the [repository guidelines](../../AGENTS.md) for shared conventions.

## Responsibilities & Structure

`audio_system.hpp/.cpp` implements `IAudioSystem` from `../engine/audio/audio_system_api.hpp` using miniaudio. It manages active sound effects, music playback, listener position, and master/music/sound volume. `ResourceManager` in `../render/` supplies decoded sound data and music paths.

## Making Changes

Keep playback control here and asset loading in the resource manager. Expose shared playback operations through `IAudioSystem` when callers only need the interface. Maintain the lifetime of resource buffers referenced by active sounds; release miniaudio objects before freeing their backing data.

Preserve the existing percentage volume convention: master, music, and sound settings default to `100.0f`. Inspect the volume conversion methods before changing gain calculations. Keep `update()` responsible for retiring finished sound instances, and review `close()` whenever adding playback resources.

## Build & Validation

From the repository root, run `cmake --build build --target app --parallel` to validate integration with rendering and resources. Launch the application and check UI click sounds, looping music, pause/resume behavior where used, independent volume controls, and exit during playback. For positional-audio changes, exercise listener movement and attenuation.
