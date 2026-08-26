#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>

template<typename Enum>
class EnumParser {
public:
    using Map = std::unordered_map<std::string, Enum>;
    using ReverseMap = std::unordered_map<Enum, std::string>;

    // Forward: string -> enum
    static Enum parse(const Map& map, const std::string& value) {
        auto it = map.find(value);
        if (it == map.end())
            throw std::runtime_error("Unknown enum value: " + value);
        return it->second;
    }

    // Reverse: enum -> untranslated string
    static const std::string& toString(const ReverseMap& reverseMap, Enum value) {
        auto it = reverseMap.find(value);
        if (it == reverseMap.end())
            throw std::runtime_error("Unknown enum value");
        return it->second;
    }

    // Reverse: enum -> translated string
    static const std::string& toStringLocal(const ReverseMap& reverseMapLocal, Enum value) {
        auto it = reverseMapLocal.find(value);
        if (it == reverseMapLocal.end())
            throw std::runtime_error("Unknown enum value");
        return it->second;
    }

    static std::vector<Enum> parseArray(const Map& map, const Json& arr) {
        std::vector<Enum> result;
        for (const auto& v : arr)
            result.push_back(parse(map, v.get<std::string>()));
        return result;
    }
};