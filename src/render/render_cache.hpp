#pragma once
#include <string>
#include <cstdint>
#include "render/render_commands.hpp"
struct TextMeasureKey {
    std::string text;
    FontRole font;
    uint32_t size;

    bool operator==(const TextMeasureKey& other) const {
        return text == other.text &&
               font == other.font &&
               size == other.size;
    }
};
struct WrapKey {
    std::vector<int32_t> codepoints;
    FontRole font;
    uint32_t size;
    float width;

    bool operator==(const WrapKey& other) const {
        return codepoints == other.codepoints &&
               font == other.font &&
               size == other.size &&
               width == other.width;
    }
};

template<>
struct std::hash<TextMeasureKey> {
    size_t operator()(const TextMeasureKey& k) const {
        size_t h = std::hash<std::string>()(k.text);
        h ^= std::hash<int>()((int)k.font) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>()(k.size);
        return h;
    }
};

template<>
struct std::hash<WrapKey> {
    size_t operator()(const WrapKey& k) const {
        size_t h = 0;
        for (int32_t cp : k.codepoints)
            h ^= std::hash<int32_t>()(cp) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()((int)k.font)  + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>()(k.size)  + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<float>()(k.width)    + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};