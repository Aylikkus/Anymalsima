#pragma once

struct Color
{
    int r;
    int g;
    int b;
    int a;

    bool operator==(const Color& other) const {
        return (r == other.r && g == other.g && b == other.b && a == other.a);
    }

    bool operator!=(const Color& other) const {
        return !(*this == other);
    }
};
