#pragma once
#ifndef VOLCANO_ENGINE_PASSES_SHADOW_SETTINGS_H
#define VOLCANO_ENGINE_PASSES_SHADOW_SETTINGS_H

#include <algorithm>
#include <cstdint>
#include <variant>
#include "../../../Config.hpp"

namespace Volcano::Engine::Passes {

// Which lights cast shadows. Exclusive levels, each including the one before.
enum class ShadowLightSources : uint8_t {
    SUN_ONLY = 0,
    SUN_AND_BLOCKS = 1,
    EVERYTHING = 2, // Including entities that glow or carry light.
};

// Shadow map side length; the enum value is a stable id, not the size.
enum class ShadowMapResolution : uint8_t {
    SMR512x_LOW = 0,
    SMR1024x_MED = 1,
    SMR2048x_HIGH = 2,
    SMR4096x_ULTRA = 3,
};

// Only the forward path exists. The type is deprecated up front so nothing
// builds on it; the setting is read as a plain byte.
enum class [[deprecated("Only forward rendering exists; deferred is not planned.")]] ShadowRenderPath : uint8_t {
    FORWARD = 0,
    DEFERRED = 1,
};

// When should we recompute shadows?
enum class ShadowRecomputeEvents : uint8_t {
    REALTIME = 0,     // Per-frame.
    CLIENT_TICK = 1,  // Per client tick; fine if the server lags.
    SERVER_TICK = 2   // Per server tick; fastest, only updates with the server.
};

// Graphics.Shadows.* - read fresh each frame so changes apply live. Off by default.
struct ShadowSettings {
    bool enabled = false;
    ShadowLightSources lightSources = ShadowLightSources::SUN_ONLY;
    ShadowMapResolution resolution = ShadowMapResolution::SMR1024x_MED;
    ShadowRecomputeEvents recomputeEvent = ShadowRecomputeEvents::REALTIME;
    uint16_t distance = 128;  // Blocks from the camera that receive shadows.
    uint8_t renderPath = 0;   // ShadowRenderPath's value; see its deprecation.
    bool debugView = false;   // Tints shadowed terrain magenta and out-of-map terrain blue, and logs caster counts.

    uint32_t MapSize() const
    {
        return 512u << static_cast<uint32_t>(resolution);
    }

    static ShadowSettings Load(Config& config)
    {
        auto number = [&](const char* key, uint32_t fallback, uint32_t maximum)
        {
            return std::min(std::get<uint32_t>(config.Get(key, fallback)), maximum);
        };

        ShadowSettings settings;
        settings.enabled = std::get<bool>(config.Get("Graphics.Shadows.Enabled", true));
        settings.lightSources = static_cast<ShadowLightSources>(number("Graphics.Shadows.LightSources", 0, 2));
        settings.resolution = static_cast<ShadowMapResolution>(number("Graphics.Shadows.Resolution", 1, 3));
        settings.recomputeEvent = static_cast<ShadowRecomputeEvents>(number("Graphics.Shadows.RecomputeOn", 0, 2));
        settings.distance = static_cast<uint16_t>(std::max(16u, number("Graphics.Shadows.Distance", 128, 1024)));
        settings.renderPath = static_cast<uint8_t>(number("Graphics.Shadows.RenderPath", 0, 1));
        settings.debugView = std::get<bool>(config.Get("Graphics.Shadows.DebugView", false));
        return settings;
    }
};

} // namespace Volcano::Engine::Passes

#endif
