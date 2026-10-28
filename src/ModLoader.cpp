#define VOLCANO_CLIENT_BUILD
#include "ModLoader.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include "Logger.hpp"
#include "platform/OS.hpp"

namespace Volcano {

using Platform::OS;

// VC_EventHandler is a plain C symbol with no instance in hand, so it reaches
// the loader through this; set only while a mod's VC_InitMod runs.
static ModLoader* initializingLoader = nullptr;

ModLoader::~ModLoader()
{
    for (ModFile& mod : mods)
    {
        if (mod.handle != nullptr) OS::UnloadObject(mod.handle);
    }
}

void ModLoader::Init()
{
    namespace fs = std::filesystem;

    std::error_code error;
    fs::create_directories(modsDir, error);

    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(modsDir, error))
    {
        if (entry.is_regular_file() && entry.path().extension() == OS::GetObjectExtension())
        {
            candidates.push_back(entry.path());
        }
    }
    std::sort(candidates.begin(), candidates.end());

    // Reserved so pointers to entries stay valid while initializing.
    mods.reserve(candidates.size());

    for (const fs::path& path : candidates)
    {
        mods.push_back(ModFile{ path.filename().string() });
        ModFile& mod = mods.back();

        mod.handle = OS::LoadObject(path.string());
        if (mod.handle == nullptr)
        {
            Log::Error("Failed to load mod " + mod.filename + "!");
            continue;
        }

        auto initMod = reinterpret_cast<VC_InitMod_T>(OS::GetObjectSymbol(mod.handle, "VC_InitMod"));
        if (initMod == nullptr)
        {
            Log::Error("Mod " + mod.filename + " has no VC_InitMod export!");
            OS::UnloadObject(mod.handle);
            mod.handle = nullptr;
            continue;
        }

        initializing = &mod;
        initializingLoader = this;
        try
        {
            initMod(state);
            mod.loaded = true;
        }
        catch (const std::exception& e)
        {
            Log::Error("Mod " + mod.filename + " threw in VC_InitMod: " + e.what());
        }
        initializing = nullptr;
        initializingLoader = nullptr;

        if (!mod.loaded)
        {
            mod.eventHandlers.clear();
            OS::UnloadObject(mod.handle);
            mod.handle = nullptr;
        }
    }

    Log::Info("Mod summary:");
    for (const ModFile& mod : mods)
    {
        const char* mark = mod.loaded ? "\x1b[32m[+]" : "\x1b[31m[x]";
        Log::Info(std::string("  ") + mark + " " + mod.filename + "\x1b[0m");
    }
}

void ModLoader::Fire(EventType type)
{
    for (ModFile& mod : mods)
    {
        if (!mod.loaded) continue;

        auto it = mod.eventHandlers.find(type);
        if (it == mod.eventHandlers.end()) continue;

        for (VC_EventCallback_T callback : it->second)
        {
            try
            {
                callback(state);
            }
            catch (const std::exception& e)
            {
                Log::Error("Mod " + mod.filename + " threw in an event handler: " + e.what());
            }
        }
    }
}

bool ModLoader::AddHandler(EventType type, VC_EventCallback_T callback)
{
    if (initializing == nullptr || callback == nullptr) return false;
    if (type >= EventType::COUNT) return false;

    initializing->eventHandlers[type].push_back(callback);
    return true;
}

std::vector<std::string> ModLoader::GetLoadedMods() const
{
    std::vector<std::string> names;
    for (const ModFile& mod : mods)
    {
        if (mod.loaded) names.push_back(mod.filename);
    }
    return names;
}

} // namespace Volcano

bool VC_EventHandler(uint8_t eventID, VC_EventCallback_T callback)
{
    if (Volcano::initializingLoader == nullptr) return false;
    return Volcano::initializingLoader->AddHandler(static_cast<Volcano::EventType>(eventID), callback);
}
