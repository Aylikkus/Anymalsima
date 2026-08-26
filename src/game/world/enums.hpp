#pragma once
#include <type_traits>


enum class FeatureMask {
    None = 0,
    LAKE = 1 << 0,
    DELTA = 1 << 1,
    OASIS = 1 << 2,
    ICE_SHEET = 1 << 3,
    GLACIER = 1 << 4,
    PERMAFROST = 1 << 5,
    SEA_ICE = 1 << 6,
    RAVINE = 1 << 7,
    CLIFF = 1 << 8,
    PLATEAU = 1 << 9,
    VOLCANO = 1 << 10,
    CRATER = 1 << 11,
    SWAMP = 1 << 12,
    MARSH = 1 << 13,
    FLOODPLAIN = 1 << 14,
    MANGROVE = 1 << 15,
    DENSE_FOREST = 1 << 16,
    CLEARING = 1 << 17,
    DEAD_ZONE = 1 << 18,
    BURNED_AREA = 1 << 19,
    DESERT_DUNES = 1 << 20,
    SALT_FLATS = 1 << 21,
    BADLANDS = 1 << 22,
    BEACH = 1 << 23,
    ROCKY_SHORE = 1 << 24,
    CORAL_REEF = 1 << 25
};

// Overload the OR operator so we can combine flags
inline FeatureMask operator|(FeatureMask lhs, FeatureMask rhs) {
    return static_cast<FeatureMask>(
        static_cast<std::underlying_type_t<FeatureMask>>(lhs) |
        static_cast<std::underlying_type_t<FeatureMask>>(rhs));
}

// Overload the AND operator for checking flags
inline FeatureMask operator&(FeatureMask lhs, FeatureMask rhs) {
    return static_cast<FeatureMask>(
        static_cast<std::underlying_type_t<FeatureMask>>(lhs) &
        static_cast<std::underlying_type_t<FeatureMask>>(rhs));
}

inline FeatureMask operator~(FeatureMask m) {
    return static_cast<FeatureMask>(
        ~static_cast<std::underlying_type_t<FeatureMask>>(m));
}


enum class BiomeType {
    POLAR,
    TUNDRA,
    TEMPERATE,
    SUBTROPICAL,
    TROPICAL,
    SCORCHING
};

enum class LandType {
    DEEP_WATER,
    WATER,
    SHORE,
    PLAINS,
    HILLS,
    MOUNTAINS,
    PEAK
};

enum class VegetationType {
    NONE,
    GRASS,
    SHRUB,
    FOREST,
    RAINFOREST,
};

enum class RiverFlowDirection { NO, NW, N, NE, E, SE, S, SW, W };

enum class CrustType {
    CONTINENTAL,
    OCEANIC,
    TRANSITIONAL  // coasts, shelves
};


enum class LayerType {
    WATER,
    LAND,
    COUNT
};