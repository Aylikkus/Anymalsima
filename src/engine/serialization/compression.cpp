#include "compression.hpp"

#include <zstd.h>

namespace compression {

Bytes compress(ByteView raw, int level) {
    Bytes out(ZSTD_compressBound(raw.size()));
    const size_t written =
        ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), level);
    if (ZSTD_isError(written))
        throw SerializationError(std::string("zstd compress: ") +
                                 ZSTD_getErrorName(written));
    out.resize(written);
    return out;
}

Bytes decompress(ByteView compressed, size_t rawSize) {
    if (rawSize > MaxRawSize)
        throw SerializationError("zstd: declared size too large");
    Bytes out(rawSize);
    const size_t written = ZSTD_decompress(
        out.data(), out.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(written))
        throw SerializationError(std::string("zstd decompress: ") +
                                 ZSTD_getErrorName(written));
    if (written != rawSize) throw SerializationError("zstd: size mismatch");
    return out;
}

}  // namespace compression
