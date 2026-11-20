#include "light_volume_manager.hpp"

#include <chrono>
#include <cstring>
#include <stdexcept>

#include "sunlight_propagator.hpp"

namespace {
// Generous idle windows: re-acquiring a slot rebuilds it synchronously, so
// turning the camera back must not rebuild hundreds of volumes at once.
constexpr uint64_t SlotIdleFrames = 1800;
constexpr uint64_t CacheIdleFrames = 1800;
constexpr int RebuildsPerFrame = 64;
// Upper bound on waiting for a pending remesh, so light still updates when a
// chunk keeps being remeshed.
constexpr uint64_t MaxMeshWaitFrames = 30;
constexpr auto RebuildBudget = std::chrono::microseconds(1500);

// X wraps around the world (wrap disabled: plain offsets).
bool offset(const WorldWrap& wrap, const ChunkCoord& coord, int dx, int dy,
            int dz, ChunkCoord& out) {
    const int64_t x = wrap.wrapChunk(int64_t{coord.x} + dx);
    const int64_t y = int64_t{coord.y} + dy;
    const int64_t z = int64_t{coord.z} + dz;
    if (x < 0 || y < 0 || z < 0 || x > UINT32_MAX || y > UINT32_MAX ||
        z > UINT32_MAX)
        return false;
    out = {unsigned(x), unsigned(y), unsigned(z)};
    return true;
}

template <typename F>
void forNeighborhood(const WorldWrap& wrap, const ChunkCoord& coord, F&& fn) {
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                ChunkCoord n;
                if (offset(wrap, coord, dx, dy, dz, n)) fn(n, dx, dy, dz);
            }
}
}  // namespace

LightVolumeManager::LightVolumeManager(ChunkManager& chunks,
                                       const DataStorage<Block>& blocks,
                                       const BlockLightPropagator* blockLight,
                                       const SunLightPropagator* sunLight)
    : chunks(chunks),
      blocks(blocks),
      blockLight(blockLight),
      sunLight(sunLight) {
    const auto* caps = bgfx::getCaps();
    if (!(caps->supported & BGFX_CAPS_TEXTURE_3D))
        throw std::runtime_error("Light volumes require 3D texture support");
    auto supports = [&](bgfx::TextureFormat::Enum f) {
        return (caps->formats[f] & BGFX_CAPS_FORMAT_TEXTURE_3D) != 0;
    };
    if (!supports(format))
        throw std::runtime_error("Light volumes require RGBA8 3D textures");
    if (!supports(bgfx::TextureFormat::R8))
        throw std::runtime_error("Ground cover volumes require R8 3D textures");
    readyConnection = chunks.becameReady.connect(
        [this](const ChunkCoord& coord) { changed(coord); });
    lightConnection = chunks.lightChanged.connect(
        [this](const ChunkCoord& coord) { changed(coord); });
    removedConnection = chunks.removed.connect(
        [this](const ChunkCoord& coord) { forget(coord); });
}

LightVolumeManager::~LightVolumeManager() {
    chunks.becameReady.disconnect(readyConnection);
    chunks.lightChanged.disconnect(lightConnection);
    chunks.removed.disconnect(removedConnection);
    // bgfx defers destruction until frames referencing the pages finish.
    for (auto& page : pages) {
        if (bgfx::isValid(page->texture)) bgfx::destroy(page->texture);
        if (bgfx::isValid(page->coverTexture))
            bgfx::destroy(page->coverTexture);
    }
}

void LightVolumeManager::changed(const ChunkCoord& coord) {
    if (auto it = cache.find(coord); it != cache.end())
        it->second.valid = false;
    // The chunk's cells appear in its own slot and in the border of every
    // neighbor (faces, edges and corners).
    forNeighborhood(chunks.wrap(), coord,
                    [&](const ChunkCoord& n, int, int, int) {
                        if (slots.contains(n)) dirty.insert(n);
                    });
}

void LightVolumeManager::forget(const ChunkCoord& coord) {
    release(coord);
    cache.erase(coord);
    dirty.erase(coord);
    deferredSince.erase(coord);
    // Neighbors fall back to clamping their own edge cells.
    forNeighborhood(chunks.wrap(), coord,
                    [&](const ChunkCoord& n, int, int, int) {
                        if (slots.contains(n)) dirty.insert(n);
                    });
}

const LightVolumeManager::CachedCells* LightVolumeManager::cells(
    const ChunkCoord& coord) {
    auto it = cache.find(coord);
    if (it != cache.end() && it->second.valid) {
        it->second.lastUsed = frame;
        return &it->second;
    }
    if (!chunks.isReady(coord)) return nullptr;
    auto& entry = it != cache.end() ? it->second : cache[coord];
    if (!chunks.withChunkLocked(coord, [&](const Chunk& chunk) {
            const auto* chroma =
                blockLight ? blockLight->chroma(coord) : nullptr;
            LightVolume::encode(chunk, blocks,
                                chroma ? chroma->data() : nullptr, entry.cells);
            LightVolume::encodeCover(chunk, entry.cover);
        }))
        return nullptr;
    entry.valid = true;
    entry.lastUsed = frame;
    return &entry;
}

void LightVolumeManager::addPage() {
    auto page = std::make_unique<Page>();
    page->texture = bgfx::createTexture3D(
        PageX, PageY, PageZ, false, format,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP);
    if (!bgfx::isValid(page->texture))
        throw std::runtime_error("Cannot create light volume page");
    page->sampler.initTexture("u_light_volume", VolumeStage, page->texture);
    page->coverTexture = bgfx::createTexture3D(
        PageX, PageY, PageZ, false, bgfx::TextureFormat::R8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP);
    if (!bgfx::isValid(page->coverTexture))
        throw std::runtime_error("Cannot create ground cover volume page");
    page->coverSampler.initTexture("u_cover_volume", CoverStage,
                                   page->coverTexture);
    for (auto& binding : page->bindings) {
        binding.chunk.initVec4("u_light_chunk", {0, 0, 0, 0});
        binding.slot.initVec4("u_light_slot", {0, 0, 0, 0});
    }
    page->free.reserve(SlotsPerPage);
    for (int i = SlotsPerPage - 1; i >= 0; --i)
        page->free.push_back(static_cast<uint16_t>(i));
    pages.push_back(std::move(page));
}

bool LightVolumeManager::allocate(const ChunkCoord& coord) {
    for (size_t p = 0; p < pages.size(); ++p) {
        auto& page = *pages[p];
        if (page.free.empty()) continue;
        slots[coord] = {static_cast<uint16_t>(p), page.free.back(), frame};
        page.free.pop_back();
        return true;
    }
    if (pages.size() < MaxPages) {
        addPage();
        return allocate(coord);
    }
    // Reuse the least recently drawn slot. A slot drawn this frame is never
    // stolen: its queued draw still reads the binding's current values.
    auto victim = slots.end();
    for (auto it = slots.begin(); it != slots.end(); ++it)
        if (it->second.lastUsed < frame &&
            (victim == slots.end() ||
             it->second.lastUsed < victim->second.lastUsed))
            victim = it;
    if (victim == slots.end()) return false;
    const auto reused = victim->second;
    dirty.erase(victim->first);
    deferredSince.erase(victim->first);
    slots.erase(victim);
    slots[coord] = {reused.page, reused.index, frame};
    return true;
}

void LightVolumeManager::release(const ChunkCoord& coord) {
    const auto it = slots.find(coord);
    if (it == slots.end()) return;
    pages[it->second.page]->free.push_back(it->second.index);
    slots.erase(it);
}

void LightVolumeManager::rebuild(const ChunkCoord& coord, const Slot& slot) {
    std::array<const LightVolume::Cells*, 27> neighborhood{};
    std::array<const LightVolume::CoverCells*, 27> coverNeighborhood{};
    forNeighborhood(chunks.wrap(), coord,
                    [&](const ChunkCoord& n, int dx, int dy, int dz) {
                        const auto* entry = cells(n);
                        const int i = LightVolume::neighbor(dx, dy, dz);
                        neighborhood[i] = entry ? &entry->cells : nullptr;
                        coverNeighborhood[i] = entry ? &entry->cover : nullptr;
                    });
    if (!neighborhood[LightVolume::neighbor(0, 0, 0)]) return;
    LightVolume::build(neighborhood, scratch);
    LightVolume::buildCover(coverNeighborhood, coverScratch);

    const int sx = slot.index % SlotsX;
    const int sy = (slot.index / SlotsX) % SlotsY;
    const int sz = slot.index / (SlotsX * SlotsY);
    const auto texels =
        LightVolume::SizeX * LightVolume::SizeY * LightVolume::SizeZ;
    const bgfx::Memory* memory = bgfx::alloc(texels * LightVolume::Channels);
    std::memcpy(memory->data, scratch.data(), scratch.size());
    auto& page = *pages[slot.page];
    bgfx::updateTexture3D(page.texture, 0, uint16_t(sx * LightVolume::SizeX),
                          uint16_t(sy * LightVolume::SizeY),
                          uint16_t(sz * LightVolume::SizeZ), LightVolume::SizeX,
                          LightVolume::SizeY, LightVolume::SizeZ, memory);
    bgfx::updateTexture3D(
        page.coverTexture, 0, uint16_t(sx * LightVolume::SizeX),
        uint16_t(sy * LightVolume::SizeY), uint16_t(sz * LightVolume::SizeZ),
        LightVolume::SizeX, LightVolume::SizeY, LightVolume::SizeZ,
        bgfx::copy(coverScratch.data(), uint32_t(coverScratch.size())));
    // The vertex shader turns world positions into slot-local texel
    // coordinates: texel = world - origin + 1 (the border is texel 0).
    auto& binding = page.bindings[slot.index];
    binding.chunk.setVec4({float(coord.x) * Chunk::SIZE_X - 1.f,
                           float(coord.y) * Chunk::SIZE_Y - 1.f,
                           float(coord.z) * Chunk::SIZE_Z - 1.f, 1.f});
    binding.slot.setVec4({float(sx * LightVolume::SizeX),
                          float(sy * LightVolume::SizeY),
                          float(sz * LightVolume::SizeZ), 0.f});
}

void LightVolumeManager::update(Vec3 focus) {
    ++frame;
    dirty.setPeriodX(chunks.wrap().tiles);
    const auto started = std::chrono::steady_clock::now();
    int rebuilt = 0;
    for (const auto& coord : dirty.ordered(focus, false)) {
        if (rebuilt >= RebuildsPerFrame ||
            (rebuilt &&
             std::chrono::steady_clock::now() - started >= RebuildBudget))
            break;
        const auto it = slots.find(coord);
        if (it == slots.end()) {
            dirty.erase(coord);
            deferredSince.erase(coord);
            continue;
        }
        // An edit changes opacity before its new geometry is uploaded. The
        // drawn (old) faces next to a placed block would sample that now
        // opaque cell and turn black, so the volume waits for the mesh.
        if (chunks.hasUploadedMesh(coord) && !chunks.isMeshUploaded(coord)) {
            const auto since = deferredSince.try_emplace(coord, frame).first;
            if (frame - since->second < MaxMeshWaitFrames) continue;
        }
        dirty.erase(coord);
        deferredSince.erase(coord);
        rebuild(coord, it->second);
        ++rebuilt;
    }
    for (auto it = slots.begin(); it != slots.end();) {
        if (it->second.lastUsed + SlotIdleFrames < frame) {
            pages[it->second.page]->free.push_back(it->second.index);
            dirty.erase(it->first);
            deferredSince.erase(it->first);
            it = slots.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = cache.begin(); it != cache.end();) {
        if (it->second.lastUsed + CacheIdleFrames < frame)
            it = cache.erase(it);
        else
            ++it;
    }
}

bool LightVolumeManager::prepare(const ChunkCoord& coord) {
    auto it = slots.find(coord);
    if (it == slots.end()) {
        if (!chunks.isReady(coord) || !allocate(coord)) return false;
        it = slots.find(coord);
        rebuild(coord, it->second);
        dirty.erase(coord);
        deferredSince.erase(coord);
    }
    it->second.lastUsed = frame;
    return true;
}

bool LightVolumeManager::bind(const ChunkCoord& coord,
                              std::vector<ShaderProperty*>& uniforms,
                              std::optional<double> originX) {
    if (!prepare(coord)) return false;
    const auto it = slots.find(coord);
    auto& page = *pages[it->second.page];
    auto& binding = page.bindings[it->second.index];
    // block.vert rebuilds world positions from the drawn transform, so the
    // volume origin must be in the same X frame as the draw. Estimated sky
    // light ignores structures above the chunk; w = 2 lets the shader shade
    // it with the sky occlusion map, as distant terrain is, until the sweep
    // lands.
    const float estimated =
        sunLight && sunLight->isEstimated(coord) ? 2.f : 1.f;
    binding.chunk.setVec4(
        {float(originX.value_or(double(coord.x) * Chunk::SIZE_X)) - 1.f,
         float(coord.y) * Chunk::SIZE_Y - 1.f,
         float(coord.z) * Chunk::SIZE_Z - 1.f, estimated});
    uniforms.push_back(&binding.chunk);
    uniforms.push_back(&binding.slot);
    uniforms.push_back(&page.sampler);
    uniforms.push_back(&page.coverSampler);
    return true;
}
