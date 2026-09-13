#pragma once
#ifndef VOLCANO_CONFIG_H
#define VOLCANO_CONFIG_H

#include <string>
#include <nlohmann/json.hpp>

namespace Volcano {

// Key-value settings store backed by settings.json, kept next to the
// running executable. A key is a dot-separated path — "Graphics.RenderDistance"
// — that maps onto nested JSON objects on disk (Graphics: { RenderDistance:
// ... }), so settings.json stays a clean, structured tree for a person to
// read/edit while the rest of the client can still get/set any setting
// through a single string.
class Config {
public:
    // Loads settings.json from next to the executable, filling in any keys
    // missing from the file (new settings this build added, or a first run
    // with no file at all) with their defaults, then re-saves so the file on
    // disk always reflects the full current schema. Also registers this
    // instance as Active() — exactly one Config exists for the process.
    void Load();

    // Writes the current settings back to settings.json.
    void Save() const;

    template <typename T>
    T Get(const std::string& key, const T& fallback) const
    {
        const nlohmann::json* node = Find(data, key);
        if (node == nullptr) return fallback;
        try {
            return node->get<T>();
        } catch (const nlohmann::json::exception&) {
            return fallback;
        }
    }

    template <typename T>
    void Set(const std::string& key, const T& value)
    {
        Assign(data, key, nlohmann::json(value));
    }

    // Global accessor for code with no GlobalState pointer at hand (e.g.
    // Logger's free functions). Null until GlobalState::LoadSettings runs.
    static Config* Active() { return activeInstance; }

private:
    static const nlohmann::json* Find(const nlohmann::json& root, const std::string& key);
    static void Assign(nlohmann::json& root, const std::string& key, nlohmann::json value);
    void ApplyDefaults();

    nlohmann::json data = nlohmann::json::object();
    std::string path;

    static inline Config* activeInstance = nullptr;
};

} // namespace Volcano

#endif
