#pragma once
#include "serializable_api.hpp"

class FileIO {
public:
    static bool fileExists(const std::string& path);
    static Json readJson(const std::string& path);
    static void writeJson(const std::string& path, const Json& j);
};