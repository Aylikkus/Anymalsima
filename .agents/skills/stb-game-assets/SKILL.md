---
name: stb-game-assets
description: 'Use the vendored stb_image and stb_truetype for game texture decoding, glyph metrics, font fallback, and GPU atlas uploads in this repository.'
---

# stb Game Assets

## Project Map

`external/stb/` contains stb_image 2.30 and stb_truetype 1.26. This local subset has no README; read each header's introductory documentation and relevant declarations. The `stb` CMake target already compiles `stb_image.c` and `stb_truetype.c` with their implementation macros. Include declarations in game code; do not define a second implementation.

`src/render/resource_manager.*` loads RGBA textures, retains font bytes, rasterizes glyphs, and maintains atlases. Text layout lives in `renderer.cpp`, with UTF-8 and fallback helpers under `src/engine/utils/`. Shader sampling for glyph coverage lives in `shaders/text/`.

## Texture Decisions

- Pair `stbi_load` with `stbi_image_free`, including failures after decoding. Check null results before use. Requested output channels determine buffer stride: this project requests four channels, while the reported original channel count can differ.
- Validate dimensions, multiplication overflow, and bgfx texture-size limits before allocating or narrowing to 16-bit dimensions. Match RGBA8 uploads and material-array dimensions to actual decoded pixels.
- Keep image orientation consistent with UV generation; do not change the global vertical-flip setting as a per-request worker operation. If concurrency requires configuration, check the installed thread-local APIs and their availability.
- Separate decode-buffer ownership from GPU upload lifetime. Freeing stb memory is safe only after copying pixels or arranging a valid bgfx upload lifetime; `makeRef` does not copy.

## Font & Atlas Decisions

- `stbtt_fontinfo` references the original font data. Retain `FontResource::ttfData` without reallocating it while font info is in use. Use Unicode codepoints, not UTF-8 bytes, for glyph lookup; resolve missing glyphs through the existing font roles/fallback logic.
- Apply the same scale to advances, bearings, ascent/descent, bitmap placement, and caret measurement. Zero-area glyphs such as spaces still have advance widths. Pair allocated glyph bitmaps with `stbtt_FreeBitmap` and the matching allocator context.
- Preserve atlas padding and invalidate affected layout/render caches when fonts or size roles change. Avoid rerasterizing unchanged glyphs every frame and copying whole atlases unnecessarily in measured hot paths.

## Validation & Sources

Build `app`; inspect alpha textures, material arrays, glyph edges, multiline alignment, selection/caret positions, CJK fallback, and UI scale changes. The upstream headers may be newer than this checkout; verify API availability locally.

Primary sources: [stb_image header documentation](https://github.com/nothings/stb/blob/master/stb_image.h) and [stb_truetype header documentation](https://github.com/nothings/stb/blob/master/stb_truetype.h).
