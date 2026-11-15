---
name: entt-game-ecs
description: 'Develop entity and component gameplay with EnTT 3.16 in this repository, including entity lifetimes, views, storage invalidation, and worker synchronization.'
---

# EnTT Gameplay ECS

## Project Map

Read `external/entt-3.16.0/README.md` and `docs/md/entity.md`; use `src/entt/entity/registry.hpp`, `storage.hpp`, and `view.hpp` to resolve API details. The `game` target links `EnTT::EnTT`. Existing component definitions are in `src/game/components/`; `Ownership` stores an `entt::entity`. Current usage is limited: establish who owns a registry before extending gameplay, rather than assuming a complete ECS scheduler exists.

## Gameplay Design

- Put small gameplay data in components and behavior in systems owned by the relevant game state/world. Keep voxel terrain in the existing chunk storage unless the task explicitly requires another representation; one entity per voxel would change memory and iteration costs substantially.
- Treat entity IDs as registry-local, versioned handles. Initialize absent relationships with `entt::null`; check `registry.valid()` and component presence before following ownership or inventory links. Validity does not imply the required component exists. Use stable application IDs and remapping for persistent relationships instead of serializing transient handles as universal IDs.
- Use views for ordinary component iteration. Consider owning groups only after profiling a repeated multi-component query; they rearrange owned storage and impose ownership restrictions. Keep component access const when a system only reads it.
- Do not keep raw component pointers across deletion, sorting, or group reordering. Default paged storage preserves references on growth, but compacting deletion can move another component. Stable deletion is an explicit tradeoff and is incompatible with owning groups.
- Follow the installed iteration rules: deleting the current entity can be supported, but removing another entity's iterated components can invalidate iteration. Collect deferred structural changes when the mutation pattern is unclear; apply them after traversal.
- A registry is not generally thread-safe. Pre-create needed storages before parallel work; partition writes and prohibit concurrent structural changes to iterated pools. Publish worker results at a controlled update boundary. `ENTT_USE_ATOMIC` does not make shared registry mutation safe.

## Validation & Sources

Build `app`. Exercise creation/destruction, stale relationships, state teardown, and repeated entity churn. For parallel systems, inspect the read/write sets and stress the same gameplay sequence under a race detector where supported.

Primary sources: [EnTT entity documentation](https://github.com/skypjack/entt/blob/main/docs/md/entity.md) and [configuration](https://github.com/skypjack/entt/wiki/Configuration). Prefer the vendored 3.16 documentation when online examples differ.
