# Streaming and LOD optimization — 2026-11-16

Same machine (16 threads), Release, fixed seed landscape; baseline built from
commit 2f904c9 in a separate worktree.

| Measurement | Baseline | Optimized |
|-------------|---------:|----------:|
| `lighting_benchmark` startup: full lighting of 4,913 chunks | 963 frames, 4,085 ms | 44 frames, 118 ms |
| `lod_benchmark`: build 244 patches, one thread | 5.92 s | 1.88 s (32 texel map), 4.24 s (64) |
| `handover_walk` initial load complete | frame 785 | frame 354 |
| `handover_walk` region crossing | dropped to 20 coarse patches, refined by frame 1575 | stays at 236 patches, done at frame 434 |
| `handover_walk` walk back | 192 patches still pending at frame 1999 | done at frame 450 |
| `travel` start / 512-tile move / long teleport | frames 1038 / 1977 / not done by 2399 | frames 464 / 673 / 931 |

All scenarios kept `detail_regressions=0`. The `max_cpu_frame_ms` counter of
`lod_render_smoke` includes PPM capture frames (GPU readback and a 2.7 MB file
write); its maxima are those frames in both builds. The High preset stayed
within its 256 MiB budget in `travel` and `handover_walk`.

# Region-boundary visibility regression — 2026-11-15

Fixed two causes of detailed terrain briefly falling back to LOD while walking:
coverage was rebuilt from the current distant LOD layout (which could temporarily
be coarse), and a queued lighting remesh invalidated visibility of its still
uploaded predecessor. Detailed-column ownership is now independent of both.
Real unloading and leaving the hysteresis keep radius still retire coverage.
Coarse LODs are masked beneath those columns and get explicit boundary joins.

All 103 gameplay cases pass in Release and the ASan/UBSan logic configuration,
including four new regressions for coverage retention, hysteresis, triangulated
surface sampling and coarse-to-detail joins.

The opt-in `handover_walk` Vulkan regression crossed one 128-tile region boundary
and returned, forcing chunk remeshes at both moves. It performed 23,796 per-frame
checks of previously visible, still drawable columns, with zero visibility
regressions. It drew 42, 40 and 52 detailed chunk meshes at the three stops.
Peak logical LOD residency, including the reserved join buffers, was 63.936 MiB;
peak per-frame geometry uploads were 809.398 KiB. This run used the same fixed
landscape and llvmpipe setup as the initial verification below.

Reproduce with:

```bash
cmake --build build --target app world_logic_tests lod_render_smoke --parallel 4
ctest --test-dir build -L gameplay --output-on-failure --no-tests=error
build/tests/lod_render_smoke build/lod_boundary 2000 handover_walk
```

---

# Distant terrain verification — 2026-11-15

The application and shader stages build. All 99 gameplay cases pass in the
Release/Clang build and in the GCC ASan + UBSan logic build, including nine LOD
cases. LeakSanitizer execution requires running outside the restricted sandbox.
No address, leak or undefined-behavior errors were reported by the logic suite.

The optional headless Vulkan tool successfully rendered the fixed synthetic
landscape using runtime material assets on llvmpipe (LLVM 22.1.8, Mesa 26.2.4),
at 1280 by 720. The loaded-terrain scenario handed over to 42 detailed chunk
draws. Coast and elevated views completed and were captured. A travel sequence
completed its initial view, a 512-tile move and a further 4,096/2,048-tile X/Z
teleport, with no remaining LOD jobs or patches at each stop. Immediate teardown
with two outstanding LOD jobs completed successfully.

| Travel stop | Patches | Visible LOD draws | Resident MiB |
| --- | ---: | ---: | ---: |
| Initial | 248 | 96 | 48.604 |
| Nearby move | 194 | 84 | 42.821 |
| Teleport | 186 | 70 | 32.012 |

Across that run, peak logical LOD residency was 55.355 MiB and the largest
per-frame patch upload was 809.398 KiB, within the 64 MiB / 2 MiB limits.
Separate elevated and loaded-handover runs also stayed within those limits.
The CPU-only benchmark generated 244 patches, about 688,000 indexed vertices
and 866,000 triangles, totaling 46.839 MiB; its largest patch was 404.008 KiB.
Full single-thread CPU generation took about 2–3 seconds across the observed
runs. The Vulkan run refined three destinations in about 14 seconds total,
including capture/readback and paced frames.

Captured views were inspected for terrain/water continuity and the transition
from voxel steps into coarse terrain. PPM/PNG captures are under `build/` and
are intentionally not committed. Commands and architecture are documented in
[LOD.md](LOD.md).

These are software Vulkan measurements, with startup pipeline compilation,
readback, concurrent builds and no physical GPU available. Full-frame maxima
included 100–280 ms stalls, so these results establish bounded resources and
functional rendering, not a low-end hardware FPS target. Interactive SDL/menu
checks and hardware frame-time comparisons with LODs enabled/disabled remain
manual validation work; the capture tool does not instantiate the application
renderer or a window.

---

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
