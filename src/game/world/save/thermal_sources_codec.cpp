#include "thermal_sources_codec.hpp"

#include <algorithm>
#include <cmath>

namespace thermal_sources_codec {
namespace {
constexpr uint16_t Version = 1;
constexpr uint16_t RoomVersion = 1;
constexpr uint32_t MaxRecords = 1u << 20;
constexpr uint32_t MaxRooms = 1u << 14;
constexpr size_t MaxItemName = 128;

bool finitePosition(const std::array<double, 3>& p) {
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

bool validRoom(const RoomRecord& room) {
    return std::isfinite(room.airC) && std::isfinite(room.wallC) &&
           std::isfinite(room.lastDay) && room.lastDay >= 0;
}

Bytes encodeRecords(Bytes heatPayload, std::vector<RoomRecord> rooms,
                    const SectionContainer& extra) {
    SectionContainer container = extra;
    container.put("HEAT", Version, std::move(heatPayload));
    if (!rooms.empty()) {
        std::sort(rooms.begin(), rooms.end(),
                  [](const RoomRecord& a, const RoomRecord& b) {
                      if (a.x != b.x) return a.x < b.x;
                      if (a.y != b.y) return a.y < b.y;
                      return a.z < b.z;
                  });
        BinaryWriter out;
        out.write(uint32_t(rooms.size()));
        for (const auto& room : rooms) {
            out.write(int32_t(room.x));
            out.write(int32_t(room.y));
            out.write(int32_t(room.z));
            out.write(room.airC);
            out.write(room.wallC);
            out.write(room.lastDay);
        }
        container.put("ROOM", RoomVersion, out.take());
    } else {
        container.remove("ROOM");
    }
    return container.encode();
}
}  // namespace

Bytes encode(std::vector<Record> records, const SectionContainer& extra) {
    std::sort(records.begin(), records.end(),
              [](const Record& a, const Record& b) {
                  if (a.key != b.key) return a.key < b.key;
                  if (a.position != b.position) return a.position < b.position;
                  return a.item < b.item;
              });
    BinaryWriter out;
    out.write(uint32_t(records.size()));
    for (const auto& record : records) {
        out.write(record.position[0]);
        out.write(record.position[1]);
        out.write(record.position[2]);
        out.writeString(record.item);
        out.write(record.key);
        out.write(uint8_t(record.carried ? 1 : 0));
    }
    return encodeRecords(out.take(), {}, extra);
}

Bytes encode(std::vector<Record> records, std::vector<RoomRecord> rooms,
             const SectionContainer& extra) {
    std::sort(records.begin(), records.end(),
              [](const Record& a, const Record& b) {
                  if (a.key != b.key) return a.key < b.key;
                  if (a.position != b.position) return a.position < b.position;
                  return a.item < b.item;
              });
    BinaryWriter out;
    out.write(uint32_t(records.size()));
    for (const auto& record : records) {
        out.write(record.position[0]);
        out.write(record.position[1]);
        out.write(record.position[2]);
        out.writeString(record.item);
        out.write(record.key);
        out.write(uint8_t(record.carried ? 1 : 0));
    }
    return encodeRecords(out.take(), std::move(rooms), extra);
}

File decode(ByteView bytes) {
    File file;
    file.extra = SectionContainer::decode(bytes);
    if (const auto* section = file.extra.find("HEAT")) {
        if (section->version != Version)
            throw SerializationError("unsupported HEAT version");
        BinaryReader in(section->data);
        const auto count = in.read<uint32_t>();
        if (count > MaxRecords)
            throw SerializationError("too many thermal sources");
        file.records.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Record record;
            record.position[0] = in.read<double>();
            record.position[1] = in.read<double>();
            record.position[2] = in.read<double>();
            record.item = in.readString(MaxItemName);
            record.key = in.read<uint64_t>();
            const auto carried = in.read<uint8_t>();
            if (record.item.empty())
                throw SerializationError("thermal source without an item");
            if (!finitePosition(record.position))
                throw SerializationError(
                    "thermal source position is not finite");
            if (carried > 1)
                throw SerializationError("bad thermal carried flag");
            record.carried = carried != 0;
            file.records.push_back(std::move(record));
        }
        if (!in.atEnd()) throw SerializationError("trailing HEAT data");
    }
    if (const auto* section = file.extra.find("ROOM")) {
        if (section->version != RoomVersion)
            throw SerializationError("unsupported ROOM version");
        BinaryReader in(section->data);
        const auto count = in.read<uint32_t>();
        if (count > MaxRooms) throw SerializationError("too many rooms");
        file.rooms.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            RoomRecord room;
            room.x = in.read<int32_t>();
            room.y = in.read<int32_t>();
            room.z = in.read<int32_t>();
            room.airC = in.read<float>();
            room.wallC = in.read<float>();
            room.lastDay = in.read<double>();
            if (!validRoom(room))
                throw SerializationError("invalid room heat record");
            file.rooms.push_back(room);
        }
        if (!in.atEnd()) throw SerializationError("trailing ROOM data");
    }
    file.extra.remove("HEAT");
    file.extra.remove("ROOM");
    return file;
}

}  // namespace thermal_sources_codec
