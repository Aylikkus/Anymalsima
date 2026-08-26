#pragma once
#include <nlohmann/json.hpp>

using Json = nlohmann::json;

class ISerializable {
public:
    virtual ~ISerializable() = default;
    virtual Json serialize() const = 0;
    virtual void deserialize(const Json& j) = 0;
};