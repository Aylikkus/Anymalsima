#include "render_target.hpp"
#include <iostream>

void RenderTarget::drawRect(const Rect &r, const Color &c)
{
    Command cmd;
    cmd.type = CommandType::Rect;
    cmd.data = DrawRectCmd(r, c);
    commands.push_back(std::move(cmd));
}

void RenderTarget::drawText(
    const std::string& text,
    const Rect& rect,
    FontRole role,
    uint32_t size,
    TextAlign alignH,
    VerticalAlign alignV,
    const Color& color)
{
    Command cmd;
    cmd.type = CommandType::Text;
    cmd.data = DrawTextCmd(
        text,
        rect,
        role,
        size,
        alignH,
        alignV,
        color
    );
    commands.push_back(std::move(cmd));
}

void RenderTarget::clear()
{
    commands.clear();
}

void RenderTarget::drawSprite(const Rect &r, const Rect &uv, const std::string* texture, const Color &tint)
{
    Command cmd;
    cmd.type = CommandType::Sprite;
    cmd.data = DrawSpriteCmd(r, uv, texture, tint);
    commands.push_back(std::move(cmd));
}

void RenderTarget::pushClip(const Rect& r) {
    Command cmd;
    cmd.type = CommandType::PushClip;
    cmd.data = ClipRectCmd(r);
    commands.push_back(std::move(cmd));
}

void RenderTarget::popClip() {
    Command cmd;
    cmd.type = CommandType::PopClip;
    commands.push_back(std::move(cmd));
}

void RenderTarget::drawTextLayout(const TextLayoutState &layout, const Color &textColor)
{
    Command cmd;
    cmd.type = CommandType::TextLayout;
    cmd.data = DrawTextLayoutCmd(layout, textColor);
    commands.push_back(std::move(cmd));
}


void RenderTarget::drawVertexBuffer(MeshVariant mesh, const std::string* shader, std::vector<ShaderProperty*>&& uniforms, bool isStatic, std::array<float, 16>&& transform, DrawLayer layer)
{
    Command cmd;
    cmd.type = CommandType::DrawVertexBuffer;
    cmd.data = DrawVertexBufferCmd(mesh, shader, std::move(uniforms), isStatic, std::move(transform), layer);
    commands.push_back(std::move(cmd));
}

void RenderTarget::drawShadowCaster(MeshVariant mesh, const std::string* shader, std::vector<ShaderProperty*>&& uniforms, bool isStatic, std::array<float, 16>&& transform, uint16_t view)
{
    Command cmd;
    cmd.type = CommandType::DrawVertexBuffer;
    DrawVertexBufferCmd draw(mesh, shader, std::move(uniforms), isStatic, std::move(transform), DrawLayer::SHADOW);
    draw.view = view;
    cmd.data = std::move(draw);
    commands.push_back(std::move(cmd));
}

void RenderTarget::drawInstanced(MeshVariant mesh, const std::string* shader, std::vector<ShaderProperty*>&& uniforms, bool isStatic, std::array<float, 16>&& transform, DrawLayer layer, const bgfx::InstanceDataBuffer* instances)
{
    Command cmd;
    cmd.type = CommandType::DrawVertexBuffer;
    DrawVertexBufferCmd draw(mesh, shader, std::move(uniforms), isStatic, std::move(transform), layer);
    draw.instances = instances;
    cmd.data = std::move(draw);
    commands.push_back(std::move(cmd));
}