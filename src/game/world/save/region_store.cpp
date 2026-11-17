#include "region_store.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

#include "serialization/file_io.hpp"

namespace {

void mergeInto(RegionSnapshot& target, RegionSnapshot&& source) {
    for (auto& [index, chunk] : source.chunks) {
        auto it = std::find_if(target.chunks.begin(), target.chunks.end(),
                               [&](const auto& c) { return c.first == index; });
        if (it == target.chunks.end())
            target.chunks.emplace_back(index, std::move(chunk));
        else
            it->second = std::move(chunk);  // newer capture wins
    }
}

}  // namespace

RegionStore::RegionStore(std::filesystem::path worldDir, ThreadPool& pool,
                         size_t warmCapacity)
    : worldDir(std::move(worldDir)), pool(pool), warmCapacity(warmCapacity) {}

RegionStore::~RegionStore() {
    try {
        flush();
    } catch (const std::exception& e) {
        std::cerr << "Region store shutdown: " << e.what() << '\n';
    }
}

void RegionStore::open(Vec2u rc) {
    auto& entry = entries[rc];
    entry.resident = true;
    entry.lastUse = ++clock;
    pump(rc, entry);
}

std::shared_ptr<const RegionData> RegionStore::ready(Vec2u rc) const {
    const auto it = entries.find(rc);
    if (it == entries.end() || it->second.job || it->second.queued)
        return nullptr;
    return it->second.data;
}

void RegionStore::store(Vec2u rc, RegionSnapshot snapshot, bool release) {
    auto& entry = entries[rc];
    entry.lastUse = ++clock;
    if (release) entry.resident = false;
    if (!snapshot.empty()) {
        if (entry.queued)
            mergeInto(*entry.queued, std::move(snapshot));
        else
            entry.queued = std::move(snapshot);
    }
    pump(rc, entry);
}

void RegionStore::release(Vec2u rc) {
    if (auto it = entries.find(rc); it != entries.end()) {
        it->second.resident = false;
        it->second.lastUse = ++clock;
    }
}

void RegionStore::pump(Vec2u rc, Entry& entry) {
    if (entry.job) return;
    if (!entry.data) {
        // Writes merge over the region's previous contents, so read first.
        if (entry.resident || entry.queued) submitRead(rc, entry);
        return;
    }
    if (entry.queued) {
        auto snapshot = std::move(*entry.queued);
        entry.queued.reset();
        submitWrite(rc, entry, std::move(snapshot));
    }
}

void RegionStore::submitRead(Vec2u rc, Entry& entry) {
    entry.job = pool.submit_task(
        [path = region_file::path(worldDir, rc)]() -> Result {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec))
                return {std::make_shared<const RegionData>()};
            try {
                return {std::make_shared<const RegionData>(
                    region_file::decode(FileIO::readBytes(path)))};
            } catch (const std::exception& e) {
                // Keep the damaged file for inspection; regenerate the
                // region rather than refusing to load the world.
                auto damaged = path;
                damaged += ".corrupt";
                std::filesystem::rename(path, damaged, ec);
                std::cerr << "Region " << path << " is unreadable (" << e.what()
                          << "); regenerating\n";
                return {std::make_shared<const RegionData>()};
            }
        },
        BS::pr::high);
}

void RegionStore::submitWrite(Vec2u rc, Entry& entry, RegionSnapshot snapshot) {
    entry.job = pool.submit_task(
        [path = region_file::path(worldDir, rc), base = entry.data,
         snapshot = std::move(snapshot)]() -> Result {
            auto merged = std::make_shared<const RegionData>(
                region_file::merge(*base, snapshot));
            try {
                FileIO::writeBytesAtomic(path, region_file::encode(*merged));
                return {merged, true};
            } catch (const std::exception& e) {
                std::cerr << "Saving region " << path << " failed: " << e.what()
                          << '\n';
                return {merged, false};
            }
        },
        // A resident region's next ready() waits for this write: keep it
        // out of the low-priority tail that heavy streaming can starve.
        entry.resident ? BS::pr::normal : BS::pr::low);
}

void RegionStore::collect(Vec2u rc, Entry& entry) {
    using namespace std::chrono_literals;
    if (!entry.job || entry.job->wait_for(0s) != std::future_status::ready)
        return;
    auto job = std::move(*entry.job);
    entry.job.reset();
    Result result;
    try {
        result = job.get();
    } catch (const std::exception& e) {
        // Encoding failed; keep the previous data and drop the snapshot.
        std::cerr << "Region job failed: " << e.what() << '\n';
        ++failures;
        if (!entry.data) entry.data = std::make_shared<const RegionData>();
        pump(rc, entry);
        return;
    }
    entry.data = std::move(result.data);
    if (!result.written) ++failures;
    entry.unsaved = !result.written;
    pump(rc, entry);
}

void RegionStore::update() {
    for (auto& [rc, entry] : entries) collect(rc, entry);
    evictCold();
}

void RegionStore::evictCold() {
    std::vector<std::pair<uint64_t, Vec2u>> cold;
    for (const auto& [rc, entry] : entries)
        if (!entry.resident && !entry.job && !entry.queued && !entry.unsaved)
            cold.emplace_back(entry.lastUse, rc);
    if (cold.size() <= warmCapacity) return;
    std::sort(cold.begin(), cold.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < cold.size() - warmCapacity; ++i)
        entries.erase(cold[i].second);
}

bool RegionStore::idle() const {
    return std::none_of(entries.begin(), entries.end(), [](const auto& e) {
        return e.second.job.has_value() || e.second.queued.has_value();
    });
}

void RegionStore::flush() {
    // Retry previously failed writes once with the data held in memory.
    for (auto& [rc, entry] : entries)
        if (entry.unsaved && !entry.queued) {
            entry.unsaved = false;
            entry.queued = RegionSnapshot{};
            pump(rc, entry);
        }
    while (!idle()) {
        for (auto& [rc, entry] : entries)
            if (entry.job) entry.job->wait();
        for (auto& [rc, entry] : entries) collect(rc, entry);
    }
    evictCold();
}
