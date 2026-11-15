---
name: tracy-game-profiling
description: 'Profile frame time, voxel generation, meshing, lighting, resource uploads, and worker scheduling with the vendored Tracy 0.13.1 integration.'
---

# Tracy Game Profiling

## Project Map

Read `external/tracy-eb3df3b-0.13.1/README.md`, `manual/tracy.tex`, and `public/tracy/Tracy.hpp`. The root CMake file controls `TRACY_ENABLE` and adds Tracy; `src/app/CMakeLists.txt` conditionally links `TracyClient`. `src/app/main.cpp` already places a guarded `FrameMark` in the frame loop.

The current root configuration forces `TRACY_PROFILING` OFF using a cache assignment with `FORCE`. Passing `-DTRACY_PROFILING=ON` alone does not enable it. When profiling setup is requested, make the option configurable and ensure include paths, definitions, and client linkage reach every instrumented target. Do not claim instrumentation is active from a cache flag alone.

## Measurement Decisions

- Start from a reproducible gameplay workload: record build configuration, seed, world settings, camera route, and warm-up. Use an optimized build with symbols for performance comparisons; keep VSync/FPS cap and asset state consistent.
- Add coarse CPU zones around generation phases, chunk meshing, sunlight columns, completion processing, uploads, and frame update/render. Separate queue wait from execution time. Avoid a zone for every voxel; instrumentation volume can dominate the work being measured.
- Use `ZoneScoped` or literal `ZoneScopedN` names at lexical scope. Use `ZoneText`/`ZoneValue` for per-job coordinates or sizes instead of creating unbounded names. Multiple zones in one scope require named-zone variants or nested scopes. Keep zone construction/destruction on the same thread.
- Keep the existing once-per-frame marker. Worker jobs are zones, not extra main frames. Name persistent workers through the pool initialization hook when useful, without replacing unrelated initialization behavior.
- CPU submission time is not GPU execution time. bgfx owns the graphics backend/context, so do not insert raw Vulkan Tracy macros into arbitrary game code. Check bgfx's profiler support and ownership before integrating GPU timing.
- When adding plots or memory tracking, use consistent units and pair allocation/free events. Do not turn sampled counters or collection overhead into gameplay dependencies. Keep instrumentation compilable with profiling disabled and avoid relying on macro argument side effects.

## Validation & Sources

Build with profiling disabled and enabled after any integration change. Connect a compatible Tracy profiler, confirm zones and frame markers appear, and capture the target workload. Compare frame-time spikes and worker utilization before/after; state whether the bottleneck is CPU work, waiting, upload/submission, or measured GPU work.

Primary sources: [Tracy project documentation](https://github.com/wolfpld/tracy) and [upstream manual](https://github.com/wolfpld/tracy/blob/master/manual/tracy.tex). Resolve macro and build details against the vendored 0.13.1 sources.
