#pragma once

#include <BS_thread_pool.hpp>

#include "animation/animator.hpp"
#include "audio/audio_system_api.hpp"
#include "core/assets_api.hpp"
#include "core/geometry.hpp"
#include "core/platform_api.hpp"
#include "core/settings.hpp"
#include "render/render_commands.hpp"
#include "render/renderer_api.hpp"

using ThreadPool = BS::thread_pool<BS::tp::priority>;

constexpr float REFERENCE_WIDTH = 1920.0f;
constexpr float REFERENCE_HEIGHT = 1080.0f;

struct UIContext {
    float globalScale = 1.0f;
    Settings* playerSettings = nullptr;
    IRenderer* renderer = nullptr;
    IAudioSystem* audioSystem = nullptr;
    IPlatform* platform = nullptr;
    IAssets* assets = nullptr;
    ThreadPool threadPool = ThreadPool();

    Signal<Vec2> onResize;

    void setWindowSize(Vec2 size) {
        if (windowSize == size) return;
        windowSize = size;
        onResize.emit(windowSize);
    }

    Vec2 getWindowSize() const { return windowSize; }

    UIContext(Vec2 windowSize, IRenderer* renderer, IAudioSystem* audioSystem,
              IPlatform* platform, IAssets* assets)
        : renderer(renderer),
          audioSystem(audioSystem),
          platform(platform),
          assets(assets) {
        this->windowSize = windowSize;
    }

    bool operator==(const UIContext& other) const {
        return (globalScale == other.globalScale &&
                windowSize == other.windowSize);
    }

    bool operator!=(const UIContext& other) const { return !(*this == other); }

    uint32_t resolveFontSize(FontSizeRole role) const {
        float uiScale = scale();

        switch (role) {
            case FontSizeRole::Smallest:
                return (uint32_t)(8 * uiScale);
            case FontSizeRole::Small:
                return (uint32_t)(12 * uiScale);
            case FontSizeRole::Medium:
                return (uint32_t)(16 * uiScale);
            case FontSizeRole::Large:
                return (uint32_t)(24 * uiScale);
            case FontSizeRole::Title:
                return (uint32_t)(36 * uiScale);
        }
        return 16;
    }

    float scale() const {
        float scaleX = windowSize.x / REFERENCE_WIDTH;
        float scaleY = windowSize.y / REFERENCE_HEIGHT;
        return std::min(scaleX, scaleY) * globalScale;
    }

   private:
    Vec2 windowSize;
};
