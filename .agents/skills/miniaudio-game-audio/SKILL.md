---
name: miniaudio-game-audio
description: 'Implement miniaudio 0.11.25 game sound effects, streamed music, positional playback, volume, and audio-resource lifetime management in this repository.'
---

# miniaudio Game Audio

## Project Map

Read `external/miniaudio-0.11.25/README.md` and the embedded manual in `miniaudio.h`; inspect the specific function's thread-safety contract before changing cross-thread calls. `src/audio/audio_system.*` implements `IAudioSystem`. `src/render/resource_manager.*` owns decoded `SoundData` and music paths. Effects use independent `ma_audio_buffer_ref` cursors; music uses `MA_SOUND_FLAG_STREAM`.

## Playback & Lifetime Decisions

- Initialize configurations with the matching `*_config_init`; check every initialization result and unwind only resources that were successfully initialized. Track loaded/initialized state before queries, stops, seeks, and cleanup.
- Initialized miniaudio objects must stay at stable addresses. `activeSounds` uses `std::list<SoundInstance>` for that reason. Do not replace it with relocating value storage or move/copy an initialized engine, sound, or buffer reference. Heap-owned stable objects are another option.
- A buffer reference does not own decoded PCM. Keep `ResourceManager` sound memory alive until every referencing sound is uninitialized. Preserve one playback cursor per instance so overlapping clicks do not share seek state. On cleanup, uninitialize sounds before their data sources, free decoded resources afterward, and shut down the engine after its sounds.
- Stream long music tracks; use decoded/reusable data for frequently repeated short effects. Review latency and memory when introducing asynchronous loading, and make completion/error ownership explicit.
- Settings use percentage volumes, while miniaudio takes linear gain. Review `computeSoundVolume` and `computeMusicVolume` to apply master gain exactly once. Test mute and restored volume for sounds that are already playing.
- Keep UI effects non-positional when intended; explicitly review spatialization flags. For world sounds, coordinate listener position/orientation with the gameplay camera and verify units and attenuation distances. Stereo assets do not behave like mono point sources in every spatialization setup.
- Do not allocate, load files, block on game locks, or run gameplay callbacks in a real-time audio callback. Device start/stop/uninit must not be called from the device callback. Send control requests to an appropriate non-audio thread.

## Validation & Sources

Build `app`; test overlapping effects, music replacement, looping, pause/resume, live volume changes, positional attenuation, failed asset loading, and shutdown during playback. For lifetime changes, use sanitizer coverage on the playback/cleanup path where available.

Primary source: [miniaudio programming manual](https://miniaud.io/docs/manual/). Local 0.11.25 declarations and embedded documentation take precedence over newer examples.
