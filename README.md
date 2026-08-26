# Voxel World

A C++ real-time 3D voxel world project built with SDL3, bgfx and Vulkan.

The project combines a custom rendering layer, UI system, procedural world generation and asynchronous voxel chunk processing. It is primarily a personal C++ project focused on real-time rendering, multithreading, procedural generation and engine architecture.

![Main menu](screenshots/screen1.png)
![Main menu (Russian)](screenshots/screen2.png)
![Generated 2D map](screenshots/screen3.png)
![Generated world](screenshots/screen4.png)

## Features

- Real-time 3D rendering using bgfx with Vulkan
- Procedural 2D world generation
  - Height maps
  - Biomes
  - Climate simulation
  - Air-flow simulation
- 3D voxel world generation
  - 32×32×32 voxel chunks
  - Dynamic chunk loading around the camera
  - Asynchronous chunk generation and meshing
  - Sunlight propagation
  - Voxel raycasting
  - Frustum culling
- Custom UI system
  - Hierarchical widget tree
  - Buttons, text, sliders, text boxes, scroll containers and dropdowns
  - Grid-based layout
  - Anchoring
  - Signals/events
  - UI animations and tweens
- Internationalization
  - English
  - Russian
  - Japanese
  - CJK font support
  - Runtime language switching
- Audio system
  - Master volume
  - Music volume
  - Sound effects volume
- Settings serialization
- Data-driven blocks, materials and world definitions
- Resource and asset management
- Multithreaded task processing using a thread pool
- Tracy profiling integration
- CMake-based build system

## Technology Stack

- **C++**
- **CMake**
- **SDL3** — windowing, input and platform integration
- **bgfx** — rendering abstraction
- **Vulkan** — graphics backend
- **EnTT** — entity/component utilities
- **fmt** — formatting
- **nlohmann/json** — JSON serialization and configuration
- **miniaudio** — audio playback
- **stb** — image and font processing
- **BS::thread_pool** — multithreaded task processing
- **Tracy** — profiling

## Project Structure

```text
src/
├── app/          Application entry point
├── audio/        Audio system
├── engine/       Core engine systems
│   ├── animation/
│   ├── audio/
│   ├── core/
│   ├── generation/
│   ├── render/
│   ├── serialization/
│   └── utils/
├── game/         Game-specific systems
│   ├── components/
│   ├── items/
│   ├── states/
│   └── world/
├── i18n/         Internationalization
├── platform/     Platform and window management
├── render/       Rendering implementation
└── ui/           Custom UI system
```

The game/world module contains the main voxel world implementation, including chunks, world generation, chunk meshing, streaming, raycasting and lighting.

Assets and game data are stored separately from the source code:

```text
assets/
├── audio/
├── data/
├── fonts/
├── i18n/
├── materials/
├── shaders/
└── textures/
```

# Dependencies

Third-party dependencies are not included in this repository.

Place the following dependencies inside the external/ directory:

```text
external/
├── bgfx.cmake-1.143-9241-532
├── entt-3.16.0
├── fmt-12.1.0
├── json-3.12.0
├── miniaudio-0.11.25
├── SDL3-3.4.8
├── stb
├── thread-pool-5.1.0
└── tracy-eb3df3b-0.13.1
```

The exact versions used by the project are listed above.

These dependencies are intentionally not committed to the repository due to repository size and third-party licensing/distribution considerations. I will setup git submodules later. Please obtain each dependency from its respective upstream repository and place it under external/ using the directory names expected by the project.

# Building

The project uses CMake.

The project is currently developed and built using Visual Studio Code with the CMake Tools extension.

After placing all required dependencies into external/, open the project in VS Code and configure it using the CMake Tools extension. Select the desired CMake build configuration and build the project from the CMake Tools interface (build was tested on recent versions of GCC and Clang).

# Controls

The current gameplay camera uses:

* `W` / `A` / `S` / `D` — camera movement
* `Shift` — move faster
* `Right mouse button` — rotate camera
* `Mouse wheel` — zoom
* `Ctrl` + `mouse wheel` — adjust camera height

# Development Status

This is an ongoing personal project. The current implementation focuses on the underlying engine and world-generation systems rather than on a complete game experience.

Major areas currently implemented include rendering, procedural world generation, voxel chunk management, asynchronous processing, UI, audio, localization and basic gameplay state management.
