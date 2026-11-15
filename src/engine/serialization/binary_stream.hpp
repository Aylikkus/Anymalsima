#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

// Raised for truncated, oversized, or otherwise malformed binary data.
struct SerializationError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Little-endian writer for save data. Values are written field by field, so
// struct padding and host endianness never reach the disk.
class BinaryWriter {
   public:
    template <typename T>
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    void write(T value) {
        if constexpr (std::is_enum_v<T>) {
            write(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (std::is_same_v<T, bool>) {
            write(uint8_t(value ? 1 : 0));
        } else if constexpr (std::is_floating_point_v<T>) {
            using Bits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
            write(std::bit_cast<Bits>(value));
        } else {
            using U = std::make_unsigned_t<T>;
            U bits = static_cast<U>(value);
            for (size_t i = 0; i < sizeof(T); ++i)
                data.push_back(std::byte((bits >> (8 * i)) & 0xFF));
        }
    }

    void writeVarint(uint64_t value);
    void writeString(std::string_view value);
    void writeBytes(ByteView value);
    // Length-prefixed (u32) byte block.
    void writeBlock(ByteView value);
    void writeTag(std::string_view fourcc);

    // Overwrites a previously written u32 at offset (for back-patching sizes).
    void patchU32(size_t offset, uint32_t value);

    size_t size() const { return data.size(); }
    const Bytes& bytes() const { return data; }
    Bytes take() { return std::move(data); }

   private:
    Bytes data;
};

// Bounds-checked reader; every read throws SerializationError on truncation.
class BinaryReader {
   public:
    explicit BinaryReader(ByteView data) : data(data) {}

    template <typename T>
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    T read() {
        if constexpr (std::is_enum_v<T>) {
            return static_cast<T>(read<std::underlying_type_t<T>>());
        } else if constexpr (std::is_same_v<T, bool>) {
            return read<uint8_t>() != 0;
        } else if constexpr (std::is_floating_point_v<T>) {
            using Bits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
            return std::bit_cast<T>(read<Bits>());
        } else {
            require(sizeof(T));
            using U = std::make_unsigned_t<T>;
            U bits = 0;
            for (size_t i = 0; i < sizeof(T); ++i)
                bits |= U(std::to_integer<uint8_t>(data[pos + i])) << (8 * i);
            pos += sizeof(T);
            return static_cast<T>(bits);
        }
    }

    template <typename T>
    void read(T& out) {
        out = read<T>();
    }

    uint64_t readVarint();
    std::string readString(size_t maxLength = 1 << 20);
    ByteView readBytes(size_t count);
    ByteView readBlock(size_t maxSize = 1u << 30);
    std::string readTag();

    size_t position() const { return pos; }
    size_t remaining() const { return data.size() - pos; }
    bool atEnd() const { return pos == data.size(); }
    void skip(size_t count) { readBytes(count); }

   private:
    void require(size_t count) const {
        if (count > data.size() - pos)
            throw SerializationError("unexpected end of data");
    }

    ByteView data;
    size_t pos = 0;
};
