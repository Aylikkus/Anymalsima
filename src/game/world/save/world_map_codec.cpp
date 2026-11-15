#include "world_map_codec.hpp"

#include "serialization/section_container.hpp"

namespace world_map_codec {
namespace {

constexpr uint16_t GenVersion = 1;
constexpr uint16_t RegionsVersion = 1;
constexpr uint16_t RiversVersion = 1;
constexpr uint32_t MaxSide = 1u << 16;

template <typename E>
void writeEnum(BinaryWriter& out, E value) {
    out.write(int32_t(value));
}
template <typename E>
E readEnum(BinaryReader& in) {
    return E(in.read<int32_t>());
}

Bytes encodeSettings(const WorldGenSettings& s) {
    BinaryWriter out;
    out.write(s.seed);
    out.write(int32_t(s.width));
    out.write(int32_t(s.height));
    out.write(int32_t(s.height_seed));
    out.write(int32_t(s.humidity_seed));
    out.write(int32_t(s.temperature_seed));
    out.write(int32_t(s.feature_seed));
    out.write(s.resourceRichness);
    out.write(s.energyLevel);
    out.write(s.min_temp_c);
    out.write(s.max_temp_c);
    return out.take();
}

WorldGenSettings decodeSettings(ByteView bytes) {
    BinaryReader in(bytes);
    WorldGenSettings s;
    s.seed = in.read<uint64_t>();
    s.width = in.read<int32_t>();
    s.height = in.read<int32_t>();
    s.height_seed = in.read<int32_t>();
    s.humidity_seed = in.read<int32_t>();
    s.temperature_seed = in.read<int32_t>();
    s.feature_seed = in.read<int32_t>();
    s.resourceRichness = in.read<float>();
    s.energyLevel = in.read<float>();
    s.min_temp_c = in.read<float>();
    s.max_temp_c = in.read<float>();
    return s;
}

void writeRegion(BinaryWriter& out, const Region& r) {
    out.write(r.height);
    out.write(int32_t(r.land_height_m));
    out.write(int32_t(r.land_height_sea_m));
    out.write(r.temperature);
    out.write(r.temperature_c);
    out.write(r.humidity);
    out.write(int32_t(r.water_distance));
    out.write(r.energy);
    writeEnum(out, r.biome);
    writeEnum(out, r.land);
    writeEnum(out, r.vegetation);
    writeEnum(out, r.features);
    writeEnum(out, r.river.flow);
    out.write(r.river.strength);
    writeEnum(out, r.crust);
    out.writeVarint(r.entities.size());
    for (const auto& e : r.entities) {
        out.write(int32_t(e.size));
        out.write(e.strength);
        out.write(e.temperatureResistance);
    }
}

void readRegion(BinaryReader& in, Region& r) {
    r.height = in.read<float>();
    r.land_height_m = in.read<int32_t>();
    r.land_height_sea_m = in.read<int32_t>();
    r.temperature = in.read<float>();
    r.temperature_c = in.read<float>();
    r.humidity = in.read<float>();
    r.water_distance = in.read<int32_t>();
    r.energy = in.read<float>();
    r.biome = readEnum<BiomeType>(in);
    r.land = readEnum<LandType>(in);
    r.vegetation = readEnum<VegetationType>(in);
    r.features = readEnum<FeatureMask>(in);
    r.river.flow = readEnum<RiverFlowDirection>(in);
    r.river.strength = in.read<float>();
    r.crust = readEnum<CrustType>(in);
    const auto entities = in.readVarint();
    if (entities > in.remaining()) throw SerializationError("bad entities");
    r.entities.resize(size_t(entities));
    for (auto& e : r.entities) {
        e.size = in.read<int32_t>();
        e.strength = in.read<float>();
        e.temperatureResistance = in.read<float>();
    }
}

Bytes encodeRivers(const std::vector<RiverPolyline>& rivers) {
    BinaryWriter out;
    out.writeVarint(rivers.size());
    for (const auto& river : rivers) {
        out.writeVarint(river.path.size());
        for (const auto& [point, width] : river.path) {
            out.write(point.x);
            out.write(point.y);
            out.write(width);
        }
        out.writeVarint(river.mesh.size());
        for (const auto& v : river.mesh) {
            for (float f : {v.x, v.y, v.z, v.u, v.v, v.placeholder, v.flow,
                            v.side, v.height})
                out.write(f);
        }
        for (float f : {river.bounds.x, river.bounds.y, river.bounds.width,
                        river.bounds.height})
            out.write(f);
    }
    return out.take();
}

std::vector<RiverPolyline> decodeRivers(ByteView bytes) {
    BinaryReader in(bytes);
    std::vector<RiverPolyline> rivers;
    const auto count = in.readVarint();
    if (count > in.remaining()) throw SerializationError("bad river count");
    rivers.resize(size_t(count));
    for (auto& river : rivers) {
        const auto points = in.readVarint();
        if (points > in.remaining()) throw SerializationError("bad river");
        river.path.resize(size_t(points));
        for (auto& [point, width] : river.path) {
            point.x = in.read<float>();
            point.y = in.read<float>();
            width = in.read<float>();
        }
        const auto vertices = in.readVarint();
        if (vertices > in.remaining()) throw SerializationError("bad river");
        river.mesh.resize(size_t(vertices));
        for (auto& v : river.mesh)
            for (float* f : {&v.x, &v.y, &v.z, &v.u, &v.v, &v.placeholder,
                             &v.flow, &v.side, &v.height})
                *f = in.read<float>();
        river.bounds.x = in.read<float>();
        river.bounds.y = in.read<float>();
        river.bounds.width = in.read<float>();
        river.bounds.height = in.read<float>();
    }
    return rivers;
}

const Section& require(const SectionContainer& c, std::string_view tag,
                       uint16_t version) {
    const auto* section = c.find(tag);
    if (!section)
        throw SerializationError("world map is missing " + std::string(tag));
    if (section->version != version)
        throw SerializationError("unsupported " + std::string(tag) +
                                 " version");
    return *section;
}

}  // namespace

Bytes encode(const World& world) {
    SectionContainer container;
    container.put("GENS", GenVersion, encodeSettings(world.genSettings));

    BinaryWriter regions;
    regions.write(uint32_t(world.width));
    regions.write(uint32_t(world.height));
    for (const auto& region : world.regions) writeRegion(regions, region);
    container.put("RGNS", RegionsVersion, regions.take());

    container.put("RIVR", RiversVersion, encodeRivers(world.rivers));
    return container.encode();
}

World decode(ByteView bytes) {
    const auto container = SectionContainer::decode(bytes);
    World world;
    world.genSettings =
        decodeSettings(require(container, "GENS", GenVersion).data);

    BinaryReader in(require(container, "RGNS", RegionsVersion).data);
    const auto width = in.read<uint32_t>();
    const auto height = in.read<uint32_t>();
    if (width == 0 || height == 0 || width > MaxSide || height > MaxSide)
        throw SerializationError("bad world dimensions");
    world.width = int(width);
    world.height = int(height);
    world.min_temp_c = world.genSettings.min_temp_c;
    world.max_temp_c = world.genSettings.max_temp_c;
    world.regions.resize(size_t(width) * height);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            auto& region = world.regions[size_t(y) * width + x];
            region.coord = {x, y};
            readRegion(in, region);
        }
    if (!in.atEnd()) throw SerializationError("trailing region data");

    if (const auto* rivers = container.find("RIVR")) {
        if (rivers->version != RiversVersion)
            throw SerializationError("unsupported RIVR version");
        world.rivers = decodeRivers(rivers->data);
    }
    return world;
}

}  // namespace world_map_codec
