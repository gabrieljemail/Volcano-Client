#pragma once
#ifndef VOLCANO_AUDIO_MANAGER_H
#define VOLCANO_AUDIO_MANAGER_H

#include <al.h>
#include <alc.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Volcano {

struct AudioClip {
    std::string registryName;
    std::string filePath;
    ALuint buffer = 0;
    ALuint source = 0;
};

// A sound about to play, handed to listeners first. They may change the
// volume, pitch or name (to swap one sound for another), or set `cancelled`
// to silence it. Every client sound (blocks, footsteps, entities, music, UI)
// goes through AudioManager::PlaySound, so this sees all of them.
struct SoundEvent {
    std::string name;   // Registry name, e.g. "minecraft:entity/pig/ambient1".
    float volume;
    float pitch;
    bool loop;
    bool cancelled = false;
};

using SoundListener = std::function<void(SoundEvent&)>;

class AudioManager {
public:
    AudioManager() = default;
    ~AudioManager();

    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    // Loads OpenAL at runtime and opens the default playback device. Failure
    // is non-fatal so machines without an OpenAL runtime can still run.
    bool Init();
    void Shutdown();
    bool IsInitialized() const { return context != nullptr; }

    AudioClip* LoadSound(const std::string& registryName, const std::string& filePath);
    // Same, from OGG bytes already in memory (label is used for logs and the clip's path).
    AudioClip* LoadSoundFromMemory(const std::string& registryName,
                                   const std::vector<uint8_t>& oggData, const std::string& label);
    bool PlaySound(const std::string& registryName, float volume = 1.0f,
                   float pitch = 1.0f, bool loop = false);
    void StopSound(const std::string& registryName = {});
    // Changes the gain of a sound that is already playing.
    void SetSoundVolume(const std::string& registryName, float volume);
    bool IsSoundPlaying(const std::string& registryName) const;

    // Listeners run on the thread that asked for the sound (render, tick or
    // network): keep them short. Returns an id for RemoveSoundListener.
    uint32_t AddSoundListener(SoundListener listener);
    bool RemoveSoundListener(uint32_t id);

private:
    AudioClip* UploadDecoded(const std::string& registryName, const std::string& filePath,
                             int channels, int sampleRate, int frameCount, short* samples);
    using OpenDeviceFn = decltype(&alcOpenDevice);
    using CreateContextFn = decltype(&alcCreateContext);
    using MakeContextCurrentFn = decltype(&alcMakeContextCurrent);
    using DestroyContextFn = decltype(&alcDestroyContext);
    using CloseDeviceFn = decltype(&alcCloseDevice);
    using GenBuffersFn = decltype(&alGenBuffers);
    using DeleteBuffersFn = decltype(&alDeleteBuffers);
    using BufferDataFn = decltype(&alBufferData);
    using GenSourcesFn = decltype(&alGenSources);
    using DeleteSourcesFn = decltype(&alDeleteSources);
    using SourceiFn = decltype(&alSourcei);
    using SourcefFn = decltype(&alSourcef);
    using SourcePlayFn = decltype(&alSourcePlay);
    using SourceStopFn = decltype(&alSourceStop);
    using GetSourceiFn = decltype(&alGetSourcei);
    using GetErrorFn = decltype(&alGetError);

    void ReleaseClip(AudioClip& clip);

    void* library = nullptr;
    ALCdevice* device = nullptr;
    ALCcontext* context = nullptr;
    OpenDeviceFn openDevice = nullptr;
    CreateContextFn createContext = nullptr;
    MakeContextCurrentFn makeContextCurrent = nullptr;
    DestroyContextFn destroyContext = nullptr;
    CloseDeviceFn closeDevice = nullptr;
    GenBuffersFn genBuffers = nullptr;
    DeleteBuffersFn deleteBuffers = nullptr;
    BufferDataFn bufferData = nullptr;
    GenSourcesFn genSources = nullptr;
    DeleteSourcesFn deleteSources = nullptr;
    SourceiFn sourcei = nullptr;
    SourcefFn sourcef = nullptr;
    SourcePlayFn sourcePlay = nullptr;
    SourceStopFn sourceStop = nullptr;
    GetSourceiFn getSourcei = nullptr;
    GetErrorFn getError = nullptr;
    std::unordered_map<std::string, AudioClip> loadedSounds;

    std::mutex soundListenersMutex; // Guards soundListeners and nextSoundListenerId; held only to edit or copy.
    std::vector<std::pair<uint32_t, SoundListener>> soundListeners;
    uint32_t nextSoundListenerId = 1;
};

} // namespace Volcano

#endif
