#include "serialization/binary_stream.hpp"
#include "serialization/compression.hpp"
#include "serialization/section_container.hpp"
#include "test.hpp"

namespace {
template <typename F>
bool throwsSerialization(F&& fn) {
    try {
        fn();
    } catch (const SerializationError&) {
        return true;
    }
    return false;
}
Bytes bytesOf(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}
}  // namespace

TEST(binary_stream_roundtrip) {
    enum class Kind : int16_t { A = -3, B = 7 };
    BinaryWriter out;
    out.write(uint8_t(250));
    out.write(int32_t(-123456));
    out.write(uint64_t(0x0123456789ABCDEFull));
    out.write(1.5f);
    out.write(-2.25);
    out.write(true);
    out.write(Kind::A);
    out.writeVarint(300);
    out.writeString("лиса");
    out.writeTag("TEST");
    // Explicit little-endian layout, independent of the host.
    EQUAL(int(std::to_integer<uint8_t>(out.bytes()[1])), 0xC0, "LE int32");

    BinaryReader in(out.bytes());
    EQUAL(int(in.read<uint8_t>()), 250, "u8");
    EQUAL(in.read<int32_t>(), -123456, "i32");
    EQUAL(in.read<uint64_t>(), 0x0123456789ABCDEFull, "u64");
    EQUAL(in.read<float>(), 1.5f, "float");
    EQUAL(in.read<double>(), -2.25, "double");
    EQUAL(in.read<bool>(), true, "bool");
    CHECK(in.read<Kind>() == Kind::A);
    EQUAL(in.readVarint(), uint64_t(300), "varint");
    EQUAL(in.readString(), std::string("лиса"), "utf-8 string");
    EQUAL(in.readTag(), std::string("TEST"), "tag");
    CHECK(in.atEnd());
}

TEST(binary_stream_truncation) {
    BinaryWriter out;
    out.write(uint16_t(7));
    BinaryReader in(out.bytes());
    CHECK(throwsSerialization([&] { in.read<uint32_t>(); }));
    BinaryWriter text;
    text.writeVarint(1000);  // claims more bytes than present
    BinaryReader shortText(text.bytes());
    CHECK(throwsSerialization([&] { shortText.readString(); }));
}

TEST(compression_roundtrip) {
    Bytes raw(10000, std::byte{42});
    for (size_t i = 0; i < raw.size(); i += 97) raw[i] = std::byte(i & 0xFF);
    const auto packed = compression::compress(raw);
    CHECK(packed.size() < raw.size() / 4);
    CHECK(compression::decompress(packed, raw.size()) == raw);
    CHECK(throwsSerialization(
        [&] { compression::decompress(packed, raw.size() - 1); }));
    CHECK(throwsSerialization(
        [&] { compression::decompress(bytesOf("garbage!"), 8); }));
}

TEST(section_container_roundtrip) {
    SectionContainer container;
    container.put("SMAL", 1, bytesOf("tiny"));
    container.put("BIGG", 3, Bytes(4096, std::byte{1}));
    container.put("PACK", 2, Bytes(4096, std::byte{2}), false);
    container.put("SMAL", 2, bytesOf("replaced"));
    const auto encoded = container.encode();
    CHECK(encoded.size() < 4096 * 2);  // BIGG compressed, PACK kept raw

    const auto decoded = SectionContainer::decode(encoded);
    EQUAL(decoded.sections.size(), size_t(3), "section count");
    CHECK(decoded.find("SMAL")->data == bytesOf("replaced"));
    EQUAL(int(decoded.find("SMAL")->version), 2, "replaced version");
    EQUAL(int(decoded.find("BIGG")->version), 3, "kept version");
    CHECK(decoded.find("BIGG")->data == Bytes(4096, std::byte{1}));
    CHECK(decoded.find("PACK")->data == Bytes(4096, std::byte{2}));
    CHECK(decoded.find("NONE") == nullptr);

    // Unknown sections survive a decode/encode cycle unchanged.
    CHECK(SectionContainer::decode(decoded.encode()).find("BIGG")->data ==
          decoded.find("BIGG")->data);
}

TEST(section_container_rejects_bad_input) {
    CHECK(throwsSerialization(
        [&] { SectionContainer::decode(bytesOf("NOPE\1\0\0\0\0\0")); }));
    SectionContainer container;
    container.put("DATA", 1, Bytes(1000, std::byte{9}));
    auto encoded = container.encode();
    encoded.resize(encoded.size() - 5);
    CHECK(throwsSerialization([&] { SectionContainer::decode(encoded); }));
    // A newer container version must not be misread.
    auto future = container.encode();
    future[4] = std::byte{99};
    CHECK(throwsSerialization([&] { SectionContainer::decode(future); }));
}
