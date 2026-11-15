#pragma once
#include <filesystem>
#include <string>

#include "binary_stream.hpp"
#include "serializable_api.hpp"

class FileIO {
   public:
    static bool fileExists(const std::string& path);
    static Json readJson(const std::string& path);
    static void writeJson(const std::string& path, const Json& j);

    // Binary and crash-safe helpers used by world saves. They throw
    // std::runtime_error (or std::filesystem::filesystem_error) on failure.
    static Bytes readBytes(const std::filesystem::path& path);
    // Writes path + ".tmp", flushes it, then renames it over path so readers
    // see either the old file or the complete new one.
    static void writeBytesAtomic(const std::filesystem::path& path,
                                 ByteView data);
    static void writeJsonAtomic(const std::filesystem::path& path,
                                const Json& j);
    static void ensureDirectory(const std::filesystem::path& path);
};
