# Shader Guidelines

See the [repository guidelines](../AGENTS.md) and [rendering guide](../src/render/AGENTS.md) for shared conventions and CPU-side integration.

## Structure & Naming

Each program has a directory containing matching stages and a local varying definition:

```text
shaders/my_effect/my_effect.vert
shaders/my_effect/my_effect.frag
shaders/my_effect/varying.def.sc
```

Use snake_case names and globally unique stage basenames: the compiler flattens output into `assets/shaders/`, so duplicate basenames overwrite each other. Start from `color/` for solid geometry, `sprite/` for textured UI, `block/` for material arrays, or the preview shaders for terrain data.

## Writing New Shaders

Use bgfx shader syntax: declare `$input` and vertex `$output` as needed, then include `<bgfx_shader.sh>`. Define attribute and varying types and semantics in `varying.def.sc`; match vertex outputs to fragment inputs. Follow existing four-space indentation and keep helper functions focused.

Match attributes to the actual vertex layout in `src/engine/render/vertex.hpp`, including component counts and normalized colors. Document packed channels: preview terrain data and block texture-array layers use different layouts. Use bgfx helpers such as `mul(u_modelViewProj, vec4(a_position, 1.0))`, `SAMPLER2D`, and `SAMPLER2DARRAY` as appropriate. World programs run in a camera-relative view (see the rendering guide): `a_position` may be mesh-local, so derive world space as `mul(u_model[0], vec4(a_position, 1.0)).xyz + u_world_eye.xyz`, as `block.vert` does, and bind `u_world_eye` for such programs.

Use `u_` names for uniforms. Match names, types, sampler slots, and uploaded values with CPU bindings. Connect new programs through the existing renderer or mesh-command shader key; bind required uniforms before submission. Review blend, depth, culling, and clipping state in the draw path.

## Compile & Register

Run from the repository root after dependencies are installed:

```bash
cmake --build build --target shaderc --parallel
python3 generate_assets.py
cmake --build build --target app --parallel
./build/src/app/app
```

Verify `SHADERC_PATH` in `generate_assets.py` matches the built executable. The script compiles both stages with the `spirv` profile and local varying definitions, producing `my_effect.vs.bin` and `my_effect.fs.bin`. It also regenerates asset definitions; confirm the shaders entry is `"my_effect": "assets/shaders/my_effect"`. The loader appends both binary suffixes. CMake copies assets but does not compile shader sources automatically.

## Validation

The generation script does not check compiler return codes or print stderr. Confirm both binaries were freshly generated; rerun failed shaderc invocations directly to inspect diagnostics. Launch the affected scene and check geometry, textures, transparency, zoom, and boundaries as relevant. Include screenshots for visual changes and review generated asset-definition changes before committing.
