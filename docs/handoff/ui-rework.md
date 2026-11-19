# UI rework

## Loading screen for entering a world

Add a dedicated loading screen when the UI rework begins. Show it when starting
a new world or continuing a save. Keep it up until the initial camera view is
ready, then reveal gameplay and enable player input.

- Wait for nearby chunks to finish generating or loading from disk, meshing,
  uploading, and lighting.
- Wait for visible terrain LODs and structure proxies to be drawable across
  the initial view. This includes large landmarks such as elders, whose
  branches can look chopped while their proxy meshes are still arriving.
- Show progress from streaming and rendering readiness. Once the initial view
  is ready, let work beyond that view continue during gameplay.

Verify by starting a new world and by reopening a save near an elder. In both
cases, the first playable view should show continuous terrain and complete
landmark silhouettes.

## Text layout

Port the commented SFML-era code in `Renderer::drawTextLayout`,
`Renderer::rebuild`, and `Renderer::rebuildIncrementally` to the current
renderer and refactor the three paths together. Verify that the drawing path
uses the layout produced by both full and incremental rebuilds.

## Image widgets

Add reusable support for images and sprites in UI widgets. It should cover item
icons in inventory slots, skill icons, and other UI sprites.
