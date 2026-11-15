---
name: ctest-game-testing
description: 'Create, register, run, and debug CMake/CTest tests for crucial gameplay logic and shared engine behavior in this repository, including headless target boundaries and deterministic world-job tests.'
---

# CTest Game Testing

## Scope & Project State

Use CTest as the test runner and registration layer; it is not a C++ assertion framework. The project requires CMake 3.20, even if the local installation is newer. No project-owned suite or assertion framework is configured yet. Recheck root/source CMake files before describing commands as operational.

Read [tests/AGENTS.md](../../../tests/AGENTS.md) for the repository's test layout and coverage priorities. Tests belong under root `tests/`, outside `src/`, runtime `assets/`, and vendored dependencies. Local code paths below are repository-relative.

## Designing Gameplay Tests

- Start with observable rules and failure modes: tile/liquid behavior, chunk boundaries, ray hits, light attenuation, mesh-face visibility, settings compatibility, and cancellation. Check the current `Chunk::SIZE_*` and coordinate conventions; comments and README dimensions may be stale.
- Use tiny synthetic chunks, explicit block/material definitions, fixed seeds, and controlled job synchronization. Prefer invariants and selected expected cells/faces over snapshots of entire generated worlds. Floating-point comparisons need justified tolerances; deterministic seeds alone do not guarantee identical results across platforms or scheduling changes.
- Assert failures in Release too. Plain C `assert` can disappear under `NDEBUG`; use an existing framework when adopted, or explicit checks and nonzero failure exits. Include coordinates, inputs, seed, and expected/actual values in diagnostics. Do not add another assertion framework without checking the suite's current choice.
- Execute production logic. Extract a narrow CPU-only target or injectable data-access boundary when needed; do not duplicate algorithms in tests. `game` links UI, and `engine` links SDL/bgfx, so linking those targets does not automatically isolate gameplay. Headless runtime tests may still require vendored dependencies at configuration/build time.
- Keep graphics/audio-device tests separately labeled. Use CPU mesh output for face-count and material-index tests. Fakes should model assets, not start a renderer. For jobs, control completion and cancellation without arbitrary sleeps, bound waits, and observe failures from futures or completion channels.

## CTest Workflow

When implementing test infrastructure, read [references/cmake-registration.md](references/cmake-registration.md) for registration examples, isolation, and commands. Enable tests from the root, register meaningful cases, build test executables before running CTest, and verify discovery. Do not report an empty suite as passing coverage.

Run focused regressions first, then the affected gameplay suite. Verify Release behavior and sanitizer configurations when relevant to changed lifetimes or concurrency. Treat coverage as a gap-finding aid; prioritize rules and boundaries rather than introducing an arbitrary percentage target.

## Sources

Based on [Testing With CMake and CTest](https://cmake.org/cmake/help/book/mastering-cmake/chapter/Testing%20With%20CMake%20and%20CTest.html), with syntax checked against the CMake 3.20 references linked in the registration guide. Revisit upstream documentation when changing tool-version requirements.
