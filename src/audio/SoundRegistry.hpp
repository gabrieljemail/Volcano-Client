#pragma once
#ifndef VOLCANO_SOUND_REGISTRY_H
#define VOLCANO_SOUND_REGISTRY_H

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "../Logger.hpp"
#include "AudioManager.hpp"

namespace Volcano {

class SoundRegistry {
public:
    explicit SoundRegistry(std::string soundDir) : soundDir(std::move(soundDir)) {}

    // Returns the number of sounds found; a missing directory is not fatal.
    size_t IndexSounds()
    {
        namespace fs = std::filesystem;
        files.clear();

        std::error_code error;
        if (!fs::is_directory(soundDir, error))
        {
            Log::Error("Sounds directory does not exist: " + soundDir);
            return 0;
        }

        for (const auto& entry : fs::recursive_directory_iterator(soundDir, error))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".ogg") continue;
            fs::path relative = fs::relative(entry.path(), soundDir).replace_extension();
            files["minecraft:" + relative.generic_string()] = entry.path().string();
        }

        Log::Info("Indexed " + std::to_string(files.size()) + " sounds.");
        return files.size();
    }

    bool Has(const std::string& name) const { return files.find(name) != files.end(); }

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
        if (loaded.count(name) != 0) return true;

        auto it = files.find(name);
        if (it == files.end())
        {
            Log::Error("Unknown sound: " + name);
            return false;
        }
        if (audioManager->LoadSound(name, it->second) == nullptr) return false;

        loaded.insert(name);
        return true;
    }

    // Loads the sound on first use, then plays it.
    bool Play(AudioManager* audioManager, const std::string& name, float volume = 1.0f,
              float pitch = 1.0f, bool loop = false)
    {
        return Load(audioManager, name) && audioManager->PlaySound(name, volume, pitch, loop);
    }

private:
    std::string soundDir;
    std::unordered_map<std::string, std::string> files;
    std::unordered_set<std::string> loaded;
};

} // namespace Volcano

#endif
