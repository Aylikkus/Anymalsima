#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "light_shadow_math.hpp"
#include "light_sources.hpp"
#include "render/frustrum.hpp"
#include "render/render_target.hpp"
#include "render/renderer_api.hpp"
#include "tile_box.hpp"

// Cube shadow maps of the most important far lights (LightEmission::isFar),
// so walls, boxes and mountains stop their light. Main thread only.
//
// Every shadowed light owns a slot of six TileSize^2 tiles in one R32F atlas
// holding distance / radius to the nearest caster (light_shadow_math.hpp).
// Shadows are cached: a face is rendered again only when a light is
// assigned, moves or turns, when a chunk mesh inside its radius changes, or
// (throttled) when distant terrain selection changes. A per-frame face
// budget spreads the cost. Only lights with ready slots illuminate surfaces
// beyond the voxel near field.
class LightShadows {
   public:
    struct Budget {
        int lights;         // shadowed lights at once
        int facesPerFrame;  // cube faces rendered per frame
    };
    // By TerrainLod quality (Low, Medium, High).
    // facesPerFrame must stay within RenderViews::MaxShadowViews.
    static constexpr Budget Budgets[] = {{4, 3}, {8, 6}, {16, 12}};
    static constexpr int TileSize = 512;
    static constexpr float NearPlane = 0.05f;
    // A light keeps its slot while it ranks within budget * (1 + Keep).
    static constexpr double Keep = 0.5;
    // Distant terrain changes re-render a light at most this often.
    static constexpr double LodRefreshSeconds = 1.0;

    // One cube face to fill: draw every caster that may intersect `frustum`
    // (coordinates relative to `light`) into `view` with drawShadowCaster.
    struct Face {
        std::array<double, 3> light;  // world, copy nearest the camera
        float radius = 0;
        uint16_t view = 0;
        Frustum frustum;
        const std::string* shader = nullptr;
        // Uniform sets for chunk-like meshes and for distant terrain (which
        // also needs the detail coverage map bound by its owner).
        std::vector<ShaderProperty*> chunkUniforms;
        std::vector<ShaderProperty*> lodUniforms;
    };
    using Casters = std::function<void(RenderTarget&, const Face&)>;

    explicit LightShadows(int quality);
    ~LightShadows();
    LightShadows(const LightShadows&) = delete;
    LightShadows& operator=(const LightShadows&) = delete;

    // ranked: visible far lights, most important first. Keeps, frees and
    // assigns slots; detects moved or changed lights.
    void assign(const std::vector<FarLightView>& ranked);
    // Atlas slot of a light whose six faces are all rendered, or -1.
    int slot(LightSourceRegistry::Id id) const;
    // Exact structure casters near the shadowed lights, aligned to chunks.
    std::vector<TileBox> casterDetail() const;
    // Geometry changed inside the box [min, max] (world, canonical X):
    // re-render the faces of lights that can see it.
    void geometryChanged(const std::array<double, 3>& min,
                         const std::array<double, 3>& max, int periodX);
    // Distant terrain selection changed (throttled refresh).
    void lodChanged();
    // Renders dirty faces within the budget. now: seconds, monotonic.
    void render(RenderTarget& target, const Casters& casters, double now);
    // Views used by render() this frame and later; pass to
    // RenderViews::applyOrder.
    uint16_t viewCount() const { return uint16_t(budget.facesPerFrame); }

    // Sampler of the atlas bound at a program-specific stage.
    ShaderProperty* atlas(uint8_t stage);
    ShaderProperty* info() { return &infoUniform; }
    bgfx::TextureHandle atlasTextureHandle() const { return atlasTexture; }
    int atlasRows() const { return rows; }
    int shadowedCount() const;
    size_t facesRendered() const { return renderedFaces; }

   private:
    struct Slot {
        bool used = false;
        bool ready = false;  // every reachable face rendered at least once
        LightSourceRegistry::Id id = 0;
        std::array<double, 3> canonical{0, 0, 0};
        std::array<double, 3> image{0, 0, 0};
        std::array<float, 3> direction{0, -1, 0};
        float radius = 0;
        uint8_t faces = 0x3f;  // reachable faces
        uint8_t dirty = 0;
        bool lodStale = false;
        double lastLod = -1e9;
        int rank = 0;
        ShaderProperty casterChunk;  // u_shadow_caster
        ShaderProperty casterLod;
        ShaderProperty light;  // u_shadow_light
    };

    Budget budget;
    int rows = 0;
    bgfx::TextureHandle atlasTexture = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle depthTexture = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle frameBuffer = BGFX_INVALID_HANDLE;
    std::vector<std::unique_ptr<Slot>> slots;
    std::vector<std::pair<uint8_t, std::unique_ptr<ShaderProperty>>> samplers;
    ShaderProperty infoUniform;
    std::string casterShader = "light_shadow";
    size_t renderedFaces = 0;
};
