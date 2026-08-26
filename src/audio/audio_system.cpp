#include "audio_system.hpp"

AudioSystem::AudioSystem(ResourceManager& rm) : rm(rm) {
    ma_engine_config config = ma_engine_config_init();
    config.channels = 2;
    config.sampleRate = 48000;

    if (ma_engine_init(&config, &engine) != MA_SUCCESS) {
        throw std::runtime_error("Can't init miniaudio engine");
    }
}

void AudioSystem::playSound(const std::string& key) {
    const SoundData& sd = rm.getSoundData(key);

    activeSounds.emplace_back();
    SoundInstance& inst = activeSounds.back();

    // Each instance gets its own buffer ref with independent cursor
    if (ma_audio_buffer_ref_init(sd.format, sd.channels, sd.pData,
                                 sd.frameCount,
                                 &inst.bufferRef) != MA_SUCCESS) {
        activeSounds.pop_back();
        return;
    }

    ma_result res = ma_sound_init_from_data_source(&engine, &inst.bufferRef, 0,
                                                   nullptr, &inst.sound);
    if (res != MA_SUCCESS) {
        ma_audio_buffer_ref_uninit(&inst.bufferRef);
        activeSounds.pop_back();
        return;
    }

    ma_sound_set_volume(&inst.sound, computeSoundVolume() / 100.0f);
    ma_sound_start(&inst.sound);
}

void AudioSystem::playSoundAt(const std::string& key, Vec3 pos, float distance,
                              float attenuation) {
    const SoundData& sd = rm.getSoundData(key);

    activeSounds.emplace_back();
    SoundInstance& inst = activeSounds.back();

    // Each instance gets its own buffer ref with independent cursor
    if (ma_audio_buffer_ref_init(sd.format, sd.channels, sd.pData,
                                 sd.frameCount,
                                 &inst.bufferRef) != MA_SUCCESS) {
        activeSounds.pop_back();
        return;
    }

    ma_result res = ma_sound_init_from_data_source(&engine, &inst.bufferRef, 0,
                                                   nullptr, &inst.sound);
    if (res != MA_SUCCESS) {
        ma_audio_buffer_ref_uninit(&inst.bufferRef);
        activeSounds.pop_back();
        return;
    }

    ma_sound_set_position(&inst.sound, pos.x, pos.y, pos.z);
    ma_sound_set_min_distance(&inst.sound, distance);
    ma_sound_set_attenuation_model(&inst.sound, ma_attenuation_model_inverse);

    ma_sound_set_volume(&inst.sound, computeSoundVolume() / 100.0f);
    ma_sound_start(&inst.sound);
}

void AudioSystem::playSound(const std::string& key, float pitch) {
    const SoundData& sd = rm.getSoundData(key);

    activeSounds.emplace_back();
    SoundInstance& inst = activeSounds.back();

    // Each instance gets its own buffer ref with independent cursor
    if (ma_audio_buffer_ref_init(sd.format, sd.channels, sd.pData,
                                 sd.frameCount,
                                 &inst.bufferRef) != MA_SUCCESS) {
        activeSounds.pop_back();
        return;
    }

    ma_result res = ma_sound_init_from_data_source(&engine, &inst.bufferRef, 0,
                                                   nullptr, &inst.sound);
    if (res != MA_SUCCESS) {
        ma_audio_buffer_ref_uninit(&inst.bufferRef);
        activeSounds.pop_back();
        return;
    }

    ma_sound_set_volume(&inst.sound, computeSoundVolume() / 100.0f);
    ma_sound_set_pitch(&inst.sound, pitch);
    ma_sound_start(&inst.sound);
}

void AudioSystem::playMusic(const std::string& key, bool loop) {
    if (musicLoaded) {
        ma_sound_uninit(&music);
    }

    const std::string& path = rm.getMusicPath(key);

    if (ma_sound_init_from_file(&engine, path.c_str(), MA_SOUND_FLAG_STREAM,
                                nullptr, nullptr, &music) != MA_SUCCESS)
        return;

    ma_sound_set_looping(&music, loop);
    ma_sound_set_volume(&music, computeMusicVolume() / 100.0f);
    ma_sound_start(&music);

    musicLoaded = true;
}

void AudioSystem::stopMusic() {
    if (musicLoaded) {
        ma_sound_stop(&music);
        musicPaused = false;
    }
}

void AudioSystem::pauseMusic() {
    if (musicLoaded && !musicPaused) {
        ma_sound_get_cursor_in_pcm_frames(&music, &musicCursor);
        ma_sound_stop(&music);
        musicPaused = true;
    }
}

void AudioSystem::resumeMusic() {
    if (musicLoaded && musicPaused) {
        ma_sound_seek_to_pcm_frame(&music, musicCursor);
        ma_sound_start(&music);
        musicPaused = false;
    }
}

void AudioSystem::setListenerPosition(Vec3 pos) {
    ma_engine_listener_set_position(&engine, 0, pos.x, pos.y, pos.z);
}

void AudioSystem::update() {
    activeSounds.remove_if([](SoundInstance& inst) {
        if (ma_sound_at_end(&inst.sound)) {
            ma_sound_uninit(&inst.sound);
            ma_audio_buffer_ref_uninit(&inst.bufferRef);
            return true;
        }
        return false;
    });
}

void AudioSystem::setMasterVolume(float volume) {
    masterVolume = std::clamp(volume, 0.f, 100.f);

    float sVol = computeSoundVolume() / 100.0f;
    float mVol = computeMusicVolume() / 100.0f;

    for (auto& s : activeSounds) {
        ma_sound_set_volume(&s.sound, sVol);
    }

    if (musicLoaded) {
        ma_sound_set_volume(&music, mVol);
    }
}

void AudioSystem::setMusicVolume(float volume) {
    musicVolume = std::clamp(volume, 0.f, 100.f);

    if (musicLoaded) {
        ma_sound_set_volume(&music, computeMusicVolume() / 100.0f);
    }
}

void AudioSystem::setSoundVolume(float volume) {
    soundVolume = std::clamp(volume, 0.f, 100.f);

    float sVol = computeSoundVolume() / 100.0f;

    for (auto& s : activeSounds) {
        ma_sound_set_volume(&s.sound, sVol);
    }
}

float AudioSystem::computeSoundVolume() const {
    return (masterVolume * soundVolume) / 100.f;
}

float AudioSystem::computeMusicVolume() const {
    return (masterVolume * musicVolume) / 100.f;
}

float AudioSystem::getMasterVolume() const { return masterVolume; }

float AudioSystem::getMusicVolume() const { return musicVolume; }

float AudioSystem::getSoundVolume() const { return soundVolume; }

bool AudioSystem::isMusicPlaying() const {
    if (!musicLoaded) return false;
    return ma_sound_is_playing(&music);
}

void AudioSystem::close() {
    ma_engine_stop(&engine);

    for (auto& s : activeSounds) {
        ma_sound_uninit(&s.sound);
        ma_audio_buffer_ref_uninit(&s.bufferRef);
    }
    activeSounds.clear();

    if (musicLoaded) {
        ma_sound_uninit(&music);
        musicLoaded = false;
    }

    ma_engine_uninit(&engine);
}
