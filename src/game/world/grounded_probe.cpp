#include "grounded_probe.hpp"

#include <algorithm>
#include <cmath>

#include "chunk_manager.hpp"

namespace {

constexpr float ContactTolerance = .0001f;
constexpr float FollowDistance = .075f;
constexpr double MaxTravel = .04;
constexpr float Gravity = 20.f;
constexpr float TerminalSpeed = 25.f;

struct Footprint {
    double x, z;

    template <typename Visit>
    bool visit(int lowY, int highY, Visit visitCell) const {
        const int lowX = int(std::floor(x - GroundedProbe::Radius));
        const int highX = int(std::ceil(x + GroundedProbe::Radius)) - 1;
        const int lowZ = int(std::floor(z - GroundedProbe::Radius));
        const int highZ = int(std::ceil(z + GroundedProbe::Radius)) - 1;
        for (int cy = lowY; cy <= highY; ++cy)
            for (int cz = lowZ; cz <= highZ; ++cz)
                for (int cx = lowX; cx <= highX; ++cx)
                    if (!visitCell(cx, cy, cz)) return false;
        return true;
    }

    std::optional<float> top(const SolidCellSnapshot& cell, int cx, int cy,
                             int cz, float high) const {
        if (!cell.hasSolid()) return std::nullopt;
        const float localX = float(x - cx);
        const float localZ = float(z - cz);
        constexpr float r = GroundedProbe::Radius;
        const Vec2 low{localX - r, localZ - r};
        const Vec2 upper{localX + r, localZ + r};
        const auto height =
            cell.usesGeometry()
                ? cell.geometry().supportHeight(low, upper, high - cy)
                : tile_geometry::supportHeight(cell.base.shape, low, upper);
        return height ? std::optional(float(cy) + *height) : std::nullopt;
    }
};

std::optional<float> highestSupport(ChunkManager& chunks, Footprint footprint,
                                    float low, float high) {
    std::optional<float> highest;
    // A tile reaching its top boundary can support feet in the cell above.
    footprint.visit(std::max(0, int(std::ceil(low)) - 1), int(std::floor(high)),
                    [&](int x, int y, int z) {
                        const auto cell = chunks.solidCellAt({x, y, z});
                        if (!cell) return true;
                        const auto top =
                            footprint.top(cell->cell, x, y, z, high);
                        if (top && *top >= low && *top <= high &&
                            (!highest || *top > *highest))
                            highest = top;
                        return true;
                    });
    return highest;
}

bool clear(ChunkManager& chunks, Footprint footprint, float feet) {
    const float head = feet + GroundedProbe::Height;
    return footprint.visit(
        int(std::floor(feet + ContactTolerance)),
        int(std::ceil(head - ContactTolerance)) - 1, [&](int x, int y, int z) {
            const auto cell =
                chunks.solidCellAt({x, std::min(y, WORLD_HEIGHT_Y - 1), z});
            // Unknown cells, including the bounded world
            // edges, block travel.
            if (!cell) return false;
            // Open sky above the highest registered slab.
            if (y >= WORLD_HEIGHT_Y) return true;
            if (cell->cell.usesGeometry()) {
                constexpr float r = GroundedProbe::Radius;
                const float lx = float(footprint.x - x);
                const float lz = float(footprint.z - z);
                return !cell->cell.geometry().intersectsBox(
                    {lx - r, feet - y + ContactTolerance, lz - r},
                    {lx + r, head - y - ContactTolerance, lz + r});
            }
            const auto top = footprint.top(cell->cell, x, y, z, head);
            return !top || *top <= feet + ContactTolerance;
        });
}

}  // namespace

std::optional<GroundedProbe> GroundedProbe::spawn(ChunkManager& chunks,
                                                  Vec3 from) {
    const Footprint footprint{from.x, from.z};
    const auto floor = highestSupport(chunks, footprint, from.y - 128, from.y);
    if (!floor || !clear(chunks, footprint, *floor)) return std::nullopt;
    GroundedProbe probe;
    probe.x = chunks.wrap().wrapX(from.x);
    probe.z = from.z;
    probe.y = *floor;
    return probe;
}

bool GroundedProbe::move(ChunkManager& chunks, double nextX, double nextZ) {
    const Footprint footprint{nextX, nextZ};
    const auto floor =
        onGround ? highestSupport(chunks, footprint, y - FollowDistance,
                                  y + FollowDistance)
                 : std::nullopt;
    const float nextY = floor.value_or(y);
    if (!clear(chunks, footprint, nextY)) return false;

    x = nextX;
    y = nextY;
    z = nextZ;
    onGround = floor.has_value();
    return true;
}

void GroundedProbe::update(ChunkManager& chunks, Vec2 velocity, float dt) {
    // Bound work after a stalled frame. Substeps bound both horizontal travel
    // (less than the thinnest wall) and elapsed gravity integration time.
    dt = std::clamp(dt, 0.f, .25f);
    if (dt == 0) return;
    const double speed = std::hypot(double(velocity.x), double(velocity.y));
    const int steps =
        std::max(1, int(std::ceil(dt * std::max(120., speed / MaxTravel))));
    const float stepTime = dt / steps;
    const double dx = double(velocity.x) * stepTime;
    const double dz = double(velocity.y) * stepTime;
    for (int i = 0; i < steps; ++i) {
        if (!move(chunks, x + dx, z + dz)) {
            if (dx != 0) move(chunks, x + dx, z);
            if (dz != 0) move(chunks, x, z + dz);
        }

        if (!onGround) {
            verticalSpeed =
                std::min(TerminalSpeed, verticalSpeed + Gravity * stepTime);
            const float targetY = y - verticalSpeed * stepTime;
            const Footprint footprint{x, z};
            const auto floor = highestSupport(chunks, footprint, targetY,
                                              y + ContactTolerance);
            if (clear(chunks, footprint, floor.value_or(targetY))) {
                y = floor.value_or(targetY);
                onGround = floor.has_value();
            } else {
                verticalSpeed = 0;
            }
        }
        if (onGround) verticalSpeed = 0;
        x = chunks.wrap().wrapX(x);
    }
}
