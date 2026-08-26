#pragma once
#include "render/text_layout_state.hpp"

class FontResolver
{
public:

    static FontRole fallbackFontFor(int32_t cp, FontRole initial)
    {
        if (cp >= 0x3040 && initial == FontRole::Regular)
            return FontRole::Regular_CJK;
        if (cp >= 0x3040 && initial == FontRole::Bold)
            return FontRole::Bold_CJK;

        return FontRole::Regular;
    }
};