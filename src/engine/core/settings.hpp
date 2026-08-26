#pragma once

#include <string>
#include <unordered_map>
#include "serialization/serializable_api.hpp"
#include "signal.hpp"
#include "serialization/file_io.hpp"
#include "serialization/saveable.hpp"

struct Settings : public Saveable {
public:
    void setUiScale(float v);
    void setMasterVolume(float v);
    void setMusicVolume(float v);
    void setSoundVolume(float v);
    void setLanguage(const std::string& v);

    float getUiScale();
    float getMasterVolume();
    float getMusicVolume();
    float getSoundVolume();
    std::string getLanguage();

    template<typename T>
    T getExtraOr(const std::string& key, const T& defaultValue) const
    {
        auto it = extra.find(key);
        if (it == extra.end())
            return defaultValue;

        try {
            return it->second.get<T>();
        } catch (...) {
            return defaultValue;
        }
    }

    template<typename T>
    T getExtra(const std::string& key) const
    {
        auto it = extra.find(key);
        if (it == extra.end())
            throw std::runtime_error("Missing setting: " + key);

        return it->second.get<T>(); // will throw if wrong type
    }

    template<typename T>
    void setExtra(const std::string& key, const T& value)
    {
        auto it = extra.find(key);

        if (it != extra.end()) {
            try {
                if (it->second.get<T>() == value)
                    return;
            } catch (...) {}
        }

        extra[key] = value;
        onExtraChanged.emit(key, extra[key]);
    }

    Signal<float> onUiScaleChanged;
    Signal<float> onMasterVolumeChanged;
    Signal<float> onMusicVolumeChanged;
    Signal<float> onSoundVolumeChanged;
    Signal<const std::string&> onLanguageChanged;

    Signal<const std::string&, const Json&> onExtraChanged;

    Json serialize() const override;
    void deserialize(const Json& j) override;

private:
    // Core settings (fast, type-safe)
    int saveVersion = 1;
    float uiScale = 1.0f;
    float masterVolume = 50.0f;
    float musicVolume = 20.0f;
    float soundVolume = 20.0f;
    std::string language = "en_US";

    // Extensible settings
    std::unordered_map<std::string, Json> extra;
};