#include "voxel_light.hpp"

#include <algorithm>
#include <cmath>

namespace voxel_light {
namespace {

struct Polygon {
    std::array<Vec2, 8> points{};
    unsigned count = 0;
};

float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }

float signedArea(const Polygon& polygon) {
    float sum = 0;
    for (unsigned i = 0; i < polygon.count; ++i)
        sum +=
            cross(polygon.points[i], polygon.points[(i + 1) % polygon.count]);
    return sum * .5f;
}

Polygon project(const tile_geometry::Polygon& input, Face face) {
    Polygon polygon;
    polygon.count = input.count;
    for (unsigned i = 0; i < input.count; ++i) {
        const auto p = input.vertices[i];
        const int axis = uint8_t(face) / 2;
        polygon.points[i] = axis == 0   ? Vec2{p.z, p.y}
                            : axis == 1 ? Vec2{p.x, p.z}
                                        : Vec2{p.x, p.y};
    }
    if (signedArea(polygon) < 0)
        std::reverse(polygon.points.begin(),
                     polygon.points.begin() + polygon.count);
    return polygon;
}

float overlap(Polygon subject, const Polygon& clip) {
    if (!clip.count) return 0;
    for (unsigned i = 0; i < clip.count && subject.count; ++i) {
        const Vec2 a = clip.points[i];
        const Vec2 b = clip.points[(i + 1) % clip.count];
        const auto side = [&](Vec2 p) {
            return cross({b.x - a.x, b.y - a.y}, {p.x - a.x, p.y - a.y});
        };
        Polygon next;
        Vec2 previous = subject.points[subject.count - 1];
        float last = side(previous);
        for (unsigned j = 0; j < subject.count; ++j) {
            const Vec2 p = subject.points[j];
            const float distance = side(p);
            if ((last < 0 && distance > 0) || (last > 0 && distance < 0)) {
                const float t = last / (last - distance);
                next.points[next.count++] =
                    Vec2{previous.x + (p.x - previous.x) * t,
                         previous.y + (p.y - previous.y) * t};
            }
            if (distance >= 0) next.points[next.count++] = p;
            previous = p;
            last = distance;
        }
        subject = next;
    }
    return signedArea(subject);
}

constexpr int Codes = (int(TileShapeProfile::DiagonalRamp) + 1) * 4;
struct Coverage {
    std::array<std::array<float, 6>, Codes> area{};
    std::array<std::array<std::array<float, 6>, Codes>, Codes> shared{};

    Coverage() {
        std::array<std::array<Polygon, 6>, Codes> polygons{};
        for (int code = 0; code < Codes; ++code) {
            const auto shape = TileShape::fromCanonicalCode(uint8_t(code));
            if (!shape) continue;
            for (int f = 0; f < 6; ++f)
                if (const auto polygon =
                        tile_geometry::boundaryFace(*shape, Face(f))) {
                    polygons[code][f] = project(*polygon, Face(f));
                    area[code][f] = signedArea(polygons[code][f]);
                }
        }
        for (int a = 0; a < Codes; ++a)
            for (int b = 0; b < Codes; ++b)
                for (int f = 0; f < 6; ++f)
                    if (area[a][f] > 0 && area[b][f ^ 1] > 0)
                        shared[a][b][f] =
                            overlap(polygons[a][f], polygons[b][f ^ 1]);
    }
};

const Coverage& coverage() {
    static const Coverage result;
    return result;
}

// Round to the nearest level, with half levels rounding up. Keep a small
// opening transmissive even if the rounded loss would be 15.
uint8_t quantize(float loss) {
    if (loss >= Opaque - 1e-5f) return Opaque;
    return uint8_t(std::clamp(int(std::floor(loss + .500001f)), 0, 14));
}
}  // namespace

namespace {

float faceArea(std::span<const Vec3> polygon, Face face) {
    const int axis = uint8_t(face) / 2;
    const auto project = [axis](Vec3 p) {
        return axis == 0   ? Vec2{p.z, p.y}
               : axis == 1 ? Vec2{p.x, p.z}
                           : Vec2{p.x, p.y};
    };
    float sum = 0;
    for (size_t i = 0; i < polygon.size(); ++i)
        sum += cross(project(polygon[i]),
                     project(polygon[(i + 1) % polygon.size()]));
    return std::abs(sum) * .5f;
}

}  // namespace

std::shared_ptr<const PieceCoverage> coverPieces(
    std::span<const tile_geometry::TilePiece> pieces) {
    auto out = std::make_shared<PieceCoverage>();
    out->pieces.assign(pieces.begin(), pieces.end());
    std::array<bool, Opaque + 1> present{};
    for (const auto& piece : pieces) {
        present[piece.blockId] = true;
        out->maximum = std::max(out->maximum, piece.blockId);
        tile_geometry::CellGeometry geometry;
        geometry.tryAdd(piece);
        for (auto& polygon : geometry.exteriorFaces())
            if (const auto face = tile_geometry::boundaryFace(polygon.plane))
                out->faces[uint8_t(*face)].push_back(
                    {piece.blockId, std::move(polygon.vertices)});
    }

    // At each opacity threshold, project the union of pieces at or above
    // it. Integrating these areas gives the strongest blocker on each ray.
    float vertical = 0;
    int previous = 0;
    for (int level = 1; level <= out->maximum; ++level) {
        if (!present[level]) continue;
        tile_geometry::CellGeometry geometry;
        for (const auto& piece : pieces)
            if (piece.blockId >= level) geometry.tryAdd(piece);
        float covered = 1;
        for (const auto& opening : geometry.overheadOpenings())
            covered -= faceArea(opening, Face::PosY);
        vertical += (level - previous) * std::clamp(covered, 0.f, 1.f);
        previous = level;
    }
    out->vertical = quantize(vertical);
    return out;
}

std::shared_ptr<const PieceCoverage> coverPieces(const SolidCellSnapshot& cell,
                                                 const OpacityTable& opacity) {
    std::vector<tile_geometry::TilePiece> pieces;
    for (const auto& entry : cell.pieces()) {
        auto piece = entry.geometry;
        piece.blockId = std::min(opacity[piece.blockId], Opaque);
        if (piece.blockId) pieces.push_back(piece);
    }
    return coverPieces(pieces);
}

uint8_t rayOpacity(const Cell& cell, Vec3 origin, Vec3 direction, float enter,
                   float leave) {
    if (!cell.compound) {
        if (cell.opacity && cell.shape.code() &&
            !tile_geometry::intersectRay(cell.shape, origin, direction, enter,
                                         leave))
            return 0;
        return cell.opacity;
    }
    uint8_t opacity = 0;
    for (const auto& piece : cell.compound->pieces) {
        if (piece.blockId <= opacity) continue;
        tile_geometry::CellGeometry geometry;
        geometry.tryAdd(piece);
        if (geometry.intersectRay(origin, direction, enter, leave))
            opacity = piece.blockId;
    }
    return opacity;
}

uint8_t shapedVerticalLoss(const Cell& cell) {
    // All supported profiles rise from their bottom footprint. Rays along
    // a zero-height edge of a ramp have zero area.
    return quantize(cell.opacity *
                    coverage().area[cell.shape.code()][uint8_t(Face::NegY)]);
}

uint8_t faceLoss(const Cell& from, const Cell& to, Face face) {
    if (from.opacity == Unknown || to.opacity == Unknown) return Opaque;
    if (from.solidOpaque() || to.solidOpaque()) return Opaque;
    if (from.compound || to.compound) {
        using Polygons = std::vector<PieceCoverage::FacePolygon>;
        const auto polygons = [](const Cell& cell, Face f,
                                 Polygons& storage) -> const Polygons& {
            if (cell.compound) return cell.compound->faces[uint8_t(f)];
            if (cell.opacity)
                if (const auto face =
                        tile_geometry::boundaryFace(cell.shape, f))
                    storage.push_back({cell.opacity,
                                       {face->vertices.begin(),
                                        face->vertices.begin() + face->count}});
            return storage;
        };
        Polygons aStorage, bStorage;
        const auto& a = polygons(from, face, aStorage);
        const auto& b = polygons(to, Face(uint8_t(face) ^ 1), bStorage);
        const auto normal = tile_geometry::boundaryFace({}, face)->plane.normal;
        float loss = 0;
        for (const auto& p : a) loss += p.opacity * faceArea(p.vertices, face);
        for (const auto& p : b) {
            loss += p.opacity * faceArea(p.vertices, face);
            auto translated = p.vertices;
            for (auto& point : translated) point = point + normal;
            for (const auto& q : a)
                loss -= std::min(p.opacity, q.opacity) *
                        faceArea(tile_geometry::clipToFace(
                                     q.vertices, translated, normal * -1),
                                 face);
        }
        return quantize(loss);
    }
    if (!from.shape.code() && !to.shape.code())
        return std::max(from.opacity, to.opacity);
    const auto& data = coverage();
    const int a = from.shape.code(), b = to.shape.code(), f = uint8_t(face);
    return quantize(from.opacity * data.area[a][f] +
                    to.opacity * data.area[b][f ^ 1] -
                    std::min(from.opacity, to.opacity) * data.shared[a][b][f]);
}

}  // namespace voxel_light
