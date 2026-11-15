#pragma once
#include "render/renderer_api.hpp"

// GPU-backed mesh uploads without a window, audio device or text system.
// Used only by the optional Vulkan capture executable.
class LodCaptureRenderer final : public IRenderer {
   public:
    ~LodCaptureRenderer() override { flush(); }
    std::vector<MeshVariant> deletes;
    void scheduleDelete(const MeshVariant& mesh) override {
        deletes.push_back(mesh);
    }
    void flush() {
        for (const auto& mesh : deletes)
            std::visit(
                [](const auto& m) {
                    if (bgfx::isValid(m.vbh)) bgfx::destroy(m.vbh);
                    if (bgfx::isValid(m.ibh)) bgfx::destroy(m.ibh);
                },
                mesh);
        deletes.clear();
    }
    void rebuild(TextLayoutState&) const override {}
    void rebuildIncrementally(TextLayoutState&) const override {}
    Vec2 caretPosition(TextLayoutState&, size_t, bool) const override {
        return {};
    }
    size_t caretAtPoint(const TextLayoutState&, Vec2) const override {
        return 0;
    }
    void buildSelectionRects(TextLayoutState&, size_t, size_t) const override {}
    const float* getViewMatrix() override { return nullptr; }
    const float* getProjMatrix() override { return nullptr; }
};
