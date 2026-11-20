#include "world_preview.hpp"

#include <bx/math.h>

#include <bitset>
#include <iostream>

void WorldPreview::setWorld(const World* w) {
    world = w;
    hoveredRegion = nullptr;
    selectedRegion = nullptr;
    dragging = false;
    offsetX = 0;
    offsetY = 0;
    zoom = 1.0f;
    wrapOffset();
    cacheDirty = true;
    if (!world) return;

    u_min_temp_c.setFloat(world->min_temp_c);
    u_max_temp_c.setFloat(world->max_temp_c);
    u_world_width.setFloat(world->width);
}

void WorldPreview::draw(RenderTarget& target, const UIContext& context) {
    if (!visible || !enabled || !world) return;

    target.drawRect(computedRect, background);
    target.pushClip(computedRect);

    uint64_t ms = context.platform->getTimeMs();

    int timeSinceBuild = ms - lastRebuildTime;

    if (prevWindowSize != context.getWindowSize() && timeSinceBuild > 32) {
        cacheDirty = true;
        wrapOffset();
    }

    prevWindowSize = context.getWindowSize();

    if (cacheDirty && timeSinceBuild > 7) {
        buildCache(target);
        cacheDirty = false;
        lastRebuildTime = ms;
    }

    if (world) {
        float sec = ms / 1000.0f;

        u_time.setFloat(sec);
        u_zoom.setFloat(zoom);

        std::vector<ShaderProperty*> u1{&u_time, &u_zoom, &u_min_temp_c,
                                        &u_max_temp_c, &u_world_width};

        std::array<float, 16> transform;
        float tx = computedRect.x - offsetX + 0.5f * zoom;
        float ty = computedRect.y - offsetY + 0.5f * zoom;

        bx::mtxIdentity(transform.data());
        transform[0] = zoom;  // scale X
        transform[5] = zoom;  // scale Y
        transform[12] = tx;   // translate X
        transform[13] = ty;   // translate Y

        target.drawVertexBuffer(terrainMesh.get(), &preview_shader,
                                std::move(u1), false, std::move(transform));

        std::vector<ShaderProperty*> u2{&u_time, &u_zoom};

        std::array<float, 16> transform_r;

        bx::mtxIdentity(transform_r.data());
        transform_r[0] = zoom;  // scale X
        transform_r[5] = zoom;  // scale Y
        transform_r[12] = tx;   // translate X
        transform_r[13] = ty;   // translate Y
        target.drawVertexBuffer(riverMesh.get(), &river_shader, std::move(u2),
                                false, std::move(transform_r));
    }

    if (debugHighlight && hoveredRegion) {
        float worldW = float(world->width);

        // determine visible repeats
        float viewL = (offsetX - computedRect.x) / zoom;
        float viewR = viewL + computedRect.width / zoom;

        int repeatMin = int(std::floor(viewL / worldW)) - 1;
        int repeatMax = int(std::floor(viewR / worldW)) + 1;

        for (int k = repeatMin; k <= repeatMax; ++k) {
            float offsetXWorld = k * worldW;

            float sx =
                computedRect.x + (hoveredX + offsetXWorld) * zoom - offsetX;
            float sy = computedRect.y + hoveredY * zoom - offsetY;

            Rect r;
            r.x = sx;
            r.y = sy;
            r.width = zoom;
            r.height = zoom;

            target.drawRect(r, {255, 0, 0, 80});
        }
    }

    if (debugHighlight && selectedRegion) {
        float worldW = float(world->width);

        // determine visible repeats
        float viewL = (offsetX - computedRect.x) / zoom;
        float viewR = viewL + computedRect.width / zoom;

        int repeatMin = int(std::floor(viewL / worldW)) - 1;
        int repeatMax = int(std::floor(viewR / worldW)) + 1;

        for (int k = repeatMin; k <= repeatMax; ++k) {
            float offsetXWorld = k * worldW;

            float sx =
                computedRect.x + (selectedX + offsetXWorld) * zoom - offsetX;
            float sy = computedRect.y + selectedY * zoom - offsetY;

            Rect r;
            r.x = sx;
            r.y = sy;
            r.width = zoom;
            r.height = zoom;

            target.drawRect(r, {0, 255, 0, 80});
        }
    }

    target.popClip();
}

bool WorldPreview::handleInput(const InputEvent& e, const UIContext& context) {
    if (!enabled || !visible) return false;

    if (e.type == InputEvent::MouseClick && e.pressed) {
        if (!contains(e.x, e.y)) return false;

        if (e.button == InputEvent::MouseButton::LEFT_BUTTON) {
            if (hoveredRegion && selectedRegion != hoveredRegion) {
                selectedRegion = hoveredRegion;
                selectedX = hoveredX;
                selectedY = hoveredY;
                onRegionSelected.emit(*selectedRegion);
            }
            dragging = true;
            lastMouseX = e.x;
            lastMouseY = e.y;
            return true;
        }
    }

    if (e.type == InputEvent::MouseClick && !e.pressed) {
        dragging = false;
    }

    if (e.type == InputEvent::MouseMove && dragging) {
        int dx = e.x - lastMouseX;
        int dy = e.y - lastMouseY;

        offsetX -= dx;
        offsetY -= dy;

        lastMouseX = e.x;
        lastMouseY = e.y;

        wrapOffset();

        cacheDirty = true;
        return true;
    }
    if (e.type == InputEvent::MouseWheel && contains(e.x, e.y)) {
        float oldZoom = zoom;

        zoom += e.delta * 0.5f;
        zoom = std::clamp(zoom, minZoom, maxZoom);

        // зум к курсору
        float mx = e.x - computedRect.x;
        float my = e.y - computedRect.y;

        offsetX = (offsetX + mx) * (zoom / oldZoom) - mx;
        offsetY = (offsetY + my) * (zoom / oldZoom) - my;

        wrapOffset();

        cacheDirty = true;
        return true;
    }

    if (e.type == InputEvent::MouseMove && contains(e.x, e.y)) {
        if (!world) return false;
        wp = screenToWorld(e.x, e.y);

        int gx = (int)std::floor(wp.x);
        int gy = (int)std::floor(wp.y);

        gx %= world->width;
        if (gx < 0) gx += world->width;

        if (gy >= 0 && gy < world->height) {
            if (gx != hoveredX || gy != hoveredY) {
                hoveredX = gx;
                hoveredY = gy;

                hoveredRegion = &world->regions[gy * world->width + gx];

                onRegionHovered.emit(*hoveredRegion);
            }
        } else {
            hoveredX = hoveredY = -1;
            hoveredRegion = nullptr;
        }
    }

    return false;
}

void WorldPreview::wrapOffset() {
    if (!world) return;
    float worldPixelW = world->width * zoom;
    // fmod keeps offsetX in [0, worldPixelW)
    offsetX = std::fmod(offsetX, worldPixelW);
    if (offsetX < 0) offsetX += worldPixelW;
}

void WorldPreview::buildCache(RenderTarget& target) {
    if (!world) return;

    const float scale = zoom;

    terrain_verts.resize(0);
    terrain_indices.resize(0);

    const float margin = 2.0f;
    const float viewL = (offsetX - computedRect.x) / scale - margin;
    const float viewT = (offsetY - computedRect.y) / scale - margin;
    const float viewR = viewL + computedRect.width / scale + margin * 2.0f;
    const float viewB = viewT + computedRect.height / scale + margin * 2.0f;

    int w = world->width;
    int h = world->height;

    int lodLevel;

    if (zoom > 20.0f)
        lodLevel = 0;
    else if (zoom > 15.0f)
        lodLevel = 1;
    else if (zoom > 10.0f)
        lodLevel = 2;
    else if (zoom > 5.0f)
        lodLevel = 3;
    else if (zoom > 3.5f)
        lodLevel = 4;
    else if (zoom > 2.0f)
        lodLevel = 5;
    else if (zoom > 1.0f)
        lodLevel = 6;
    else
        lodLevel = 7;

    for (size_t layer = 0; layer < world->regionMeshes.size(); ++layer) {
        const RegionMesh& mesh = world->regionMeshes[layer];

        const float worldW = float(world->width);

        int minCX = int(std::floor(viewL / mesh.chunk_size.x)) - 1;
        int maxCX = int(std::floor(viewR / mesh.chunk_size.x)) + 1;

        int minCY = int(viewT) / mesh.chunk_size.y;
        int maxCY = int(viewB) / mesh.chunk_size.y;

        minCY = std::max(0, minCY);
        maxCY = std::min(mesh.chunks_y - 1, maxCY);

        auto wrapChunkX = [&](int cx) {
            int m = cx % mesh.chunks_x;
            if (m < 0) m += mesh.chunks_x;
            return m;
        };

        auto wrapXCoord = [&](int x) {
            x %= w;
            if (x < 0) x += w;
            return x;
        };

        size_t approxChunkCount = (maxCX - minCX + 1) * (maxCY - minCY + 1);
        terrain_verts.reserve(approxChunkCount * 256);
        terrain_indices.reserve(approxChunkCount * 384);

        for (int cy = minCY; cy <= maxCY; ++cy) {
            for (int cx = minCX; cx <= maxCX; ++cx) {
                int wrappedCX = wrapChunkX(cx);

                int tileIndex = cx / mesh.chunks_x;
                if (cx < 0 && cx % mesh.chunks_x) --tileIndex;
                float offsetXWorld = float(tileIndex) * worldW;

                const MeshChunk& chunk = mesh.getChunk(wrappedCX, cy);
                const std::vector<TerrainVertex>* tris = nullptr;

                switch (lodLevel) {
                    case 0:
                        tris = &chunk.lod0;
                        break;
                    case 1:
                        tris = &chunk.lod1;
                        break;
                    case 2:
                        tris = &chunk.lod2;
                        break;
                    case 3:
                        tris = &chunk.lod3;
                        break;
                    case 4:
                        tris = &chunk.lod4;
                        break;
                    case 5:
                        tris = &chunk.lod5;
                        break;
                    case 6:
                        tris = &chunk.lod6;
                        break;
                    case 7:
                        tris = &chunk.lod7;
                        break;
                }

                for (size_t t = 0; t + 11 < tris->size(); t += 12) {
                    uint32_t baseIndex = (uint32_t)terrain_verts.size();

                    // process 4 triangles (12 verts)
                    for (int k = 0; k < 12; k += 3) {
                        const auto& v0 = (*tris)[t + k + 0];
                        const auto& v1 = (*tris)[t + k + 1];
                        const auto& v2 = (*tris)[t + k + 2];

                        float x0 = v0.x + offsetXWorld;
                        float x1 = v1.x + offsetXWorld;
                        float x2 = v2.x + offsetXWorld;

                        terrain_verts.push_back({x0, v0.y, v0.z, x0, v0.y,
                                                 v0.biome, v0.moisture,
                                                 v0.temperature, v0.height});

                        terrain_verts.push_back({x1, v1.y, v1.z, x1, v1.y,
                                                 v1.biome, v1.moisture,
                                                 v1.temperature, v1.height});

                        terrain_verts.push_back({x2, v2.y, v2.z, x2, v2.y,
                                                 v2.biome, v2.moisture,
                                                 v2.temperature, v2.height});
                    }

                    terrain_indices.insert(
                        terrain_indices.end(),
                        {baseIndex + 0, baseIndex + 1, baseIndex + 2,
                         baseIndex + 3, baseIndex + 4, baseIndex + 5,
                         baseIndex + 6, baseIndex + 7, baseIndex + 8,
                         baseIndex + 9, baseIndex + 10, baseIndex + 11});
                }

                // tail (remaining triangles)
                for (size_t t = (tris->size() / 12) * 12; t + 2 < tris->size();
                     t += 3) {
                    uint32_t baseIndex = (uint32_t)terrain_verts.size();

                    const auto& v0 = (*tris)[t + 0];
                    const auto& v1 = (*tris)[t + 1];
                    const auto& v2 = (*tris)[t + 2];

                    float x0 = v0.x + offsetXWorld;
                    float x1 = v1.x + offsetXWorld;
                    float x2 = v2.x + offsetXWorld;

                    terrain_verts.push_back({x0, v0.y, v0.z, x0, v0.y, v0.biome,
                                             v0.moisture, v0.temperature,
                                             v0.height});
                    terrain_verts.push_back({x1, v1.y, v1.z, x1, v1.y, v1.biome,
                                             v1.moisture, v1.temperature,
                                             v1.height});
                    terrain_verts.push_back({x2, v2.y, v2.z, x2, v2.y, v2.biome,
                                             v2.moisture, v2.temperature,
                                             v2.height});

                    terrain_indices.push_back(baseIndex + 0);
                    terrain_indices.push_back(baseIndex + 1);
                    terrain_indices.push_back(baseIndex + 2);
                }
            }
        }
    }

    // how many horizontal repeats are visible
    int repeatMin = int(std::floor(viewL / w)) - 1;
    int repeatMax = int(std::floor(viewR / w)) + 1;

    if (zoom > 4.0f) {
        river_verts.resize(0);
        river_indices.resize(0);

        const RiverMesh& rm = world->riverMesh;

        int minCX = int(std::floor(viewL / rm.chunk_size.x)) - 1;
        int maxCX = int(std::floor(viewR / rm.chunk_size.x)) + 1;

        int minCY = int(std::floor(viewT / rm.chunk_size.y)) - 1;
        int maxCY = int(std::floor(viewB / rm.chunk_size.y)) + 1;

        minCY = std::max(0, minCY);
        maxCY = std::min(rm.chunks_y - 1, maxCY);

        auto wrapChunkX = [&](int cx) {
            int m = cx % rm.chunks_x;
            if (m < 0) m += rm.chunks_x;
            return m;
        };

        auto chunkWorldRect = [&](int cx, int cy) {
            int tileIndex = int(std::floor(float(cx) / float(rm.chunks_x)));
            float worldOffsetX = float(tileIndex) * w;
            int wrappedCX = ((cx % rm.chunks_x) + rm.chunks_x) % rm.chunks_x;

            return Rect{float(wrappedCX * rm.chunk_size.x) + worldOffsetX,
                        float(cy * rm.chunk_size.y), float(rm.chunk_size.x),
                        float(rm.chunk_size.y)};
        };

        size_t approxChunkCount = (maxCX - minCX + 1) * (maxCY - minCY + 1);
        river_verts.reserve(approxChunkCount * 256);
        river_indices.reserve(approxChunkCount * 384);

        for (int cy = minCY; cy <= maxCY; ++cy) {
            for (int cx = minCX; cx <= maxCX; ++cx) {
                int wrappedCX = wrapChunkX(cx);

                const RiverChunk& chunk = rm.getChunk(wrappedCX, cy);

                if (chunk.vertices.empty()) {
                    continue;
                }

                int tileIndex = int(std::floor(float(cx) / float(rm.chunks_x)));
                float offsetXWorld = float(tileIndex) * w;

                // chunk-level culling (важно)
                Rect cb = chunkWorldRect(cx, cy);
                if (cb.x > viewR || cb.x + cb.width < viewL || cb.y > viewB ||
                    cb.y + cb.height < viewT)
                    continue;
                for (size_t t = 0; t + 2 < chunk.vertices.size(); t += 3) {
                    const auto& a = chunk.vertices[t + 0];
                    const auto& b = chunk.vertices[t + 1];
                    const auto& c = chunk.vertices[t + 2];

                    float ax = a.x + offsetXWorld;
                    float bx = b.x + offsetXWorld;
                    float cx = c.x + offsetXWorld;

                    float txMin = std::min(std::min(ax, bx), cx);
                    float txMax = std::max(std::max(ax, bx), cx);
                    float tyMin = std::min(std::min(a.y, b.y), c.y);
                    float tyMax = std::max(std::max(a.y, b.y), c.y);

                    if (txMax < viewL || txMin > viewR || tyMax < viewT ||
                        tyMin > viewB)
                        continue;

                    uint32_t baseIndex = (uint32_t)river_verts.size();

                    river_verts.push_back({ax, a.y, a.z, ax, a.y, a.placeholder,
                                           a.flow, a.side, a.height});

                    river_verts.push_back({bx, b.y, b.z, bx, b.y, b.placeholder,
                                           b.flow, b.side, b.height});

                    river_verts.push_back({cx, c.y, c.z, cx, c.y, c.placeholder,
                                           c.flow, c.side, c.height});

                    river_indices.push_back(baseIndex + 0);
                    river_indices.push_back(baseIndex + 1);
                    river_indices.push_back(baseIndex + 2);
                }
            }
        }
    }

    if (!terrain_verts.empty()) {
        terrainMesh.update(terrain_verts.data(), terrain_verts.size(),
                           terrain_indices.data(), terrain_indices.size());
    }
    if (!river_verts.empty()) {
        riverMesh.update(river_verts.data(), river_verts.size(),
                         river_indices.data(), river_indices.size());
    }
}

WorldPreview::~WorldPreview() {}

WorldPreview::WorldPreview(const UIContext& context) {
    terrainMesh.initDynamic<TerrainVertex>(context.renderer, 2048, 2048 * 3);
    riverMesh.initDynamic<RiverVertex>(context.renderer, 1024, 1024 * 3);
    u_zoom.initFloat("u_zoom", zoom);
    u_time.initFloat("u_time", context.platform->getTimeMs() / 1000.0f);
    u_min_temp_c.initFloat("u_min_temp_c", 0);
    u_max_temp_c.initFloat("u_max_temp_c", 0);
    u_world_width.initFloat("u_world_width", 0);

    terrain_verts.reserve(10000);
    terrain_indices.reserve(15000);

    river_verts.reserve(1000);
    river_indices.reserve(500);
}

Vec2 WorldPreview::screenToWorld(int mx, int my) const {
    float x = (mx - computedRect.x + offsetX) / zoom;
    float y = (my - computedRect.y + offsetY) / zoom;

    float worldW = float(world->width);

    // wrap X into [0, worldW)
    x = std::fmod(x, worldW);
    if (x < 0) x += worldW;

    return {x, y};
}

std::optional<const Region*> WorldPreview::getSelectedRegion() {
    if (!selectedRegion) return std::nullopt;
    return selectedRegion;
}
