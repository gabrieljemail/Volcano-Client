#pragma once
#ifndef VOLCANO_MODS_MOD_API_H
#define VOLCANO_MODS_MOD_API_H

#include <stdint.h>

// Mods are DLLs (Windows) or shared objects (Linux) dropped into mods/. There
// are two ways in, and a mod can use either or both:
//
//  - C++ mods link against the client's own classes (every Volcano:: function
//    is exported; see CMakeLists.txt) and receive the GlobalState in
//    VC_InitMod. They can also detour any exported function with MinHook.
//  - Anything else (other languages, integrations) uses only the C functions
//    below: VC_EventHandler / VC_EventHandlerEx.
//
// A mod exports VC_InitMod, and optionally VC_GetModInfo (so the client can
// check it was built for a compatible API) and VC_ShutdownMod. See sdk/.

#if defined(_WIN32)
    #define VC_MOD_EXPORT extern "C" __declspec(dllexport)
    #ifdef VOLCANO_CLIENT_BUILD
        #define VC_HOST_API extern "C" __declspec(dllexport)
        #define VC_API __declspec(dllexport)
    #else
        #define VC_HOST_API extern "C" __declspec(dllimport)
        #define VC_API __declspec(dllimport)
    #endif
#else
    #define VC_MOD_EXPORT extern "C" __attribute__((visibility("default")))
    #define VC_HOST_API extern "C" __attribute__((visibility("default")))
    #define VC_API __attribute__((visibility("default")))
#endif

// Bumped on a breaking change (the major) or an addition (the minor).
// Changing GlobalState's layout is breaking: C++ mods read its fields directly.
#define VC_API_VERSION_MAJOR 2
#define VC_API_VERSION_MINOR 0

namespace Volcano {

struct GlobalState;

// Values are the eventID passed to VC_EventHandler. New events are only ever
// appended. Threads: everything fires on the main thread except
// ENTER_CONFIG_STATE / ENTER_PLAY_STATE / CHAT_RECEIVED (network
// thread), BLOCK_CHANGED (meshing thread), TICK (tick thread) and FRAME_BEGIN / HUD_RENDER / KEY_PRESSED
// (render thread; HUD_RENDER is inside the open ImGui frame).
enum class EventType : uint8_t {
    PRE_START,          // Before the window and renderEngine exist.
    SPLASH_SCREEN,
    TEXTURES_LOADED,
    AUDIO_LOADED,
    RENDER_START,
    ENTER_CONFIG_STATE,
    ENTER_PLAY_STATE,
    CHUNKS_READY,
    LEAVE_PLAY_STATE,
    PRE_QUIT,
    FRAME_BEGIN,        // Start of every rendered frame.
    TICK,               // Every client tick (20 per second).
    CHAT_RECEIVED,      // data->text is the plain text of the message.
    KEY_PRESSED,        // data->a is the GLFW key code.
    BLOCK_CHANGED,      // data->a, b, c are the block position; data->d the new block's visual id (BlockRegistry).
    HUD_RENDER,         // Draw ImGui here; drawn after the vanilla HUD.
    COUNT
};

} // namespace Volcano

// Event payload; fields not listed for an event are zero/null. Valid only
// during the callback.
struct VC_EventData {
    uint32_t size;      // sizeof(VC_EventData) in the client that filled it in.
    const char* text;   // Event-specific string, or null.
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
};

typedef void (*VC_EventCallback_T)(Volcano::GlobalState* game);
typedef void (*VC_EventCallbackEx_T)(Volcano::GlobalState* game, const VC_EventData* data, void* userData);
typedef void (*VC_InitMod_T)(Volcano::GlobalState* game);
typedef void (*VC_ShutdownMod_T)(Volcano::GlobalState* game);

// What a mod says about itself. Return a pointer to a static instance from
// VC_GetModInfo; the strings must stay valid until VC_ShutdownMod.
struct VC_ModInfo {
    uint32_t apiMajor;  // Pass VC_API_VERSION_MAJOR / MINOR from the headers the mod built with.
    uint32_t apiMinor;
    const char* id;     // Short unique id, e.g. "hello_mod".
    const char* name;   // Display name.
    const char* version;
};
typedef const VC_ModInfo* (*VC_GetModInfo_T)();

// Client-provided. Registers callback for eventID (a Volcano::EventType) on
// behalf of the mod being initialized. Only valid inside VC_InitMod; returns
// false for an unknown event or a call made outside initialization.
VC_HOST_API bool VC_EventHandler(uint8_t eventID, VC_EventCallback_T callback);

// Like VC_EventHandler, but the callback gets the event payload and the
// userData given here (so it can be a bound object, a script handle...). Returns
// a handle for VC_UnregisterEventHandler, or 0 on failure. Valid inside
// VC_InitMod only.
VC_HOST_API uint32_t VC_EventHandlerEx(uint8_t eventID, VC_EventCallbackEx_T callback, void* userData);

// Removes a handler registered with VC_EventHandlerEx. Safe from any thread,
// including from inside the handler itself.
VC_HOST_API bool VC_UnregisterEventHandler(uint32_t handle);

// Mod-provided, e.g.:
//   VC_MOD_EXPORT void VC_InitMod(Volcano::GlobalState* game) { ... }
//   VC_MOD_EXPORT const VC_ModInfo* VC_GetModInfo() { static VC_ModInfo info{...}; return &info; }
//   VC_MOD_EXPORT void VC_ShutdownMod(Volcano::GlobalState* game) { ... }

#endif
