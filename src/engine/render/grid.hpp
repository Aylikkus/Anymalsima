#pragma once

struct UIGrid {
    int columns = 1;
    int rows = 1;

    float cellWidth() const { return 1.0f / columns; }
    float cellHeight() const { return 1.0f / rows; }
};

struct GridPlacement {
    float col = 0;
    float row = 0;
    float colSpan = 1;
    float rowSpan = 1;

    float paddingLeft = 0;
    float paddingTop = 0;
    float paddingRight = 0;
    float paddingBottom = 0;
};