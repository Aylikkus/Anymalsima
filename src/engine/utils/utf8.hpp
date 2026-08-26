#pragma once

#include <string>
#include <sstream>
#include <type_traits>
#include <vector>

// Возвращает список codepoints из UTF-8 строки
inline std::vector<int32_t> utf8ToCodepoints(std::string_view text) {
    std::vector<int32_t> result;
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = text[i];
        int32_t cp;
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = (c & 0x1F) << 6 | (text[i + 1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = (c & 0x0F) << 12 | (text[i + 1] & 0x3F) << 6 |
                 (text[i + 2] & 0x3F);
            i += 3;
        } else {
            cp = (c & 0x07) << 18 | (text[i + 1] & 0x3F) << 12 |
                 (text[i + 2] & 0x3F) << 6 | (text[i + 3] & 0x3F);
            i += 4;
        }
        result.push_back(cp);
    }
    return result;
}

