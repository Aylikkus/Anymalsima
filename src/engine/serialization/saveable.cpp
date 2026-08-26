#include "saveable.hpp"

void Saveable::loadIfExists(const std::string &path, bool create)
{
    if (!FileIO::fileExists(path)) {
        if (!create)
            return;
        save(path);
    }

    deserialize(FileIO::readJson(path));
}

void Saveable::load(const std::string &path)
{
    deserialize(FileIO::readJson(path));
}

void Saveable::save(const std::string &path)
{
    FileIO::writeJson(path, serialize());
}
