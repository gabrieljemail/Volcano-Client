#pragma once
#ifndef VOLCANO_AUDIO_MANAGER_H
#define VOLCANO_AUDIO_MANAGER_H

#include <al.h>
#include <alc.h>
#include <string>
#include <unordered_map>

namespace Volcano {

struct AudioClip {
    std::string registryName;
    std::string filePath;
    ALuint buffer = 0;
    ALuint source = 0;
};

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
    bool PlaySound(const std::string& registryName, float volume = 1.0f,
                   float pitch = 1.0f, bool loop = false);
    void StopSound(const std::string& registryName = {});
    bool IsSoundPlaying(const std::string& registryName) const;

private:
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
};

} // namespace Volcano

#endif
