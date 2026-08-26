#pragma once

#include <unordered_map>
#include <string>
#include "gettext_wrapper.hpp"
#include "items/enums.hpp"

struct ItemsEnumMaps {
    // Forward maps (string -> enum) - raw untranslated strings
    static const std::unordered_map<std::string, RockType>& rockTypes() {
        static const std::unordered_map<std::string, RockType> m = {
            {__("Sedimentary"), RockType::SEDIMENTARY},
            {__("Metamorphic"), RockType::METAMORPHIC},
            {__("Igneous Intrusive"), RockType::IGNEOUS_INTRUSIVE},
            {__("Igneous Extrusive"), RockType::IGNEOUS_EXTRUSIVE}
        };
        return m;
    }

    static const std::unordered_map<std::string, BlockRenderCategory>& blockRenderCategories() {
        static const std::unordered_map<std::string, BlockRenderCategory> m = {
            {__("Terrain Rock"), BlockRenderCategory::TERRAIN_ROCK},
            {__("Terrain Soil"), BlockRenderCategory::TERRAIN_SOIL},
            {__("Structure Wall"), BlockRenderCategory::STRUCTURE_WALL},
            {__("Structure Floor"), BlockRenderCategory::STRUCTURE_FLOOR},
            {__("Vegetation"), BlockRenderCategory::VEGETATION},
            {__("Furniture"), BlockRenderCategory::FURNITURE}
        };
        return m;
    }

    // Reverse maps (enum -> untranslated string) - raw untranslated strings
    static const std::unordered_map<RockType, std::string>& rockTypesReverse() {
        static const std::unordered_map<RockType, std::string> m = []() {
            std::unordered_map<RockType, std::string> reverse;
            for (const auto& [str, val] : rockTypes()) {
                reverse[val] = str;
            }
            return reverse;
        }();
        return m;
    }

    static const std::unordered_map<BlockRenderCategory, std::string>& blockRenderCategoriesReverse() {
        static const std::unordered_map<BlockRenderCategory, std::string> m = []() {
            std::unordered_map<BlockRenderCategory, std::string> reverse;
            for (const auto& [str, val] : blockRenderCategories()) {
                reverse[val] = str;
            }
            return reverse;
        }();
        return m;
    }

    // Localized reverse map (enum -> translated string) - mutable, rebuilt on locale change
    static std::unordered_map<RockType, std::string>& rockTypesReverseLocalized() {
        static std::unordered_map<RockType, std::string> m;
        return m;
    }

    static std::unordered_map<BlockRenderCategory, std::string>& blockRenderCategoriesReverseLocalized() {
        static std::unordered_map<BlockRenderCategory, std::string> m;
        return m;
    }

    // Build localized maps - call whenever locale changes
    static void buildLocalized() {
        buildRockTypesLocalized();
    }

private:
    static void buildRockTypesLocalized() {
        auto& reverse = rockTypesReverse();
        auto& localized = rockTypesReverseLocalized();
        localized.clear();
        for (const auto& [val, str] : reverse) {
            localized[val] = i18n::tr(str);
        }
    }

    static void buildBlockRenderCategoriesLocalized() {
        auto& reverse = blockRenderCategoriesReverse();
        auto& localized = blockRenderCategoriesReverseLocalized();
        localized.clear();
        for (const auto& [val, str] : reverse) {
            localized[val] = i18n::tr(str);
        }
    }
};