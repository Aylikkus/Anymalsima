#pragma once

#include "serializable_api.hpp"
#include "file_io.hpp"

class Saveable : public ISerializable {
public:
    void loadIfExists(const std::string& path, bool create = false);
    void load(const std::string& path);
    void save(const std::string& path);
};