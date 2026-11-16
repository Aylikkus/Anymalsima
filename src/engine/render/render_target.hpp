#pragma once

#include <vector>
#include <string>
#include <span>
#include "core/color.hpp"
#include "core/geometry.hpp"
#include "render_commands.hpp"

class RenderTarget
{
public:
    std::vector<Command> commands;

    void drawRect(const Rect &r, const Color &c);

    void drawText(
        const std::string &text,
        const Rect &rect,
        FontRole role,
        uint32_t size,
        TextAlign alignH,
        VerticalAlign alignV,
        const Color &color);

    void drawSprite(const Rect &r, const Rect &uv, const std::string* texture, const Color &tint = {255, 255, 255, 255});

    void drawTextLayout(const TextLayoutState& layout, const Color& textColor);

    void drawVertexBuffer(MeshVariant mesh, const std::string* shader, std::vector<ShaderProperty*>&& uniforms, bool isStatic, std::array<float, 16>&& transform, DrawLayer layer = DrawLayer::UI);

    // A shadow caster draw into the given bgfx view (DrawLayer::SHADOW).
    void drawShadowCaster(MeshVariant mesh, const std::string* shader, std::vector<ShaderProperty*>&& uniforms, bool isStatic, std::array<float, 16>&& transform, uint16_t view);

    // An instanced draw; `instances` must outlive the frame's submission.
    void drawInstanced(MeshVariant mesh, const std::string* shader, std::vector<ShaderProperty*>&& uniforms, bool isStatic, std::array<float, 16>&& transform, DrawLayer layer, const bgfx::InstanceDataBuffer* instances);

    void pushClip(const Rect &r);

    void popClip();

    void clear();
};