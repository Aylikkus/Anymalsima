#pragma once

#include <bgfx/bgfx.h>

struct PosVertex {
    float x, y, z;

    static void init(bgfx::VertexLayout& layout) {
        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .end();
    }
};

struct PosUvColorVertex {
    float x, y, z;
    float u, v;
    uint32_t abgr;

    static void init(bgfx::VertexLayout& layout) {
        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .end();
    }
};

// Terrain vertex is for preview only
struct TerrainVertex {
    float x, y, z;  // position
    float u, v;     // world uv
    float biome;
    float moisture;
    float temperature;
    float height;

    static void init(bgfx::VertexLayout& layout) {
        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord1, 4,
                 bgfx::AttribType::Float)  // biome/moist/temp/height
            .end();
    }
};

struct RiverVertex {
    float x, y, z;
    float u, v;
    float placeholder = 255;  // unused
    float flow;
    float side;
    float height;

    static void init(bgfx::VertexLayout& layout) {
        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord1, 4,
                 bgfx::AttribType::Float)  // placeholder/flow/side/height
            .end();
    }
};

// Plant meshes (game/world/flora_mesh_builder.hpp), drawn instanced by the
// foliage program. Positions are plant-local (base at the origin, +Y up).
struct FoliageVertex {
    float x, y, z;
    float nx, ny, nz;
    // u, v, foliage texture layer (-1: untextured, opaque), and height:
    // 0 at the root to 1 at the top (colour gradient, wind, root shading).
    float u, v, layer, height;
    // translucency, hue jitter, value jitter, normal mode (0 = card: faces
    // the viewer from both sides; 1 = shaped: spherical or cylindrical).
    float translucency, hueJitter, valueJitter, normalMode;
    uint32_t color;  // base RGBA8 (palette, gradient, per-card variation)
    uint32_t dry;    // withered colour RGBA8

    static void init(bgfx::VertexLayout& layout) {
        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord1, 4, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::Color1, 4, bgfx::AttribType::Uint8, true)
            .end();
    }
};

struct BlockVertex {
    float x, y, z;
    float nx, ny, nz;  // face normal
    float tx, ty, tz;  // tangent
    float u, v;
    float layer;
    uint32_t tint;

    static void init(bgfx::VertexLayout& layout) {
        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Tangent, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord1, 1, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .end();
    }
};