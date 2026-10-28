#pragma once
#ifndef VOLCANO_MOD_LOADER_H
#define VOLCANO_MOD_LOADER_H

#include <string>
#include <unordered_map>
#include <vector>
#include "mods/VolcanoModAPI.h"

namespace Volcano {

struct ModFile {
    std::string filename;
    void* handle{nullptr};
    std::unordered_map<EventType, std::vector<VC_EventCallback_T>> eventHandlers;
    bool loaded{false};
};

class ModLoader {
public:
    ModLoader(GlobalState* state, const std::string& modsDir = "mods")
        : state(state), modsDir(modsDir)
    {}
    ~ModLoader();

    // Loads every mod in modsDir and runs its VC_InitMod. Call once.
    void Init();

    // Runs every loaded mod's handlers for the event, in load order.
    void Fire(EventType type);

    // Registration target for VC_EventHandler; only valid during Init().
    bool AddHandler(EventType type, VC_EventCallback_T callback);

    std::vector<std::string> GetLoadedMods() const;

private:
    GlobalState* state;
    std::string modsDir;
    std::vector<ModFile> mods;
    ModFile* initializing{nullptr};
};

} // namespace Volcano

#endif
