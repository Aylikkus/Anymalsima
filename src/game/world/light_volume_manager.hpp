#pragma once

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "camera_work_queue.hpp"
#include "chunk_manager.hpp"
#include "light_volume.hpp"

// Main-thread owner of the GPU light volumes sampled by the block shader.
//
// Every drawn chunk owns one 18^3 slot inside a paged 3D atlas. A slot holds
// the chunk's light cells plus a one-cell border copied from its 26
// neighbors, so the trilinear filter interpolates identical values on both
// sides of a chunk face. Light changes only re-upload slots; they never
// remesh. Pages avoid one texture per chunk (bgfx caps textures at 4096).
class LightVolumeManager {
   public:
    // Slots per page along each axis; keep in sync with LIGHT_PAGE_INV in
    // shaders/block/block.frag.
    static constexpr int SlotsX = 8, SlotsY = 8, SlotsZ = 4;
    static constexpr int SlotsPerPage = SlotsX * SlotsY * SlotsZ;
    static constexpr int PageX = SlotsX * LightVolume::SizeX;  // 144
    static constexpr int PageY = SlotsY * LightVolume::SizeY;  // 144
    static constexpr int PageZ = SlotsZ * LightVolume::SizeZ;  // 72
    static constexpr int MaxPages = 16;
    // Sampler stage of u_light_volume in the block program.
    static constexpr uint8_t VolumeStage = 2;

    LightVolumeManager(ChunkManager& chunks, const DataStorage<Block>& blocks);
    ~LightVolumeManager();
    LightVolumeManager(const LightVolumeManager&) = delete;
    LightVolumeManager& operator=(const LightVolumeManager&) = delete;

    // Refreshes dirty slots near the focus within a frame budget and evicts
    // slots that have not been drawn recently. Call once per frame after
    // ChunkManager::uploadReadyMeshes and before drawing, so a volume that
    // waited for its chunk's new mesh changes in the same frame as it.
    void update(Vec3 focus);
    // Appends the light uniforms of a chunk about to be drawn, building its
    // volume immediately when it has none. Returns false when no slot is
    // available; the chunk must then be skipped this frame.
    // originX: world X the chunk is drawn at (its copy nearest the eye when
    // X wraps); defaults to the canonical chunk origin.
    bool bind(const ChunkCoord& coord, std::vector<ShaderProperty*>& uniforms,
              std::optional<double> originX = std::nullopt);
    size_t pageCount() const { return pages.size(); }
    size_t slotCount() const { return slots.size(); }

   private:
    struct Binding {
        ShaderProperty chunk;  // u_light_chunk: origin - 1, valid flag
        ShaderProperty slot;   // u_light_slot: slot base texel in the page
    };
    // Pages are heap-pinned: ShaderProperty cannot be moved safely, and
    // queued draw commands keep pointers to these bindings.
    struct Page {
        bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
        ShaderProperty sampler;
        std::array<Binding, SlotsPerPage> bindings;
        std::vector<uint16_t> free;
    };
    struct Slot {
        uint16_t page = 0;
        uint16_t index = 0;
        uint64_t lastUsed = 0;
    };
    struct CachedCells {
        LightVolume::Cells cells;
        bool valid = false;
        uint64_t lastUsed = 0;
    };

    void changed(const ChunkCoord& coord);
    void forget(const ChunkCoord& coord);
    bool allocate(const ChunkCoord& coord);
    void release(const ChunkCoord& coord);
    void rebuild(const ChunkCoord& coord, const Slot& slot);
    const LightVolume::Cells* cells(const ChunkCoord& coord);
    void addPage();

    ChunkManager& chunks;
    const DataStorage<Block>& blocks;
    Signal<const ChunkCoord&>::Connection readyConnection;
    Signal<const ChunkCoord&>::Connection removedConnection;
    Signal<const ChunkCoord&>::Connection lightConnection;
    std::vector<std::unique_ptr<Page>> pages;
    std::unordered_map<ChunkCoord, Slot> slots;
    std::unordered_map<ChunkCoord, CachedCells> cache;
    CameraWorkQueue dirty;
    // Frame at which a dirty slot started waiting for its chunk's remesh.
    std::unordered_map<ChunkCoord, uint64_t> deferredSince;
    bgfx::TextureFormat::Enum format = bgfx::TextureFormat::RG8;
    int bytesPerTexel = 2;
    uint64_t frame = 1;
    LightVolume::Volume scratch;
};
