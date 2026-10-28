#pragma once
#ifndef VOLCANO_SOUND_REGISTRY_H
#define VOLCANO_SOUND_REGISTRY_H

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "../Logger.hpp"
#include "../resources/ResourceManager.hpp"
#include "AudioManager.hpp"

namespace Volcano {

class SoundRegistry {
public:
    explicit SoundRegistry(std::string soundDir) : soundDir(std::move(soundDir)) {}

    // Indexes the resource-pack sounds (logical paths) and returns how many
    // were found. Safe to call again after a resource reload: pack sounds are
    // re-indexed and reloaded on next use, mod Register() entries are kept.
    size_t IndexSounds()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        std::unordered_map<std::string, std::string> modFiles;
        for (const auto& [name, path] : files)
        {
            if (logicalNames.count(name) == 0) modFiles[name] = path;
        }
        files.clear();
        logicalNames.clear();
        loaded.clear(); // Load() replaces the old clip when it runs again.

        const std::string prefix = soundDir + "/";
        for (const std::string& logical : ResourceManager::Instance().List(soundDir, ".ogg", true))
        {
            if (!logical.starts_with(prefix)) continue;
            std::string relative = logical.substr(prefix.size(), logical.size() - prefix.size() - 4);
            std::string name = "minecraft:" + relative;
            files[name] = logical;
            logicalNames.insert(name);
        }

        for (const auto& [name, path] : modFiles) files[name] = path;

        if (files.empty()) Log::Error("No sounds found under " + soundDir);
        Log::Info("Indexed " + std::to_string(files.size()) + " sounds.");
        return files.size();
    }

    bool Has(const std::string& name) const { return files.find(name) != files.end(); }

    // Adds (or replaces) a sound under `name`, e.g. "mymod:ui/click" for an
    // .ogg file, so Play() can load it on first use. Replacing a name that was
    // already loaded takes effect after a restart.
    void Register(const std::string& name, const std::string& path)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        files[name] = path;
        logicalNames.erase(name);
    }

    // Finds a sound under `prefix` (e.g. "minecraft:music/") by bare file
    // name, so callers needn't know which subfolder it lives in.
    std::string FindByStem(const std::string& prefix, const std::string& stem) const
    {
        const std::string suffix = "/" + stem;
        for (const auto& [name, path] : files)
        {
            if (name.starts_with(prefix) && name.ends_with(suffix)) return name;
        }
        return {};
    }

    bool Load(AudioManager* audioManager, const std::string& name)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (loaded.count(name) != 0) return true;

        auto it = files.find(name);
        if (it == files.end())
        {
            Log::Error("Unknown sound: " + name);
            return false;
        }
        if (logicalNames.count(name) != 0)
        {
            auto bytes = ResourceManager::Instance().ReadBytes(it->second);
            if (!bytes || audioManager->LoadSoundFromMemory(name, *bytes, it->second) == nullptr)
            {
                Log::Error("Failed to load sound: " + it->second);
                return false;
            }
        }
        else if (audioManager->LoadSound(name, it->second) == nullptr) return false;

        loaded.insert(name);
        return true;
    }

    // Loads the sound on first use, then plays it.
    bool Play(AudioManager* audioManager, const std::string& name, float volume = 1.0f,
              float pitch = 1.0f, bool loop = false)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        return Load(audioManager, name) && audioManager->PlaySound(name, volume, pitch, loop);
    }

private:
    // Footsteps (tick thread), block sounds (main thread) and music share this.
    std::recursive_mutex mutex;
    std::string soundDir;
    std::unordered_map<std::string, std::string> files;
    // Names in files whose value is a ResourceManager logical path, not a disk path.
    std::unordered_set<std::string> logicalNames;
    std::unordered_set<std::string> loaded;
};

} // namespace Volcano

#endif
