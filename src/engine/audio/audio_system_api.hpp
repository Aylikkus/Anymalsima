#pragma once
#include <cstdint>
#include <string>
#include "core/geometry.hpp"

class IAudioSystem {
public:
    virtual void playSound(const std::string& key) = 0;
    virtual void playSound(const std::string& key, float pitch) = 0;
    virtual void playSoundAt(const std::string& key, Vec3 pos, float distance, float attenuation) = 0;

    virtual void playMusic(const std::string& key, bool loop) = 0;
    virtual void stopMusic() = 0;
    virtual void pauseMusic() = 0;
    virtual void resumeMusic() = 0;

    virtual bool isMusicPlaying() const = 0;

    virtual void setMasterVolume(float volume) = 0;
    virtual void setMusicVolume(float volume) = 0;
    virtual void setSoundVolume(float volume) = 0;

    virtual float getMasterVolume() const = 0;
    virtual float getMusicVolume() const = 0;
    virtual float getSoundVolume() const = 0;

    virtual void setListenerPosition(Vec3 pos) = 0;

    virtual ~IAudioSystem() = default;
};