#pragma once

#include <miniaudio.h>
#include <list>

#include "audio/audio_system_api.hpp"
#include "resource_manager.hpp"

struct SoundInstance {
    ma_audio_buffer_ref bufferRef;
    ma_sound sound;
};

class AudioSystem : public IAudioSystem {
   public:
    AudioSystem(ResourceManager& rm);

    void playSound(const std::string& key);
    void playSound(const std::string& key, float pitch);
    void playSoundAt(const std::string& key, Vec3 pos, float distance,
                     float attenuation);
    void playMusic(const std::string& key, bool loop);
    void stopMusic();
    void pauseMusic();
    void resumeMusic();
    void setListenerPosition(Vec3 pos);
    void update();

    void setMasterVolume(float volume);
    void setMusicVolume(float volume);
    void setSoundVolume(float volume);

    bool isMusicPlaying() const;

    float getMasterVolume() const;
    float getMusicVolume() const;
    float getSoundVolume() const;

    float computeSoundVolume() const;
    float computeMusicVolume() const;

    void close();

   private:
    ResourceManager& rm;
    float masterVolume = 100.0f;
    float soundVolume = 100.0f;
    float musicVolume = 100.0f;

    std::list<SoundInstance> activeSounds;
    ma_sound music;
    bool musicLoaded = false;

    ma_uint64 musicCursor = 0;
    bool musicPaused = false;

    ma_engine engine;

};