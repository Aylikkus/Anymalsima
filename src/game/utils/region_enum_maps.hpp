#pragma once

#include <unordered_map>
#include <string>
#include "world/enums.hpp"
#include "gettext_wrapper.hpp"

struct RegionEnumMaps {
    // Forward maps (string -> enum) - raw untranslated strings
    static const std::unordered_map<std::string, LandType>& land() {
        static const std::unordered_map<std::string, LandType> m = {
            {__("Deep Water"), LandType::DEEP_WATER},
            {__("Water"), LandType::WATER},
            {__("Shore"), LandType::SHORE},
            {__("Plains"), LandType::PLAINS},
            {__("Hills"), LandType::HILLS},
            {__("Mountains"), LandType::MOUNTAINS},
            {__("Peak"), LandType::PEAK}
        };
        return m;
    }

    // Reverse maps (enum -> untranslated string) - raw untranslated strings
    static const std::unordered_map<LandType, std::string>& landReverse() {
        static const std::unordered_map<LandType, std::string> m = []() {
            std::unordered_map<LandType, std::string> reverse;
            for (const auto& [str, val] : land()) {
                reverse[val] = str;
            }
            return reverse;
        }();
        return m;
    }

    // Localized reverse map (enum -> translated string) - mutable, rebuilt on locale change
    static std::unordered_map<LandType, std::string>& landReverseLocalized() {
        static std::unordered_map<LandType, std::string> m;
        return m;
    }

    static const std::unordered_map<std::string, FeatureMask>& features() {
        static const std::unordered_map<std::string, FeatureMask> m = {
            {__("Lake"), FeatureMask::LAKE},
            {__("Delta"), FeatureMask::DELTA},
            {__("Oasis"), FeatureMask::OASIS},
            {__("Ice Sheet"), FeatureMask::ICE_SHEET},
            {__("Glacier"), FeatureMask::GLACIER},
            {__("Permafrost"), FeatureMask::PERMAFROST},
            {__("Sea Ice"), FeatureMask::SEA_ICE},
            {__("Ravine"), FeatureMask::RAVINE},
            {__("Cliff"), FeatureMask::CLIFF},
            {__("Plateau"), FeatureMask::PLATEAU},
            {__("Volcano"), FeatureMask::VOLCANO},
            {__("Crater"), FeatureMask::CRATER},
            {__("Swamp"), FeatureMask::SWAMP},
            {__("Marsh"), FeatureMask::MARSH},
            {__("Floodplain"), FeatureMask::FLOODPLAIN},
            {__("Mangrove"), FeatureMask::MANGROVE},
            {__("Dense Forest"), FeatureMask::DENSE_FOREST},
            {__("Clearing"), FeatureMask::CLEARING},
            {__("Dead Zone"), FeatureMask::DEAD_ZONE},
            {__("Burned Area"), FeatureMask::BURNED_AREA},
            {__("Desert Dunes"), FeatureMask::DESERT_DUNES},
            {__("Salt Flats"), FeatureMask::SALT_FLATS},
            {__("Badlands"), FeatureMask::BADLANDS},
            {__("Beach"), FeatureMask::BEACH},
            {__("Rocky Shore"), FeatureMask::ROCKY_SHORE},
            {__("Coral Reef"), FeatureMask::CORAL_REEF}
        };
        return m;
    }

    static const std::unordered_map<FeatureMask, std::string>& featuresReverse() {
        static const std::unordered_map<FeatureMask, std::string> m = []() {
            std::unordered_map<FeatureMask, std::string> reverse;
            for (const auto& [str, val] : features()) {
                reverse[val] = str;
            }
            return reverse;
        }();
        return m;
    }

    static std::unordered_map<FeatureMask, std::string>& featuresReverseLocalized() {
        static std::unordered_map<FeatureMask, std::string> m;
        return m;
    }

    static const std::unordered_map<std::string, CrustType>& crust() {
        static const std::unordered_map<std::string, CrustType> m = {
            {__("Continental"), CrustType::CONTINENTAL},
            {__("Oceanic"), CrustType::OCEANIC},
            {__("Transitional"), CrustType::TRANSITIONAL}
        };
        return m;
    }

    static const std::unordered_map<CrustType, std::string>& crustReverse() {
        static const std::unordered_map<CrustType, std::string> m = []() {
            std::unordered_map<CrustType, std::string> reverse;
            for (const auto& [str, val] : crust()) {
                reverse[val] = str;
            }
            return reverse;
        }();
        return m;
    }

    static std::unordered_map<CrustType, std::string>& crustReverseLocalized() {
        static std::unordered_map<CrustType, std::string> m;
        return m;
    }

    static const std::unordered_map<std::string, BiomeType>& biome() {
        static const std::unordered_map<std::string, BiomeType> m = {
            {__("Polar"), BiomeType::POLAR},
            {__("Scorching"), BiomeType::SCORCHING},
            {__("Subtropical"), BiomeType::SUBTROPICAL},
            {__("Temperate"), BiomeType::TEMPERATE},
            {__("Tropical"), BiomeType::TROPICAL},
            {__("Tundra"), BiomeType::TUNDRA}
        };
        return m;
    }

    static const std::unordered_map<BiomeType, std::string>& biomeReverse() {
        static const std::unordered_map<BiomeType, std::string> m = []() {
            std::unordered_map<BiomeType, std::string> reverse;
            for (const auto& [str, val] : biome()) {
                reverse[val] = str;
            }
            return reverse;
        }();
        return m;
    }


    static std::unordered_map<BiomeType, std::string>& biomeReverseLocalized() {
        static std::unordered_map<BiomeType, std::string> m;
        return m;
    }


    static const std::unordered_map<std::string, VegetationType>& vegetation() {
        static const std::unordered_map<std::string, VegetationType> m = {
            {__("Forest"), VegetationType::FOREST},
            {__("Grass"), VegetationType::GRASS},
            {__("None"), VegetationType::NONE},
            {__("Rainforest"), VegetationType::RAINFOREST},
            {__("Shrub"), VegetationType::SHRUB}
        };
        return m;
    }

    static const std::unordered_map<VegetationType, std::string>& vegetationReverse() {
        static const std::unordered_map<VegetationType, std::string> m = []() {
            std::unordered_map<VegetationType, std::string> reverse;
            for (const auto& [str, val] : vegetation()) {
                reverse[val] = str;
            }
            return reverse;
        }();
        return m;
    }


    static std::unordered_map<VegetationType, std::string>& vegetationReverseLocalized() {
        static std::unordered_map<VegetationType, std::string> m;
        return m;
    }


    static const std::unordered_map<std::string, RiverFlowDirection>& flowDirection() {
        static const std::unordered_map<std::string, RiverFlowDirection> m = {
            {__("East"), RiverFlowDirection::E},
            {__("North"), RiverFlowDirection::N},
            {__("North-East"), RiverFlowDirection::NE},
            {__("None"), RiverFlowDirection::NO},
            {__("Nort-West"), RiverFlowDirection::NW},
            {__("South"), RiverFlowDirection::S},
            {__("South-East"), RiverFlowDirection::SE},
            {__("South-West"), RiverFlowDirection::SW},
            {__("West"), RiverFlowDirection::W},
        };
        return m;
    }

    static const std::unordered_map<RiverFlowDirection, std::string>& flowDirectionReverse() {
        static const std::unordered_map<RiverFlowDirection, std::string> m = []() {
            std::unordered_map<RiverFlowDirection, std::string> reverse;
            for (const auto& [str, val] : flowDirection()) {
                reverse[val] = str;
            }
            return reverse;
        }();
        return m;
    }


    static std::unordered_map<RiverFlowDirection, std::string>& flowDirectionReverseLocalized() {
        static std::unordered_map<RiverFlowDirection, std::string> m;
        return m;
    }

    // Build localized maps - call whenever locale changes
    static void buildLocalized() {
        buildLandLocalized();
        buildFeaturesLocalized();
        buildCrustLocalized();
        buildBiomeLocalized();
        buildVegetationLocalized();
        buildFlowDirectionLocalized();
    }

private:
    static void buildLandLocalized() {
        auto& reverse = landReverse();
        auto& localized = landReverseLocalized();
        localized.clear();
        for (const auto& [val, str] : reverse) {
            localized[val] = i18n::tr(str);
        }
    }

    static void buildFeaturesLocalized() {
        auto& reverse = featuresReverse();
        auto& localized = featuresReverseLocalized();
        localized.clear();
        for (const auto& [val, str] : reverse) {
            localized[val] = i18n::tr(str);
        }
    }

    static void buildCrustLocalized() {
        auto& reverse = crustReverse();
        auto& localized = crustReverseLocalized();
        localized.clear();
        for (const auto& [val, str] : reverse) {
            localized[val] = i18n::tr(str);
        }
    }

    static void buildBiomeLocalized() {
        auto& reverse = biomeReverse();
        auto& localized = biomeReverseLocalized();
        localized.clear();
        for (const auto& [val, str] : reverse) {
            localized[val] = i18n::tr(str);
        }
    }

    static void buildVegetationLocalized() {
        auto& reverse = vegetationReverse();
        auto& localized = vegetationReverseLocalized();
        localized.clear();
        for (const auto& [val, str] : reverse) {
            localized[val] = i18n::tr(str);
        }
    }

    static void buildFlowDirectionLocalized() {
        auto& reverse = flowDirectionReverse();
        auto& localized = flowDirectionReverseLocalized();
        localized.clear();
        for (const auto& [val, str] : reverse) {
            localized[val] = i18n::tr(str);
        }
    }
};