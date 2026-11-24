#pragma once

#include <functional>
#include <cstdint>

struct Vec2u { 
    uint32_t x, y; 

    bool operator==(const Vec2u& other) const {
        return (x == other.x && y == other.y);
    }

    bool operator!=(const Vec2u& other) const {
        return !(*this == other);
    }
};


struct Vec2i {
    int x, y;


    bool operator==(const Vec2i& other) const {
        return (x == other.x && y == other.y);
    }

    bool operator!=(const Vec2i& other) const {
        return !(*this == other);
    }
};

struct Vec2 {
    float x, y;


    bool operator==(const Vec2& other) const {
        return (x == other.x && y == other.y);
    }

    bool operator!=(const Vec2& other) const {
        return !(*this == other);
    }

    Vec2 operator-(const Vec2& other) const {
        return {x - other.x, y - other.y};
    }

    Vec2 operator+(const Vec2& other) const {
        return {x + other.x, y + other.y};
    }

    constexpr Vec2() : x(0), y(0) {}

    constexpr Vec2(float x, float y){
        this->x = x;
        this->y = y;
    }

    Vec2(const Vec2i& v){
        x = v.x;
        y = v.y;
    }
};

struct Vec3 {
    float x, y, z;


    bool operator==(const Vec3& other) const {
        return (x == other.x && y == other.y && z == other.z);
    }

    bool operator!=(const Vec3& other) const {
        return !(*this == other);
    }

    Vec3 operator-(const Vec3& other) const {
        return Vec3(x - other.x, y - other.y, z - other.z);
    }

    Vec3 operator+(const Vec3& other) const {
        return Vec3(x + other.x, y + other.y, z + other.z);
    }

    Vec3 operator*(const Vec3& other) const {
        return Vec3(x * other.x, y * other.y, z * other.z);
    }

    Vec3 operator*(const float& scalar) const {
        return Vec3(x * scalar, y * scalar, z * scalar);
    }
};

namespace std {
    template <>
    struct hash<Vec2i> {
        size_t operator()(const Vec2i& v) const {
            return std::hash<float>()(v.x) ^ (std::hash<float>()(v.y) << 1);
        }
    };

    template <>
    struct hash<Vec2u> {
        size_t operator()(const Vec2u& v) const {
            return std::hash<float>()(v.x) ^ (std::hash<float>()(v.y) << 1);
        }
    };

    template <>
    struct hash<Vec2> {
        size_t operator()(const Vec2& v) const {
            return std::hash<float>()(v.x) ^ (std::hash<float>()(v.y) << 1);
        }
    };
}

struct Rect {
    float x, y;
    float width, height;

    bool operator==(const Rect& other) const {
        return (x == other.x && y == other.y && width == other.width && height == other.height);
    }

    bool operator!=(const Rect& other) const {
        return !(*this == other);
    }

    static Rect makeRect(float x, float y, float w, float h) {
        return {x, y, w, h};
    }

    bool contains(int x, int y)
    {
        return x >= this->x && x <= this->x + this->width && y >= this->y && y <= this->y + this->height;
    }

    Rect intersect(const Rect& other)
    {
        float x1 = std::max(x, other.x);
        float y1 = std::max(y, other.y);
        float x2 = std::min(x + width,  other.x + other.width);
        float y2 = std::min(y + height, other.y + other.height);

        if (x2 < x1 || y2 < y1)
            return Rect(0,0,0,0);

        return Rect(x1, y1, x2 - x1, y2 - y1);
    }
};

struct Anchor {
    float minX;
    float minY;
    float maxX;
    float maxY;
};

struct Offset {
    float left;
    float top;
    float right;
    float bottom;
};