#pragma once
#ifndef VOLCANO_MODS_MOD_API_H
#define VOLCANO_MODS_MOD_API_H

#include <stdint.h>

// Mods are DLLs (Windows) or shared objects (Linux) dropped into mods/. They
// link against the client's own classes and export VC_InitMod, which the
// client calls once right after loading; from there a mod registers event
// handlers with VC_EventHandler.

#if defined(_WIN32)
    #define VC_MOD_EXPORT extern "C" __declspec(dllexport)
    #ifdef VOLCANO_CLIENT_BUILD
        #define VC_HOST_API extern "C" __declspec(dllexport)
    #else
        #define VC_HOST_API extern "C" __declspec(dllimport)
    #endif
#else
    #define VC_MOD_EXPORT extern "C" __attribute__((visibility("default")))
    #define VC_HOST_API extern "C" __attribute__((visibility("default")))
#endif

namespace Volcano {

struct GlobalState;

// Values are the eventID passed to VC_EventHandler.
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
    PRE_QUIT,
    COUNT
};

} // namespace Volcano

typedef void (*VC_EventCallback_T)(Volcano::GlobalState* game);
typedef void (*VC_InitMod_T)(Volcano::GlobalState* game);

// Client-provided. Registers callback for eventID (a Volcano::EventType) on
// behalf of the mod being initialized. Only valid inside VC_InitMod; returns
// false for an unknown event or a call made outside initialization.
VC_HOST_API bool VC_EventHandler(uint8_t eventID, VC_EventCallback_T callback);

// Mod-provided, e.g.:
//   VC_MOD_EXPORT void VC_InitMod(Volcano::GlobalState* game) { ... }

#endif
