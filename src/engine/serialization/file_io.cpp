#include "file_io.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>

Json FileIO::readJson(const std::string &path) {
    std::ifstream f(path);
    return Json::parse(f);
}

void FileIO::writeJson(const std::string &path, const Json &j) {
    std::ofstream file(path);
    file << std::setw(2) << j;
}

bool FileIO::fileExists(const std::string &path) {
    return std::filesystem::exists(path);
}

Bytes FileIO::readBytes(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open " + path.string());
    const auto size = file.tellg();
    if (size < 0) throw std::runtime_error("cannot size " + path.string());
    Bytes data(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(data.data()), size))
        throw std::runtime_error("cannot read " + path.string());
    return data;
}

void FileIO::writeBytesAtomic(const std::filesystem::path &path,
                              ByteView data) {
    if (path.has_parent_path()) ensureDirectory(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
            throw std::runtime_error("cannot create " + temporary.string());
        file.write(reinterpret_cast<const char *>(data.data()),
                   std::streamsize(data.size()));
        file.flush();
        if (!file)
            throw std::runtime_error("cannot write " + temporary.string());
    }
    std::filesystem::rename(temporary, path);
}

void FileIO::writeJsonAtomic(const std::filesystem::path &path, const Json &j) {
    const auto text = j.dump(2);
    writeBytesAtomic(path,
                     ByteView(reinterpret_cast<const std::byte *>(text.data()),
                              text.size()));
}

void FileIO::ensureDirectory(const std::filesystem::path &path) {
    std::filesystem::create_directories(path);
}
