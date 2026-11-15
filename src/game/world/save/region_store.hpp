#pragma once

#include <filesystem>
#include <future>
#include <optional>
#include <unordered_map>

#include "region_file.hpp"
#include "render/ui_context.hpp"

// Asynchronous, ordered disk access for region files.
//
// Main-thread state machine; workers only receive owned inputs (paths,
// immutable RegionData, chunk snapshots) and never touch the store. Each
// region has at most one job in flight; later requests wait on the main
// thread and are submitted from update(), so workers never block on futures.
// Recently released regions stay cached ("warm") so crossing back over a
// border does not hit the disk.
class RegionStore {
   public:
    RegionStore(std::filesystem::path worldDir, ThreadPool& pool,
                size_t warmCapacity = 16);
    ~RegionStore();
    RegionStore(const RegionStore&) = delete;
    RegionStore& operator=(const RegionStore&) = delete;

    // Marks the region as needed and starts reading it if necessary.
    void open(Vec2u rc);
    // The region's latest data once every earlier read/write completed;
    // null while loading. Empty data means "not generated yet".
    std::shared_ptr<const RegionData> ready(Vec2u rc) const;
    // Queues snapshot chunks to be encoded and written. With release, the
    // region is no longer needed and may leave the cache after writing.
    void store(Vec2u rc, RegionSnapshot snapshot, bool release);
    // Drops interest in a region without writing anything.
    void release(Vec2u rc);

    void update();
    // Blocks until every queued and in-flight job finished. Failed writes are
    // retried once.
    void flush();
    bool idle() const;

    const std::filesystem::path& directory() const { return worldDir; }
    size_t cachedRegions() const { return entries.size(); }
    size_t failedWrites() const { return failures; }

   private:
    struct Result {
        std::shared_ptr<const RegionData> data;
        bool written = true;
    };
    struct Entry {
        std::shared_ptr<const RegionData> data;
        std::optional<std::future<Result>> job;
        std::optional<RegionSnapshot> queued;
        bool resident = false;
        bool unsaved = false;  // the last write failed; data is newer on RAM
        uint64_t lastUse = 0;
    };

    void pump(Vec2u rc, Entry& entry);
    void submitRead(Vec2u rc, Entry& entry);
    void submitWrite(Vec2u rc, Entry& entry, RegionSnapshot snapshot);
    void collect(Vec2u rc, Entry& entry);
    void evictCold();

    std::filesystem::path worldDir;
    ThreadPool& pool;
    size_t warmCapacity;
    uint64_t clock = 0;
    size_t failures = 0;
    std::unordered_map<Vec2u, Entry> entries;
};
