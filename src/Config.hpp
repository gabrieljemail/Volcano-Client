#pragma once
#ifndef VOLCANO_CONFIG_H
#define VOLCANO_CONFIG_H

#include <cstdint>
#include <string>
#include <variant>
#include <nlohmann/json.hpp>

namespace Volcano {

// Every type a setting can hold. uint32_t covers all of the client's
// numeric settings (widths, distances, counts, ports, ...) so callers don't
// juggle int/float/uint16_t variants of the same lookup.
using ConfigValue = std::variant<std::string, uint32_t, bool>;

// Key-value settings store backed by settings.json, kept next to the
// running executable. A key is a dot-separated path — "Graphics.RenderDistance"
// — that maps onto nested JSON objects on disk (Graphics: { RenderDistance:
// ... }), so settings.json stays a clean, structured tree for a person to
// read/edit while the rest of the client can still get/set any setting
// through a single string.
class Config {
public:
    // Loads settings.json from next to the executable. Missing or
    // unparsable settings.json just starts Get() from an empty store —
    // there's nothing to default up front; each key gets its default (and
    // settings.json gets that key) the first time something calls Get() for
    // it. Also registers this instance as Active() — exactly one Config
    // exists for the process.
    void Load();

    // Writes the current settings back to settings.json.
    void Save() const;

    // Returns the value stored at key, provided it's present and holds the
    // same alternative as fallback. Otherwise — key missing, or holding
    // some other type (e.g. a hand-edited settings.json with the wrong
    // shape) — fallback is written to key and returned, so the first Get()
    // for a key is also what defines and persists its default.
    ConfigValue Get(const std::string& key, const ConfigValue& fallback);

    // Writes value to key immediately (e.g. from a SettingsUpdateEvent).
    void Set(const std::string& key, const ConfigValue& value);

    // Global accessor for code with no GlobalState pointer at hand (e.g.
    // Logger's free functions). Null until GlobalState::LoadSettings runs.
    static Config* Active() { return activeInstance; }

private:
    static const nlohmann::json* Find(const nlohmann::json& root, const std::string& key);
    static void Assign(nlohmann::json& root, const std::string& key, nlohmann::json value);

    nlohmann::json data = nlohmann::json::object();
    std::string path;

    static inline Config* activeInstance = nullptr;
};

} // namespace Volcano

#endif
