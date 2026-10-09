#include "AudioManager.hpp"
#include "../Logger.hpp"
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#include <algorithm>
#include <cstdlib>
#include <limits>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace Volcano {

namespace {

void* LoadOpenALLibrary()
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryW(L"OpenAL32.dll"));
#else
    void* library = dlopen("libopenal.so.1", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) library = dlopen("libopenal.so", RTLD_NOW | RTLD_LOCAL);
    return library;
#endif
}

void UnloadOpenALLibrary(void* library)
{
    if (library == nullptr) return;
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

void* LoadOpenALSymbol(void* library, const char* name)
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

}

AudioManager::~AudioManager()
{
    Shutdown();
}

bool AudioManager::Init()
{
    if (IsInitialized()) return true;

    library = LoadOpenALLibrary();
    if (library == nullptr)
    {
        Log::Info("OpenAL runtime unavailable; audio is disabled.");
        return false;
    }

    openDevice = reinterpret_cast<OpenDeviceFn>(LoadOpenALSymbol(library, "alcOpenDevice"));
    createContext = reinterpret_cast<CreateContextFn>(LoadOpenALSymbol(library, "alcCreateContext"));
    makeContextCurrent = reinterpret_cast<MakeContextCurrentFn>(LoadOpenALSymbol(library, "alcMakeContextCurrent"));
    destroyContext = reinterpret_cast<DestroyContextFn>(LoadOpenALSymbol(library, "alcDestroyContext"));
    closeDevice = reinterpret_cast<CloseDeviceFn>(LoadOpenALSymbol(library, "alcCloseDevice"));
    genBuffers = reinterpret_cast<GenBuffersFn>(LoadOpenALSymbol(library, "alGenBuffers"));
    deleteBuffers = reinterpret_cast<DeleteBuffersFn>(LoadOpenALSymbol(library, "alDeleteBuffers"));
    bufferData = reinterpret_cast<BufferDataFn>(LoadOpenALSymbol(library, "alBufferData"));
    genSources = reinterpret_cast<GenSourcesFn>(LoadOpenALSymbol(library, "alGenSources"));
    deleteSources = reinterpret_cast<DeleteSourcesFn>(LoadOpenALSymbol(library, "alDeleteSources"));
    sourcei = reinterpret_cast<SourceiFn>(LoadOpenALSymbol(library, "alSourcei"));
    sourcef = reinterpret_cast<SourcefFn>(LoadOpenALSymbol(library, "alSourcef"));
    sourcePlay = reinterpret_cast<SourcePlayFn>(LoadOpenALSymbol(library, "alSourcePlay"));
    sourceStop = reinterpret_cast<SourceStopFn>(LoadOpenALSymbol(library, "alSourceStop"));
    getSourcei = reinterpret_cast<GetSourceiFn>(LoadOpenALSymbol(library, "alGetSourcei"));
    getError = reinterpret_cast<GetErrorFn>(LoadOpenALSymbol(library, "alGetError"));

    if (openDevice == nullptr || createContext == nullptr || makeContextCurrent == nullptr
        || destroyContext == nullptr || closeDevice == nullptr || genBuffers == nullptr
        || deleteBuffers == nullptr || bufferData == nullptr || genSources == nullptr
        || deleteSources == nullptr || sourcei == nullptr || sourcef == nullptr
        || sourcePlay == nullptr || sourceStop == nullptr || getSourcei == nullptr
        || getError == nullptr)
    {
        Log::Error("OpenAL runtime is missing required ALC entry points.");
        Shutdown();
        return false;
    }

    device = openDevice(nullptr);
    if (device == nullptr)
    {
        Log::Info("No OpenAL playback device is available; audio is disabled.");
        Shutdown();
        return false;
    }

    context = createContext(device, nullptr);
    if (context == nullptr || makeContextCurrent(context) == ALC_FALSE)
    {
        Log::Error("Failed to create or activate the OpenAL context.");
        Shutdown();
        return false;
    }

    Log::Info("OpenAL audio context initialized.");
    return true;
}

void AudioManager::Shutdown()
{
    if (context != nullptr)
    {
        for (auto& [name, clip] : loadedSounds) ReleaseClip(clip);
        loadedSounds.clear();
        if (makeContextCurrent != nullptr) makeContextCurrent(nullptr);
        if (destroyContext != nullptr) destroyContext(context);
        context = nullptr;
    }
    if (device != nullptr)
    {
        if (closeDevice != nullptr) closeDevice(device);
        device = nullptr;
    }

    openDevice = nullptr;
    createContext = nullptr;
    makeContextCurrent = nullptr;
    destroyContext = nullptr;
    closeDevice = nullptr;
    genBuffers = nullptr;
    deleteBuffers = nullptr;
    bufferData = nullptr;
    genSources = nullptr;
    deleteSources = nullptr;
    sourcei = nullptr;
    sourcef = nullptr;
    sourcePlay = nullptr;
    sourceStop = nullptr;
    getSourcei = nullptr;
    getError = nullptr;
    UnloadOpenALLibrary(library);
    library = nullptr;
}

AudioClip* AudioManager::LoadSound(const std::string& registryName, const std::string& filePath)
{
    if (!IsInitialized() || registryName.empty()) return nullptr;

    int channels = 0;
    int sampleRate = 0;
    short* samples = nullptr;
    int frameCount = stb_vorbis_decode_filename(filePath.c_str(), &channels, &sampleRate, &samples);
    if (frameCount <= 0 || samples == nullptr)
    {
        Log::Error("Failed to decode OGG sound: " + filePath);
        std::free(samples);
        return nullptr;
    }

    if ((channels != 1 && channels != 2) || sampleRate <= 0
        || static_cast<uint64_t>(frameCount) * static_cast<uint64_t>(channels) * sizeof(short)
            > static_cast<uint64_t>((std::numeric_limits<ALsizei>::max)()))
    {
        Log::Error("Unsupported OGG format for OpenAL (expected mono/stereo PCM): " + filePath);
        std::free(samples);
        return nullptr;
    }

    ALenum format = channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
    ALsizei dataSize = static_cast<ALsizei>(frameCount * channels * static_cast<int>(sizeof(short)));
    ALuint buffer = 0;
    ALuint source = 0;
    while (getError() != AL_NO_ERROR) {}
    genBuffers(1, &buffer);
    if (buffer != 0) bufferData(buffer, format, samples, dataSize, sampleRate);
    std::free(samples);

    if (buffer == 0 || getError() != AL_NO_ERROR)
    {
        if (buffer != 0) deleteBuffers(1, &buffer);
        Log::Error("Failed to upload decoded audio to OpenAL: " + filePath);
        return nullptr;
    }

    genSources(1, &source);
    if (source != 0)
    {
        sourcei(source, AL_BUFFER, static_cast<ALint>(buffer));
    }
    if (source == 0 || getError() != AL_NO_ERROR)
    {
        if (source != 0) deleteSources(1, &source);
        deleteBuffers(1, &buffer);
        Log::Error("Failed to create OpenAL source: " + filePath);
        return nullptr;
    }

    auto existing = loadedSounds.find(registryName);
    if (existing != loadedSounds.end())
    {
        ReleaseClip(existing->second);
        loadedSounds.erase(existing);
    }

    auto [it, inserted] = loadedSounds.emplace(registryName,
        AudioClip{registryName, filePath, buffer, source});
    return inserted ? &it->second : nullptr;
}

bool AudioManager::PlaySound(const std::string& registryName, float volume, float pitch, bool loop)
{
    auto it = loadedSounds.find(registryName);
    if (!IsInitialized() || it == loadedSounds.end()) return false;

    ALuint source = it->second.source;
    while (getError() != AL_NO_ERROR) {}
    sourceStop(source);
    sourcef(source, AL_GAIN, std::clamp(volume, 0.0f, 1.0f));
    sourcef(source, AL_PITCH, std::clamp(pitch, 0.01f, 4.0f));
    sourcei(source, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
    sourcePlay(source);
    if (getError() != AL_NO_ERROR)
    {
        Log::Error("Failed to play sound: " + registryName);
        return false;
    }
    return true;
}

void AudioManager::StopSound(const std::string& registryName)
{
    if (!IsInitialized()) return;
    if (registryName.empty())
    {
        for (auto& [name, clip] : loadedSounds) sourceStop(clip.source);
        return;
    }

    auto it = loadedSounds.find(registryName);
    if (it != loadedSounds.end()) sourceStop(it->second.source);
}

bool AudioManager::IsSoundPlaying(const std::string& registryName) const
{
    auto it = loadedSounds.find(registryName);
    if (!IsInitialized() || it == loadedSounds.end()) return false;

    ALint state = AL_STOPPED;
    getSourcei(it->second.source, AL_SOURCE_STATE, &state);
    return state == AL_PLAYING;
}

void AudioManager::ReleaseClip(AudioClip& clip)
{
    if (clip.source != 0 && deleteSources != nullptr) deleteSources(1, &clip.source);
    if (clip.buffer != 0 && deleteBuffers != nullptr) deleteBuffers(1, &clip.buffer);
    clip.source = 0;
    clip.buffer = 0;
}

} // namespace Volcano
