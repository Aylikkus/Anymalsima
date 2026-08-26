#include "file_io.hpp"

#include <fstream>

Json FileIO::readJson(const std::string &path)
{
    std::ifstream f(path);
    return Json::parse(f);
}

void FileIO::writeJson(const std::string &path, const Json &j)
{
    std::ofstream file(path);
    file << std::setw(2) << j;
}

bool FileIO::fileExists(const std::string &path)
{
    return std::filesystem::exists(path);
}
