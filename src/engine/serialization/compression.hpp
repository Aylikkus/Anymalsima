#pragma once

#include "binary_stream.hpp"

// zstd wrappers. Decompression requires the exact raw size recorded by the
// caller, which bounds allocation for untrusted input.
namespace compression {

inline constexpr int DefaultLevel = 3;
inline constexpr size_t MaxRawSize = size_t{1} << 30;

Bytes compress(ByteView raw, int level = DefaultLevel);
Bytes decompress(ByteView compressed, size_t rawSize);

}  // namespace compression
