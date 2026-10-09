#pragma once
#ifndef VOLCANO_MOD_LOADER_H
#define VOLCANO_MOD_LOADER_H

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <functional>
#include "platform/OS.hpp"
#include "GlobalState.hpp"

using fs = std::filesystem;

namespace Volcano {

enum class EventType : uint8_t {
    PRE_START,
    SPLASH_SCREEN,
    TEXTURES_LOADED,
    AUDIO_LOADED,
    RENDER_START,
    ENTER_CONFIG_STATE,
    ENTER_PLAY_STATE,
    CHUNKS_READY,
    LEAVE_PLAY_STATE,
    PRE_QUIT
};

enum class ModPlatformType : uint8_t {
    DYNAMIC_LINK_LIBRARY,
    SHARED_OBJECT
};

typedef std::function<void(GlobalState* game)> ModEventHandler;

struct ModFile {
    std::string filename;
    ModPlatformType type;
    std::unordered_map<EventType, ModEventHandler> eventHandlers;
    bool loaded = false;
};

class ModLoader {
public:
    ModLoader(const std::string& modsDir = "mods")
        : modsDir(modsDir)
    {}
    ~ModLoader() = default;

    void Init()
    {
        // Scan modsDir for mods.
        for (const auto& entry : fs::directory_iterator(fs::path(modsDir)))
        {
            bool loaded = OS::LoadObject(modsDir + entry.path().filename());
            mods.push_back(ModFile {
                entry.path().filename(),
                entry.path().extension() == ".dll" ? ModPlatformType::DYNAMIC_LINK_LIBRARY : ModPlatformType::SHARED_OBJECT,
                loaded
            });
        }
    }

    std::vector<std::string> GetLoadedMods()
    {
        std::vector<std::string> rt;
        rt.resize(mods);
        for (ModFile& entry : mods)
        {
            if (entry.loaded) rt.push_back(entry.filename);
        }
        return rt;
    }

private:
    const std::string& modsDir = "mods";
    std::vector<ModFile> mods;
};

} // namespace Volcano

#endif
