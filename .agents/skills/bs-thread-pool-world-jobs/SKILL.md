---
name: bs-thread-pool-world-jobs
description: 'Implement and debug BS::thread_pool 5.1 world generation, chunk meshing, and sunlight jobs with safe publication, cancellation, and owner teardown.'
---

# BS Thread Pool World Jobs

## Project Map

Read `external/thread-pool-5.1.0/README.md`, especially task submission, exception handling, purging, and deadlocks; inspect `include/BS_thread_pool.hpp` for exact behavior. CMake exports `BS_thread_pool`. `src/engine/render/ui_context.hpp` owns `ThreadPool = BS::thread_pool<BS::tp::none>`.

`ChunkManager` submits generation futures and detached mesh jobs. `SunlightPropagator` submits detached column work. `WorldGenTask` uses a separate `std::thread`, so pool waiting does not cover it.

## Job Design

- Prefer jobs that read immutable inputs, compute privately, and publish owned results through a synchronized completion queue. Mutate game/UI state and perform GPU uploads at the consuming update boundary. A `shared_ptr<Chunk>` keeps a chunk alive but does not synchronize voxel access or keep a captured manager alive.
- Trace every captured `this`, pointer, and reference through cancellation and state destruction. Stop scheduling, request cooperative cancellation, wait for all jobs using an owner, then destroy the owner and dependencies. Waiting for the shared pool only when `UIContext` dies can be too late for an earlier-destroyed game state.
- Tag results with a world/chunk revision or generation token when a coordinate can be unloaded and reused. Drop stale completions and reset pending/in-flight state on failure or cancellation so streaming can recover.
- Use `submit_task` when completion or failure must be observed. Consume futures with `get()`; `wait()` alone does not propagate exceptions. Detached-task exceptions are swallowed by the default exception-enabled library unless caught inside the job; publish failures explicitly instead of leaving a chunk permanently queued.
- `purge()` discards queued work only; running jobs continue. Because this pool is shared, purging also affects unrelated systems. Prefer scoped cancellation tokens and task accounting when cancelling one world subsystem.
- Do not block a pool worker waiting for work queued to the same saturated pool. The current `tp::none` alias has no optional wait-deadlock checks. Such checks do not solve every nested-future starvation case. Poll ready futures on the main thread without stalling a frame.
- Bound outstanding chunk jobs and tune task size against streaming demand. Derive deterministic random seeds from world seed and coordinates, not worker execution order. Avoid holding chunk-map locks while waiting or doing long generation work.

## Validation & Sources

Build `app`; stress repeated world creation, rapid chunk unload/reload, cancellation, and exit during meshing/lighting. Exercise job failures when changing error handling. Use a race detector for shared-data changes where supported and measure queue depth/frame stalls for performance changes.

Primary source: [upstream 5.1.0 README](https://raw.githubusercontent.com/bshoshany/thread-pool/v5.1.0/README.md); the complete same-version manual is vendored locally.
