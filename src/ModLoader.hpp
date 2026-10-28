#pragma once
#ifndef VOLCANO_MOD_LOADER_H
#define VOLCANO_MOD_LOADER_H

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "mods/VolcanoModAPI.h"

namespace Volcano {

// One registered callback; plain or extended (with payload and user data).
struct ModHandler {
    uint32_t handle{0};
    EventType type{EventType::COUNT};
    VC_EventCallback_T plain{nullptr};
    VC_EventCallbackEx_T extended{nullptr};
    void* userData{nullptr};
};

struct ModFile {
    std::string filename;
    void* handle{nullptr};
    std::vector<ModHandler> eventHandlers; // Guarded by ModLoader::handlersMutex.
    VC_ShutdownMod_T shutdown{nullptr};
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

    // Runs every loaded mod's handlers for the event, in load order. Callable
    // from any thread; a mod's exceptions are logged, never propagated. `data`
    // (optional) is handed to extended handlers.
    void Fire(EventType type, const VC_EventData* data = nullptr);

    // True if any mod listens for the event: lets hot call sites (every frame
    // or tick) skip building a payload.
    bool HasHandlers(EventType type) const
    {
        return handlerCounts[static_cast<size_t>(type)].load(std::memory_order_relaxed) > 0;
    }

    // Registration targets for VC_EventHandler(Ex); only valid during Init().
    // Return the handler's handle (0 on failure).
    uint32_t AddHandler(EventType type, VC_EventCallback_T callback);
    uint32_t AddHandler(EventType type, VC_EventCallbackEx_T callback, void* userData);

    // Removes a handler by handle, from any thread.
    bool RemoveHandler(uint32_t handle);

    std::vector<std::string> GetLoadedMods() const;

private:
    uint32_t Register(ModHandler handler);

    GlobalState* state;
    std::string modsDir;
    std::vector<ModFile> mods;
    ModFile* initializing{nullptr};
    uint32_t nextHandle{1};
    std::mutex handlersMutex; // Guards every ModFile::eventHandlers and nextHandle; held only to copy or edit lists.
    std::array<std::atomic<uint32_t>, static_cast<size_t>(EventType::COUNT)> handlerCounts{};
};

} // namespace Volcano

#endif
