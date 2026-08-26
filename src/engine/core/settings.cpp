#include "settings.hpp"
#include <iostream>

void Settings::setUiScale(float v)
{
    if (v == uiScale) return;
    uiScale = std::clamp(v, 0.001f, 10.0f);
    onUiScaleChanged.emit(uiScale);
}

void Settings::setMasterVolume(float v)
{
    if (v == masterVolume) return;
    masterVolume = std::clamp(v, 0.0f, 100.0f);
    onMasterVolumeChanged.emit(masterVolume);
}

void Settings::setMusicVolume(float v)
{
    if (v == musicVolume) return;
    musicVolume = std::clamp(v, 0.0f, 100.0f);
    onMusicVolumeChanged.emit(musicVolume);
}

void Settings::setSoundVolume(float v)
{
    if (v == soundVolume) return;
    soundVolume = std::clamp(v, 0.0f, 100.0f);
    onSoundVolumeChanged.emit(soundVolume);
}

void Settings::setLanguage(const std::string& v)
{
    if (v == language) return;
    language = v;
    onLanguageChanged.emit(language);
}

float Settings::getUiScale()
{
    return uiScale;
}

float Settings::getMasterVolume()
{
    return masterVolume;
}

float Settings::getMusicVolume()
{
    return musicVolume;
}

float Settings::getSoundVolume()
{
    return soundVolume;
}

std::string Settings::getLanguage()
{
    return language;
}

Json Settings::serialize() const
{
    Json j;

    // core fields
    j["saveVersion"] = saveVersion;
    j["uiScale"] = uiScale;
    j["masterVolume"] = masterVolume;
    j["musicVolume"] = musicVolume;
    j["soundVolume"] = soundVolume;
    j["language"] = language;

    Json extra_obj = Json::object();

    // dynamic fields
    for (const auto &[key, value] : extra)
    {
        extra_obj[key] = value;
    }

    j["extra"] = extra_obj;

    return j;
}

void Settings::deserialize(const Json &j)
{
    // core fields (safe reads)
    if (j.contains("saveVersion"))
        saveVersion = j["saveVersion"].get<int>();
    if (j.contains("uiScale"))
        uiScale = j["uiScale"].get<float>();
    if (j.contains("masterVolume"))
        masterVolume = j["masterVolume"].get<float>();
    if (j.contains("musicVolume"))
        musicVolume = j["musicVolume"].get<float>();
    if (j.contains("soundVolume"))
        soundVolume = j["soundVolume"].get<float>();
    if (j.contains("language"))
        language = j["language"].get<std::string>();

    onUiScaleChanged.emit(uiScale);
    onMasterVolumeChanged.emit(masterVolume);
    onMusicVolumeChanged.emit(musicVolume);
    onSoundVolumeChanged.emit(soundVolume);
    onLanguageChanged.emit(language);
    
    // capture unknown fields
    for (auto &[key, value] : j["extra"].items())
    {
        extra[key] = value;
        onExtraChanged.emit(key, extra[key]);
    }
}