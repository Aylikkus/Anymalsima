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