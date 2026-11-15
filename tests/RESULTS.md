# Gameplay tests and camera-priority lighting results

Verified on 2026-11-15. All original sunlight regressions remain fixed. The suite
now contains 72 cases, including 14 new scheduling/streaming regressions.

| Configuration | Result | Execution time |
| --- | --- | --- |
| Debug, Clang 22 | 72 passed, 0 failed | about 0.62 s |
| Release, GCC 16 | 72 passed, 0 failed | about 0.28 s |
| RelWithDebInfo, GCC 16, ASan + UBSan | 72 passed, 0 failed | about 2.05 s |

The sanitizer run reported no address, leak, or undefined-behavior errors. UBSan
terminates on its first error. Sanitizer execution ran outside the restricted
sandbox so LeakSanitizer could inspect processes. The application target builds.

## Camera-priority benchmark

Release/GCC 16, eight workers, 4,913 preloaded chunks (radius eight), with identical
explicit-air contents. The baseline uses pre-priority source snapshots saved
before edits under `build/lighting-baseline/`; it runs the same benchmark source
with `LIGHTING_BASELINE`. Runs were performed sequentially after builds/tests.

| Scenario / implementation | First direct light | Complete target light | Full backlog |
| --- | --- | --- | --- |
| Startup, baseline | 2,040 updates / 4,719 ms | 2,040 updates / 4,719 ms | 2,603 updates / 6,059 ms |
| Startup, camera priority | 1 update / 4.4 ms | 3 updates / 13.3 ms | 1,039 updates / 4,387 ms |
| Teleport, baseline | 742 updates / 1,725 ms | 742 updates / 1,725 ms | 2,603 updates / 7,124 ms |
| Teleport, camera priority | 1 update / 5.3 ms | 2 updates / 10.5 ms | 860 updates / 3,730 ms |

Startup measures the central chunk. Teleport changes focus to a corner at update
eight; target latencies are measured after the jump, while full-backlog figures
include the entire run. Baseline direct light appears only with final indirect
publication, so those two baseline milestones coincide.

These are headless scheduler measurements, not game-loading or GPU frame times.
Each update has a worker completion barrier, with no imposed frame cadence.
Target counts are update intervals from the initial dispatch or camera jump;
the first dispatch uses index zero.
Generation/preload, rendering, and meshing are excluded. The 2 ms dispatch budget
and runtime load affect full-backlog update counts and timings; no numeric timing
threshold is asserted in CTest. In particular, these millisecond values do not
promise an equivalent end-to-end visual startup time.

Reproduce the current benchmark with:

```bash
cmake --build build/tests-release --target lighting_benchmark --parallel 4
./build/tests-release/tests/lighting_benchmark
./build/tests-release/tests/lighting_benchmark teleport
```

The saved baseline executable is `build/lighting-baseline/lighting_benchmark` and
accepts the same scenario argument. Build-directory snapshots are local artifacts,
not an additional production lighting implementation.

## Scheduling behavior verified

- Direct sunlight publishes progressively for nearby chunks. Indirect jobs wait
  for current local halo sources rather than all distant column sweeps.
- Pending work is reranked from the current camera position, including vertical
  moves and teleports. Camera movement preserves valid existing lighting.
- Column-ready notifications preserve valid sources. Changed direct buffers
  invalidate dependent halos; the column index avoids scanning every ready chunk
  to snapshot a single column.
- Sky generation inherits beneficiary priority. If the camera moves into that
  sky slab, its own proximity can take precedence over the earlier beneficiary.
- Generation and mesh queues stay outside the worker pool until selected.
  Submission caps are four generation jobs, two mesh jobs, and 1–8 lighting jobs
  based on worker count. Lighting dispatch checks a 2 ms preparation budget
  between jobs, allowing one job to ensure progress.
- Lighting, prerequisite generation, and newly lit remeshes use the shared pool's
  high-priority tier. Every sixteenth dispatch advances the oldest pending work.
- Generation and mesh workers own snapshots. Identity/revision checks reject
  obsolete completions after replacement or edits. Unloading does not wait for
  submitted generation work, and existing uploaded geometry stays visible while
  remeshing.

Promise-gated worker tests and single-worker fixtures verify priority/caps without
sleep-based races. Tests cover near-first publication, local dependencies,
continuous arrivals, stable request age, background progress, focus changes,
replacement, urgent pool work, and remesh selection.

## Existing lighting correctness

The previous roof-shadow, lateral propagation, generated-ocean, and opacity-JSON
failures still pass. Exact-value tests cover both directions of X/Y/Z boundaries,
a bend crossing all three axes, opacity/water attenuation, competing sources,
maximum range, opening/closing passages, late neighbors, missing upper slabs,
source unload/reload, stale results, raised bounds, and teardown.

Direct light and final exposure remain separate. Indirect jobs use a 14-tile halo
and six-direction opacity/distance attenuation; camera priority changes scheduling
and publication, not the settled illumination rules.

## Commands and limits

See [README.md](README.md) for configuration and sanitizer commands. Each build's
latest full CTest output is in `Testing/Temporary/LastTest.log`.

```bash
ctest --test-dir build -L gameplay --output-on-failure --no-tests=error
ctest --test-dir build -L priority --output-on-failure --no-tests=error
```

GPU appearance and visual startup timing, full planetary-generation correctness,
every data catalog/seed, and ThreadSanitizer coverage were not validated. Existing
warnings in layer/noise generation and data storage remain outside these changes.
