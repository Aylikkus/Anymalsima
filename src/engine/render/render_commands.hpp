#pragma once
#include "core/geometry.hpp"
#include "text_layout_state.hpp"
#include "core/color.hpp"
#include <string>
#include <cstdint>
#include <SFML/Graphics.hpp>
#include <variant>
#include <unordered_map>
#include "renderer_api.hpp"
#include "vertex.hpp"
#include <variant>

using FontId = uint32_t;

enum class DrawLayer {
    UI,
    GAME,
    // World-space additive glow (light glare): drawn after GAME in its own
    // view, depth-tested against the world but never writing depth.
    GAME_GLOW
};

struct DrawRectCmd {
    Rect rect;
    Color color;
};

struct DrawTextCmd {
    std::string text;
    Rect rect;
    FontRole font;
    uint32_t size;
    TextAlign alignH;
    VerticalAlign alignV;
    Color color;
};


struct DrawSpriteCmd {
    Rect rect;
    Rect uv;
    const std::string* texture;
    Color tint;
};

struct DrawTextLayoutCmd {
    TextLayoutState layout;
    Color textColor;
};

struct ClipRectCmd {
    Rect rect;
};

enum class BufferType {
    Triangles,
    Lines,
    TriangleStrip
};

struct DrawVertexBufferCmd {
    MeshVariant mesh;
    const std::string* shader;
    std::vector<ShaderProperty*> uniforms;
    bool isStaticMesh;
    std::array<float, 16> transform;
    DrawLayer layer = DrawLayer::UI;
};

struct SetCameraCmd {
    Camera* camera;
};

enum class CommandType {
    Rect,
    Sprite,
    Text,
    TextLayout,
    DrawVertexBuffer,
    PushClip,
    PopClip
};

using CommandVariant = std::variant<
    DrawRectCmd,
    DrawSpriteCmd,
    DrawTextCmd,
    DrawTextLayoutCmd,
    DrawVertexBufferCmd,
    ClipRectCmd  // PushClip/PopClip
>;

struct Command {
    CommandType type;
    CommandVariant data;
};