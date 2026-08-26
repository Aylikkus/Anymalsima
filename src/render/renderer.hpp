#pragma once
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>

#include <queue>

#include "audio_system.hpp"
#include "platform.hpp"
#include "render/render_target.hpp"
#include "render/renderer_api.hpp"
#include "render/ui_context.hpp"
#include "render_cache.hpp"
#include "render/vertex.hpp"
#include "resource_manager.hpp"


struct Line {
    std::vector<int32_t> codepoints;
    size_t start;
    size_t end;
    FontRole font;
    int raw_line_index;
    float minX;
    float maxX;
};

class Renderer : public IRenderer {
   public:
    Renderer(Platform& platform);
    bool isOpen();

    void beginFrame();
    void draw(const RenderTarget& target);
    void endFrame();

    void scheduleDelete(const MeshVariant& mesh);

    const float* getViewMatrix();
    const float* getProjMatrix();

    void clear();

    ResourceManager& getResources();
    AudioSystem& getAudioSystem();
    void rebuild(TextLayoutState& state) const;
    void rebuildIncrementally(TextLayoutState& state) const;
    Vec2 caretPosition(TextLayoutState& state, size_t index,
                       bool raw = false) const;
    size_t caretAtPoint(const TextLayoutState& state, Vec2 point) const;
    void buildSelectionRects(TextLayoutState& state, size_t selStart,
                             size_t selEnd) const;

   private:
    Platform& platform;
    ResourceManager resources;
    AudioSystem audio_system;
    void clearTextCaches();
    std::vector<Line> wrapText(const std::vector<int32_t>& codepoints,
                               float maxWidth, FontRole font, uint32_t size);
    std::vector<Line> wrapText(const std::vector<int32_t>& codepoints,
                               float maxWidth, FontRole font,
                               uint32_t size) const;
    float measureTextWidth(const std::vector<int32_t>& codepoints,
                           FontRole font, uint32_t size) const;
    void drawText(const DrawTextCmd& cmd);
    void drawTextLayout(const DrawTextLayoutCmd& cmd);
    void drawRect(const DrawRectCmd& cmd);
    void drawSprite(const DrawSpriteCmd& cmd);
    void drawVB(const DrawVertexBufferCmd& cmd);
    void applyClip();
    std::unordered_map<WrapKey, std::vector<Line>> wrapCache;
    std::vector<Rect> clipStack;

    std::vector<MeshVariant> pendingDeletes;

    void createRectMesh();
    void createRectUVMesh();

    float view_g[16];
    float proj_g[16];

    bgfx::UniformHandle u_color;
    bgfx::UniformHandle u_tex;
    bgfx::UniformHandle u_uvRect;
    MeshHandle rect;
    MeshHandle uv_rect;
};