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

// The loader mods unregister through: set in Init, cleared on destruction.
static ModLoader* activeLoader = nullptr;

ModLoader::~ModLoader()
{
    activeLoader = nullptr;

    // Latest-loaded first, so a mod that builds on an earlier one goes first.
    for (auto it = mods.rbegin(); it != mods.rend(); ++it)
    {
        ModFile& mod = *it;
        if (mod.loaded && mod.shutdown != nullptr)
        {
            try
            {
                mod.shutdown(state);
            }
            catch (...)
            {
                Log::Error("Mod " + mod.filename + " threw in VC_ShutdownMod.");
            }
        }
        if (mod.handle != nullptr) OS::UnloadObject(mod.handle);
    }
}

void ModLoader::Init()
{
    namespace fs = std::filesystem;
    activeLoader = this;

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

        // Optional: refuse a mod built for another API generation.
        auto getInfo = reinterpret_cast<VC_GetModInfo_T>(OS::GetObjectSymbol(mod.handle, "VC_GetModInfo"));
        const VC_ModInfo* info = getInfo != nullptr ? getInfo() : nullptr;
        if (info == nullptr)
        {
            Log::Info("Mod " + mod.filename + " has no VC_GetModInfo; loading it without a version check.");
        }
        else if (info->apiMajor != VC_API_VERSION_MAJOR || info->apiMinor > VC_API_VERSION_MINOR)
        {
            Log::Error("Mod " + mod.filename + " was built for mod API " + std::to_string(info->apiMajor) + "."
                + std::to_string(info->apiMinor) + " but this client provides " + std::to_string(VC_API_VERSION_MAJOR)
                + "." + std::to_string(VC_API_VERSION_MINOR) + "; skipped.");
            OS::UnloadObject(mod.handle);
            mod.handle = nullptr;
            continue;
        }
        mod.shutdown = reinterpret_cast<VC_ShutdownMod_T>(OS::GetObjectSymbol(mod.handle, "VC_ShutdownMod"));

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
        catch (...)
        {
            Log::Error("Mod " + mod.filename + " threw in VC_InitMod.");
        }
        initializing = nullptr;
        initializingLoader = nullptr;

        if (!mod.loaded)
        {
            std::lock_guard<std::mutex> lock(handlersMutex);
            for (const ModHandler& handler : mod.eventHandlers) handlerCounts[static_cast<size_t>(handler.type)]--;
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

void ModLoader::Fire(EventType type, const VC_EventData* data)
{
    if (!HasHandlers(type)) return;

    static const VC_EventData EMPTY{ sizeof(VC_EventData), nullptr, 0, 0, 0, 0 };
    if (data == nullptr) data = &EMPTY;

    // Copy the lists so a handler may unregister while the event runs.
    std::vector<std::pair<const ModFile*, ModHandler>> calls;
    {
        std::lock_guard<std::mutex> lock(handlersMutex);
        for (const ModFile& mod : mods)
        {
            if (!mod.loaded) continue;
            for (const ModHandler& handler : mod.eventHandlers)
            {
                if (handler.type == type) calls.emplace_back(&mod, handler);
            }
        }
    }

    for (const auto& [mod, handler] : calls)
    {
        try
        {
            if (handler.extended != nullptr) handler.extended(state, data, handler.userData);
            else handler.plain(state);
        }
        catch (const std::exception& e)
        {
            Log::Error("Mod " + mod->filename + " threw in an event handler: " + e.what());
        }
        catch (...)
        {
            Log::Error("Mod " + mod->filename + " threw in an event handler.");
        }
    }
}

uint32_t ModLoader::Register(ModHandler handler)
{
    if (initializing == nullptr) return 0;
    if (handler.type >= EventType::COUNT) return 0;

    std::lock_guard<std::mutex> lock(handlersMutex);
    handler.handle = nextHandle++;
    initializing->eventHandlers.push_back(handler);
    handlerCounts[static_cast<size_t>(handler.type)]++;
    return handler.handle;
}

uint32_t ModLoader::AddHandler(EventType type, VC_EventCallback_T callback)
{
    if (callback == nullptr) return 0;
    ModHandler handler;
    handler.type = type;
    handler.plain = callback;
    return Register(handler);
}

uint32_t ModLoader::AddHandler(EventType type, VC_EventCallbackEx_T callback, void* userData)
{
    if (callback == nullptr) return 0;
    ModHandler handler;
    handler.type = type;
    handler.extended = callback;
    handler.userData = userData;
    return Register(handler);
}

bool ModLoader::RemoveHandler(uint32_t handle)
{
    std::lock_guard<std::mutex> lock(handlersMutex);
    for (ModFile& mod : mods)
    {
        auto it = std::find_if(mod.eventHandlers.begin(), mod.eventHandlers.end(),
            [handle](const ModHandler& h) { return h.handle == handle; });
        if (it == mod.eventHandlers.end()) continue;

        handlerCounts[static_cast<size_t>(it->type)]--;
        mod.eventHandlers.erase(it);
        return true;
    }
    return false;
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
    return Volcano::initializingLoader->AddHandler(static_cast<Volcano::EventType>(eventID), callback) != 0;
}

uint32_t VC_EventHandlerEx(uint8_t eventID, VC_EventCallbackEx_T callback, void* userData)
{
    if (Volcano::initializingLoader == nullptr) return 0;
    return Volcano::initializingLoader->AddHandler(static_cast<Volcano::EventType>(eventID), callback, userData);
}

bool VC_UnregisterEventHandler(uint32_t handle)
{
    return Volcano::activeLoader != nullptr && Volcano::activeLoader->RemoveHandler(handle);
}
