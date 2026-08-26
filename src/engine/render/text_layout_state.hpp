#pragma once
#include "core/geometry.hpp"
#include <vector>
#include <string>
#include <cstdint>

enum class FontRole {
    Regular,
    Bold,
    Regular_CJK,
    Bold_CJK,
    Count
};

enum class FontSizeRole {
    Smallest,
    Small,
    Medium,
    Large,
    Title
};

enum class TextAlign {
    Left,
    Center,
    Right,
    Justify
};

enum class VerticalAlign {
    Top,
    Middle,
    Bottom
};

struct GlyphRow {
    std::string       text;
    size_t               startIndex;
    size_t               endIndex;
    float                baselineY;
    float                baselineX;
    float                bearingX  = 0.f; // bounds.position.x (left bearing)
    float                bearingY  = 0.f; // bounds.position.y (top bearing, usually negative)
    std::vector<float>   caretXPositions;
    FontRole             font;
    int                  raw_line_index;
};

struct TextLayoutState {
    // --- inputs (used for dirty check) ---
    std::string      text;
    Rect rect;   // visible area (inner box)
    FontRole            fontRole;
    uint32_t            fontSize;
    bool                wrap;
    TextAlign           alignH;
    VerticalAlign       alignV;
    Vec2                scroll;
    
    // --- computed ---
    std::vector<GlyphRow> rows;
    float               lineHeight;
    float               totalHeight;
    float               maxLineWidth = 0.0f;
    bool                dirty = true;

    // --- caret / selection / increment (invalidated separately) ---
    size_t              caretIndex = 0;
    Vec2                caretPixelPos{0,0};
    bool                caretDirty = true;

    size_t              selStart = 0;
    size_t              selEnd   = 0;
    std::vector<Rect>   selectionRects;
    bool                selDirty = true;

    size_t incrementIdx = 0;
    int lastDelta = 0;
    bool removedNewLine = false;
    bool incrementDirty = false;

    // call when text/rect/font changes
    void markLayoutDirty() {
        dirty    = true;
        caretDirty = true;
        selDirty   = true;
        rows.clear();
    }

    void markIncrementDirty() {
        incrementDirty = true;
    }

    void markCaretDirty()     { caretDirty = true; }
    void markSelectionDirty() { selDirty   = true; }
};