#pragma once
#include "base_widget.hpp"
#include "world/world.hpp"

#include "render/renderer_api.hpp"
#include "render/vertex.hpp"

class WorldPreview : public BaseWidget {
public:
    WorldPreview(const UIContext& context);
    ~WorldPreview();

    Signal<const Region&> onRegionHovered;
    Signal<const Region&> onRegionSelected;

    std::optional<const Region*> getSelectedRegion();

    void setWorld(const World* w);

    virtual void draw(RenderTarget& target, const UIContext& context) override;
    virtual bool handleInput(const InputEvent& e, const UIContext& context) override;

private:
    const World* world = nullptr;
    std::string preview_shader = "map_preview";
    std::string river_shader = "river_preview";

    // камера
    float offsetX = 0.0f;
    float offsetY = 0.0f;

    float zoom = 1.0f;

    // input
    bool dragging = false;
    int lastMouseX = 0;
    int lastMouseY = 0;

    // настройки
    float minZoom = 1.0f;
    float maxZoom = 100.0f;

    bool cacheDirty = true;

    void buildCache(RenderTarget& target);

    MeshHandle terrainMesh;
    MeshHandle riverMesh;

    std::vector<TerrainVertex> terrain_verts;
    std::vector<uint32_t> terrain_indices;

    std::vector<RiverVertex> river_verts;
    std::vector<uint32_t> river_indices;
    
    ShaderProperty u_time;
    ShaderProperty u_zoom;
    ShaderProperty u_min_temp_c;
    ShaderProperty u_max_temp_c;
    ShaderProperty u_world_width;

    const Region* hoveredRegion = nullptr;
    const Region* selectedRegion = nullptr;

    bool debugHighlight = true;
    int hoveredX = -1;
    int hoveredY = -1;
    int selectedX = -1;
    int selectedY = -1;

    void wrapOffset();
    Vec2 screenToWorld(int mx, int my) const;
    Vec2 wp = {0,0};

    uint64_t lastRebuildTime = 0;

    Vec2 prevWindowSize;

};