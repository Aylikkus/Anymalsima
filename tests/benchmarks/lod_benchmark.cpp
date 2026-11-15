#include <chrono>
#include <iostream>

#include "lod_scene.hpp"
#include "test_assets.hpp"
#include "world/terrain_lod.hpp"

int main() {
    test::Assets assets;
    const auto world = lodScene();
    const Vec3 eye{6144, 450, 6144};
    float threshold = 2;
    std::vector<TerrainLod::Patch> layout;
    size_t estimated = 0;
    do {
        layout = TerrainLod::select(
            12288, 12288, eye, 720,
            [](const auto& p) { return p.spacing * .4f; }, false, threshold);
        estimated = 0;
        for (const auto& p : layout)
            estimated += p.spacing == 1 ? 320 * 1024 : 180 * 1024;
        if (estimated <= TerrainLod::GpuBudget * 3 / 4 || threshold >= 64)
            break;
        threshold *= 2;
    } while (threshold <= 64);
    size_t bytes = 0, maxBytes = 0, vertices = 0, triangles = 0;
    const auto start = std::chrono::steady_clock::now();
    std::atomic_bool cancel{false};
    for (const auto& p : layout) {
        SurfaceSampler sampler(world, assets);
        const auto mesh = TerrainLod::build(p, sampler, cancel);
        bytes += mesh.bytes();
        maxBytes = std::max(maxBytes, mesh.bytes());
        vertices += mesh.vertices.size();
        triangles += mesh.indices.size() / 3;
    }
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    std::cout << "seed=1234/5678 viewport=1280x720 distance=4096 patches="
              << layout.size() << " pixel_threshold=" << threshold
              << " estimated_MiB=" << estimated / 1048576.
              << " actual_MiB=" << bytes / 1048576.
              << " max_upload_KiB=" << maxBytes / 1024.
              << " vertices=" << vertices << " triangles=" << triangles
              << " build_seconds=" << seconds << '\n';
    return bytes > TerrainLod::GpuBudget || maxBytes > TerrainLod::UploadBudget
               ? 1
               : 0;
}
