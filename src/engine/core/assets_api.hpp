#pragma once

#include <bgfx/bgfx.h>

#include <limits>
#include <stdexcept>
#include <string>

#include "core/geometry.hpp"
#include "serialization/data_storage_api.hpp"
#include "serialization/serializable_api.hpp"

class IAssets {
   public:
    virtual Vec2u getTextureSize(const std::string& key) = 0;

    virtual uint16_t getMaterialAlbedoLayer(
        const std::string& materialId) const = 0;
    virtual uint16_t getMaterialNormalLayer(
        const std::string& materialId) const = 0;

    virtual bgfx::TextureHandle getMaterialAlbedoArray() const = 0;
    virtual bgfx::TextureHandle getMaterialNormalArray() const = 0;

    template <typename T>
    void registerDataType();

    template <typename T>
    bool hasStorage() const {
        return dataStorages.contains(T::TypeName);
    }

    template <typename T>
    const DataStorage<T>& getStorage() const;

   protected:
    std::unordered_map<std::string, std::unique_ptr<IDataStorage>> dataStorages;
    using LoaderFunc = std::function<void(IAssets&, const Json&)>;
    std::unordered_map<std::string, LoaderFunc> loaders;
};

template <typename T>
void IAssets::registerDataType() {
    const std::string& typeName = T::TypeName;
    T::counter = 1;
    dataStorages[typeName] = std::make_unique<DataStorage<T>>();

    loaders[typeName] = [typeName](IAssets& rm, const Json& j) {
        auto& storage =
            static_cast<DataStorage<T>&>(*rm.dataStorages[typeName]);

        if (!j.is_array())
            throw std::runtime_error(typeName + ": expected data array");
        for (const auto& entry : j) {
            T item = entry.get<T>();  // requires from_json
            if (storage.indexByName.contains(item.name))
                throw std::runtime_error(typeName + ": duplicate name " +
                                         item.name);
            if (storage.items.size() >=
                std::numeric_limits<decltype(item.id)>::max())
                throw std::runtime_error(typeName + ": ID capacity exceeded");
            item.id = static_cast<decltype(item.id)>(storage.items.size() + 1);
            T::counter =
                item.id == std::numeric_limits<decltype(item.id)>::max()
                    ? 0
                    : item.id + 1;
            storage.indexByName[item.name] = storage.items.size();
            storage.items.push_back(std::move(item));
        }
    };
}

template <typename T>
const DataStorage<T>& IAssets::getStorage() const {
    auto it = dataStorages.find(T::TypeName);
    return static_cast<const DataStorage<T>&>(*it->second);
}