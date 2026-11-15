#include "binary_stream.hpp"

void BinaryWriter::writeVarint(uint64_t value) {
    while (value >= 0x80) {
        data.push_back(std::byte((value & 0x7F) | 0x80));
        value >>= 7;
    }
    data.push_back(std::byte(value));
}

void BinaryWriter::writeString(std::string_view value) {
    writeVarint(value.size());
    const auto* begin = reinterpret_cast<const std::byte*>(value.data());
    data.insert(data.end(), begin, begin + value.size());
}

void BinaryWriter::writeBytes(ByteView value) {
    data.insert(data.end(), value.begin(), value.end());
}

void BinaryWriter::writeBlock(ByteView value) {
    if (value.size() > UINT32_MAX)
        throw SerializationError("block exceeds 4 GiB");
    write(uint32_t(value.size()));
    writeBytes(value);
}

void BinaryWriter::writeTag(std::string_view fourcc) {
    if (fourcc.size() != 4) throw SerializationError("tag must be 4 chars");
    for (char c : fourcc) data.push_back(std::byte(c));
}

void BinaryWriter::patchU32(size_t offset, uint32_t value) {
    if (offset + 4 > data.size())
        throw SerializationError("patch out of range");
    for (size_t i = 0; i < 4; ++i)
        data[offset + i] = std::byte((value >> (8 * i)) & 0xFF);
}

uint64_t BinaryReader::readVarint() {
    uint64_t result = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        const auto byte = read<uint8_t>();
        result |= uint64_t(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return result;
    }
    throw SerializationError("varint too long");
}

std::string BinaryReader::readString(size_t maxLength) {
    const auto length = readVarint();
    if (length > maxLength) throw SerializationError("string too long");
    const auto bytes = readBytes(size_t(length));
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

ByteView BinaryReader::readBytes(size_t count) {
    require(count);
    auto result = data.subspan(pos, count);
    pos += count;
    return result;
}

ByteView BinaryReader::readBlock(size_t maxSize) {
    const auto size = read<uint32_t>();
    if (size > maxSize) throw SerializationError("block too large");
    return readBytes(size);
}

std::string BinaryReader::readTag() {
    const auto bytes = readBytes(4);
    return {reinterpret_cast<const char*>(bytes.data()), 4};
}
